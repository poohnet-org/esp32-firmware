package control

import (
	"fmt"
	"math"
	"time"
)

// Tick runs one control cycle. It is invoked sequentially from a single goroutine,
// so cycles never overlap (no cycle_in_flight guard needed). The state mutex is
// held only around field access, never across the blocking Modbus transactions.
func (c *Controller) Tick() {
	c.mu.Lock()
	c.watchdogTick()
	proceed, socDue := c.beginCycleLocked()
	c.mu.Unlock()
	if !proceed {
		c.notifyState()
		return
	}

	// Ensure the inverter connection (blocking dial, no lock held).
	if !c.inv.Connected() {
		if err := c.inv.Connect(); err != nil {
			c.mu.Lock()
			c.setModeLocked(ModeNotConnected)
			c.mu.Unlock()
			c.notifyState()
			return
		}
		c.mu.Lock()
		c.law.Reset()
		c.consecutiveFail = 0
		c.safetyZeroArmed = false
		c.batteryIdleSince = time.Time{}
		c.keepalivePendingZero = false
		c.setModeLocked(ModeStale)
		c.mu.Unlock()
	}

	// Reads (blocking).
	grid, err := c.inv.ReadGridPowerW()
	if err != nil {
		c.cycleFailed("grid_power", err)
		return
	}
	battery, err := c.inv.ReadBatteryPowerW()
	if err != nil {
		c.cycleFailed("battery_power", err)
		return
	}
	soc := uint8(255)
	socOK := false
	if socDue {
		if s, serr := c.inv.ReadSocPct(); serr != nil {
			// Non-fatal: keep the last known SoC and proceed.
			c.logf("soc read failed: %v", serr)
		} else {
			soc, socOK = s, true
		}
	}

	c.computeAndWrite(grid, battery, soc, socOK)
}

// beginCycleLocked performs the firmware begin_cycle gating (minus the connection
// check, which Tick does unlocked). Returns whether to proceed and whether a SoC
// read is due. Caller holds mu.
func (c *Controller) beginCycleLocked() (proceed, socDue bool) {
	if !c.enabled {
		c.setModeLocked(ModeDisabled)
		return false, false
	}
	if c.paused {
		if !c.now().Before(c.pausedUntil) {
			c.paused = false
			c.law.Reset()
			c.batteryIdleSince = time.Time{}
			c.keepalivePendingZero = false
		} else {
			c.setModeLocked(ModePaused)
			return false, false
		}
	}
	socDue = c.lastSocRead.IsZero() || c.now().Sub(c.lastSocRead) >= c.socInterval()
	return true, socDue
}

// computeAndWrite runs the control law, the keep-alive heartbeat and the deadband,
// then writes the setpoint if needed (firmware compute_and_write). The law + write
// decision run under mu; the actual write does not.
func (c *Controller) computeAndWrite(grid, battery int32, soc uint8, socOK bool) {
	c.mu.Lock()

	// A pause requested mid-cycle: the 0 W write already went out, drop this one.
	if c.paused {
		c.setModeLocked(ModePaused)
		c.mu.Unlock()
		c.notifyState()
		return
	}

	// All required reads landed -- reset the safety streak.
	if c.consecutiveFail != 0 || c.safetyZeroArmed {
		c.consecutiveFail = 0
		c.safetyZeroArmed = false
	}
	c.lastError = ""

	if socOK {
		c.socPct = soc
		c.lastSocRead = c.now()
	}
	c.gridWRaw = grid
	c.batteryWRaw = battery

	p := LawParams{
		Kp: c.kp, Kd: c.kd, AlphaGrid: c.alphaGrid, AlphaSetpoint: c.alphaSetpoint,
		Lo: c.lo, Hi: c.hi, MaxChargeW: c.maxChargeW, MaxDischargeW: c.maxDischargeW,
		ForceW: c.modbusForceW, SocPct: c.socPct,
	}
	targetW, finite := ComputeSetpoint(&c.law, p, grid, battery)
	if !finite {
		c.lastError = "non-finite setpoint computed"
		c.setModeLocked(ModeFaulted)
		c.mu.Unlock()
		c.notifyState()
		return
	}

	c.trace.AddSample(int32(math.Round(c.law.EmaGrid)), battery, c.lastWrittenW, c.lo, c.hi)

	// Keep-alive idle tracking.
	if battery != 0 {
		c.batteryIdleSince = time.Time{}
	} else if c.batteryIdleSince.IsZero() {
		c.batteryIdleSince = c.now()
	}

	keepaliveFired := false
	keepaliveReturn := false
	interval := time.Duration(c.keepaliveIntervalS) * time.Second
	if c.keepalivePendingZero {
		c.keepalivePendingZero = false
		if c.modbusForceW == 0 {
			keepaliveReturn = true
			targetW = 0
			c.law.EmaSetpoint = 0
		}
	} else if c.keepaliveIntervalS > 0 && targetW == 0 && c.modbusForceW == 0 &&
		!c.batteryIdleSince.IsZero() && c.now().Sub(c.batteryIdleSince) >= interval {
		pulse := PickKeepalivePulse(c.keepalivePulseW, c.maxChargeW, c.maxDischargeW, c.socPct, &c.keepaliveNextCharge)
		if pulse != 0 {
			targetW = pulse
			keepaliveFired = true
			c.keepalivePendingZero = true
			c.batteryIdleSince = c.now()
		}
	}

	keepaliveRefreshDue := c.keepaliveIntervalS > 0 && !c.lastWriteOK.IsZero() &&
		c.now().Sub(c.lastWriteOK) >= interval
	bypass := keepaliveFired || keepaliveReturn || keepaliveRefreshDue

	if !bypass && !c.lastWriteOK.IsZero() && absI32(targetW-c.lastWrittenW) < c.deadbandW {
		c.setModeLocked(runningMode(c.modbusForceW, c.maxChargeW, c.maxDischargeW))
		c.mu.Unlock()
		c.notifyState()
		return
	}

	forceW, maxC, maxD := c.modbusForceW, c.maxChargeW, c.maxDischargeW
	c.mu.Unlock()

	// Write (blocking).
	err := c.inv.WriteSetpointW(targetW)

	c.mu.Lock()
	if err != nil {
		c.writeErrCount++
		c.lastError = fmt.Sprintf("write failed: %v", err)
		c.logf("setpoint write failed: %v", err)
		c.setModeLocked(ModeStale)
		c.mu.Unlock()
		c.notifyState()
		return
	}
	c.lastWrittenW = targetW
	c.lastWriteOK = c.now()
	c.writeOkCount++
	c.setModeLocked(runningMode(forceW, maxC, maxD))
	c.mu.Unlock()
	c.notifyState()
}

// cycleFailed records a read failure, arms the safety-zero one-shot when the streak
// crosses the threshold, and forces a reconnect after a longer streak (the firmware
// relied on the pool's socket-level disconnect detection, which simonvetter lacks).
func (c *Controller) cycleFailed(where string, ferr error) {
	c.mu.Lock()
	c.lastError = fmt.Sprintf("%s read failed: %v", where, ferr)
	c.consecutiveFail++
	streak := c.consecutiveFail
	c.logf("%s read failed: %v", where, ferr)
	arm := c.safetyZeroAfterFails != 0 && !c.safetyZeroArmed &&
		streak >= c.safetyZeroAfterFails && c.inv.Connected()
	if arm {
		c.safetyZeroArmed = true
	}
	armed := c.safetyZeroArmed
	reconnect := streak >= c.reconnectThresholdLocked()
	c.mu.Unlock()

	if arm {
		c.logf("read failure streak hit %d, commanding 0 W safety setpoint", streak)
		c.sendSafetyZero()
		return
	}

	if reconnect {
		c.inv.Close() // next tick reconnects + reseeds
	}

	c.mu.Lock()
	if armed {
		c.setModeLocked(ModeSafety)
	} else {
		c.setModeLocked(ModeStale)
	}
	c.mu.Unlock()
	c.notifyState()
}

// sendSafetyZero writes a one-shot 0 W setpoint (firmware send_safety_zero).
func (c *Controller) sendSafetyZero() {
	err := c.inv.WriteSetpointW(0)
	c.mu.Lock()
	if err == nil {
		c.lastWrittenW = 0
		c.lastWriteOK = c.now()
		c.writeOkCount++
	} else {
		c.writeErrCount++
		c.logf("safety-zero write failed: %v", err)
	}
	c.setModeLocked(ModeSafety)
	c.mu.Unlock()
	c.notifyState()
}

// reconnectThresholdLocked is the streak length that forces a fresh TCP connection.
func (c *Controller) reconnectThresholdLocked() uint32 {
	if c.safetyZeroAfterFails == 0 {
		return 20
	}
	return c.safetyZeroAfterFails * 2
}

// Shutdown writes a best-effort 0 W setpoint and closes the connection (firmware
// pre_reboot).
func (c *Controller) Shutdown() {
	if c.inv.Connected() {
		_ = c.inv.WriteSetpointW(0)
	}
	c.inv.Close()
}
