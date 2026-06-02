package control

import "github.com/poohnet/sbse-controller/internal/config"

// These methods are wired into the SMA Modbus server as its semantic backend
// (the server itself is a pure protocol adapter). They mirror the firmware's
// on_modbus_* handlers in sbse_controller.cpp.

// OnOpMod latches a 40236 CmpBMS.OpMod write. Unknown values fall back to Default
// so a buggy client can't trap us in a force-mode (firmware on_modbus_op_mod_write).
func (c *Controller) OnOpMod(opMod uint32) error {
	c.mu.Lock()
	switch uint16(opMod) {
	case OpModForceCharge, OpModForceDischarge, OpModDefault:
		c.modbusOpMod = uint16(opMod)
	default:
		c.modbusOpMod = OpModDefault
	}
	c.modbusWriteCount++
	c.mu.Unlock()
	c.notifyState()
	return nil
}

// OnSetpoint applies a 40793..40802 setpoint sub-block write. The server has
// already validated alignment and range (firmware on_modbus_setpoint_write).
func (c *Controller) OnSetpoint(startAddr, regCount uint16, regs []uint16) error {
	c.mu.Lock()
	ac, changed := c.applyModbusSetpointPartialLocked(startAddr, regCount, regs)
	c.modbusWriteCount++
	c.mu.Unlock()
	if changed {
		c.notifyActive(ac)
	}
	c.notifyState()
	return nil
}

// SynthInputs feeds the proxy's read-time synthesis (grid/battery/SoC).
func (c *Controller) SynthInputs() (gridW, batteryW int32, socPct uint8) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.gridWRaw, c.batteryWRaw, c.socPct
}

// NoteRead bumps the proxy read counter (drives the dashboard read LED).
func (c *Controller) NoteRead() {
	c.mu.Lock()
	c.modbusReadCount++
	c.mu.Unlock()
	c.notifyState()
}

// applyModbusSetpointPartialLocked decodes the SMA setpoint fields present in the
// write and applies them per the configured authority (firmware
// apply_modbus_setpoint_partial). Returns the active config and whether it changed.
// Caller holds mu.
func (c *Controller) applyModbusSetpointPartialLocked(startAddr, regCount uint16, regs []uint16) (config.ActiveConfig, bool) {
	const setpAddrLo = 40793
	firstField := (startAddr - setpAddrLo) / 2
	fieldCount := regCount / 2

	var (
		hasChaMax, hasDchgMax, hasGridSpt bool
		batChaMax, batDchgMax             uint32
		gridSpt                           int32
	)
	for i := uint16(0); i < fieldCount; i++ {
		value := (uint32(regs[2*i]) << 16) | uint32(regs[2*i+1])
		switch firstField + i {
		case 0: // BatChaMinW -- ignored
		case 1:
			batChaMax = value
			hasChaMax = true
		case 2: // BatDchgMinW -- ignored
		case 3:
			batDchgMax = value
			hasDchgMax = true
		case 4:
			gridSpt = int32(value)
			hasGridSpt = true
		}
	}

	clampW := func(v uint32) uint32 {
		if v > 10000 {
			return 10000
		}
		return v
	}
	chaMaxC := clampW(batChaMax)
	dchgMaxC := clampW(batDchgMax)
	sptC := clampI32(gridSpt, -750, 2500)

	// Interpret the sticky OpMod. The fallback for an absent max field uses the
	// current cached cap (read before applyRuntimeFromActive below).
	switch c.modbusOpMod {
	case OpModForceCharge:
		mag := c.maxChargeW
		if hasChaMax {
			mag = int32(chaMaxC)
		}
		c.modbusForceW = -mag
	case OpModForceDischarge:
		mag := c.maxDischargeW
		if hasDchgMax {
			mag = int32(dchgMaxC)
		}
		c.modbusForceW = mag
	default:
		c.modbusForceW = 0
	}

	changed := false
	if c.modbusServerAuthority >= AuthorityCaps {
		if hasChaMax {
			c.active.MaxChargeW = chaMaxC
			changed = true
		}
		if hasDchgMax {
			c.active.MaxDischargeW = dchgMaxC
			changed = true
		}
	}
	if c.modbusServerAuthority >= AuthorityFull && hasGridSpt {
		c.active.GridChargeTargetW = sptC
		c.active.GridDischargeTargetW = sptC
		changed = true
	}
	if changed {
		c.applyRuntimeFromActive()
	}

	c.lastModbusWrite = c.now()
	c.modbusActive = true
	return c.active, changed
}

func clampI32(v, lo, hi int32) int32 {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}

func absI32(v int32) int32 {
	if v < 0 {
		return -v
	}
	return v
}
