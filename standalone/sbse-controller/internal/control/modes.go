package control

// Mode mirrors the firmware SbseController::Mode enum and its string names
// (sbse_control_loop.cpp mode_name). The block_* values are derived from the
// saturation caps regardless of who set them.
type Mode uint8

const (
	ModeDisabled Mode = iota
	ModeNotConnected
	ModeStale
	ModeRunning
	ModeFaulted
	ModePaused
	ModeSafety
	ModeForceCharge
	ModeForceDischarge
	ModeBlocked
	ModeBlockCharge
	ModeBlockDischarge
)

func (m Mode) String() string {
	switch m {
	case ModeDisabled:
		return "disabled"
	case ModeNotConnected:
		return "not_connected"
	case ModeStale:
		return "stale"
	case ModeRunning:
		return "running"
	case ModeFaulted:
		return "faulted"
	case ModePaused:
		return "paused"
	case ModeSafety:
		return "safety"
	case ModeForceCharge:
		return "force_charge"
	case ModeForceDischarge:
		return "force_discharge"
	case ModeBlocked:
		return "blocked"
	case ModeBlockCharge:
		return "block_charge"
	case ModeBlockDischarge:
		return "block_discharge"
	default:
		return "?"
	}
}

// Authority mirrors SbseController::ModbusAuthority: how much of active_config an
// external Modbus client may overwrite on a 40793 write.
type Authority uint8

const (
	AuthorityForceOnly Authority = 0 // ignore caps + GridWSpt
	AuthorityCaps      Authority = 1 // apply max_charge_w/max_discharge_w
	AuthorityFull      Authority = 2 // also mirror GridWSpt into both grid targets
)

// SMA OpMod values latched from a 40236 write.
const (
	OpModDefault        uint16 = 2424
	OpModForceCharge    uint16 = 2289
	OpModForceDischarge uint16 = 2290
)

// runningMode picks the running-family Mode from the force state and saturation
// caps (firmware current_running_mode, sbse_control_loop.cpp:737-760).
func runningMode(forceW, maxChargeW, maxDischargeW int32) Mode {
	if forceW < 0 {
		return ModeForceCharge
	}
	if forceW > 0 {
		return ModeForceDischarge
	}
	if maxChargeW == 0 && maxDischargeW == 0 {
		return ModeBlocked
	}
	if maxChargeW == 0 {
		return ModeBlockCharge
	}
	if maxDischargeW == 0 {
		return ModeBlockDischarge
	}
	return ModeRunning
}
