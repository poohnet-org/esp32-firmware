package control

import "math"

// LawState holds the exponential-moving-average state carried between ticks.
// Only the control loop mutates it, so it needs no locking of its own.
type LawState struct {
	EmaGridSeeded     bool
	EmaGrid           float64
	PrevEmaGridSeeded bool
	PrevEmaGrid       float64
	EmaSetpointSeeded bool
	EmaSetpoint       float64
}

// Reset clears the EMA seeds so the next sample re-seeds them. Used on connect,
// pause/resume and disconnect to avoid a derivative kick from a stale reference.
func (s *LawState) Reset() {
	s.EmaGridSeeded = false
	s.PrevEmaGridSeeded = false
	s.EmaSetpointSeeded = false
}

// LawParams are the per-tick control-law inputs derived from active_config plus
// the Modbus force-mode override and the latest SoC.
type LawParams struct {
	Kp, Kd                    float64
	AlphaGrid, AlphaSetpoint  float64
	Lo, Hi                    int32 // grid_charge_target_w, grid_discharge_target_w
	MaxChargeW, MaxDischargeW int32
	ForceW                    int32 // modbus_force_w; 0 = P controller in charge
	SocPct                    uint8 // 255 = unknown (no SoC clamp)
}

// ComputeSetpoint runs the firmware control law (compute_and_write steps 1-6,
// sbse_control_loop.cpp:330-442): grid EMA, D-on-measurement, force-mode bypass or
// the regime-based P+implicit-I+D law over the [lo,hi] deadzone with direction
// lock, SoC clamps, the saturation clamp, and the output EMA. It mutates st's EMAs
// and returns the rounded target setpoint plus a finite flag (false => refuse to
// write, mirroring the NaN/inf guard).
func ComputeSetpoint(st *LawState, p LawParams, gridWRaw, batteryWRaw int32) (int32, bool) {
	// 1) Smooth grid (seed on first sample).
	if !st.EmaGridSeeded {
		st.EmaGrid = float64(gridWRaw)
		st.EmaGridSeeded = true
	} else {
		st.EmaGrid = p.AlphaGrid*float64(gridWRaw) + (1.0-p.AlphaGrid)*st.EmaGrid
	}

	// 2) Derivative on the smoothed measurement (0 on the first cycle after seeding).
	dGrid := 0.0
	if st.PrevEmaGridSeeded {
		dGrid = st.EmaGrid - st.PrevEmaGrid
	}
	st.PrevEmaGrid = st.EmaGrid
	st.PrevEmaGridSeeded = true

	// 3) Raw battery setpoint.
	var raw float64
	if p.ForceW != 0 {
		// 3a) Force-mode bypass.
		raw = float64(p.ForceW)
	} else {
		// 3b) Regime-based target over the [lo, hi] deadzone.
		loF := float64(p.Lo)
		hiF := float64(p.Hi)
		natural := st.EmaGrid + float64(batteryWRaw) // = home_load - PV
		target := clampF(natural, loF, hiF)
		delta := st.EmaGrid - target
		raw = float64(batteryWRaw) + p.Kp*delta + p.Kd*dGrid

		// Direction lock: each active regime acts in its natural direction only.
		if natural > hiF && raw < 0.0 {
			raw = 0.0
		}
		if natural < loF && raw > 0.0 {
			raw = 0.0
		}
	}

	// SoC limits (only with a usable reading).
	if p.SocPct != 255 {
		if p.SocPct >= 100 && raw < 0.0 {
			raw = 0.0
		}
		if p.SocPct == 0 && raw > 0.0 {
			raw = 0.0
		}
	}

	// Hard limits.
	raw = clampF(raw, float64(-p.MaxChargeW), float64(p.MaxDischargeW))

	// Smooth the commanded setpoint.
	if !st.EmaSetpointSeeded {
		st.EmaSetpoint = raw
		st.EmaSetpointSeeded = true
	} else {
		st.EmaSetpoint = p.AlphaSetpoint*raw + (1.0-p.AlphaSetpoint)*st.EmaSetpoint
	}

	// NaN / inf guard.
	if math.IsInf(st.EmaSetpoint, 0) || math.IsNaN(st.EmaSetpoint) {
		return 0, false
	}
	return int32(math.Round(st.EmaSetpoint)), true
}

// PickKeepalivePulse chooses the next keep-alive pulse: alternating sign, respecting
// the saturation caps and SoC edges (firmware pick_keepalive_pulse). nextCharge is
// the alternation flag, toggled in place. Returns 0 when no direction is available.
func PickKeepalivePulse(pulseW, maxChargeW, maxDischargeW int32, socPct uint8, nextCharge *bool) int32 {
	mag := pulseW
	if mag <= 0 {
		return 0
	}
	pulseD := min(mag, maxDischargeW)
	pulseC := min(mag, maxChargeW)
	canDischarge := pulseD > 0 && (socPct == 255 || socPct > 0)
	canCharge := pulseC > 0 && (socPct == 255 || socPct < 100)
	if !canDischarge && !canCharge {
		return 0
	}
	charge := *nextCharge
	if charge && !canCharge {
		charge = false
	}
	if !charge && !canDischarge {
		charge = true
	}
	*nextCharge = !charge
	if charge {
		return -pulseC
	}
	return pulseD
}

func clampF(v, lo, hi float64) float64 {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}
