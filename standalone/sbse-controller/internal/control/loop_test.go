package control

import "testing"

// makeParams builds the default control-law params for a [lo, hi] deadzone with
// the firmware defaults (Kp 1.0, Kd 0, alpha_grid 0.30, alpha_setpoint 0.70, no
// SoC clamp, generous saturation caps).
func makeParams(lo, hi int32) LawParams {
	return LawParams{
		Kp: 1.0, Kd: 0.0, AlphaGrid: 0.30, AlphaSetpoint: 0.70,
		Lo: lo, Hi: hi, MaxChargeW: 10000, MaxDischargeW: 10000,
		ForceW: 0, SocPct: 255,
	}
}

// simulate runs the closed loop to steady state for a fixed natural grid
// (= home_load - PV): each step the inverter is assumed to follow the commanded
// setpoint exactly, so battery_{k+1} = target_k and grid = natural - battery.
// Returns the converged (grid, battery).
func simulate(t *testing.T, p LawParams, natural, initialBattery int32, steps int) (grid, battery int32) {
	t.Helper()
	st := &LawState{}
	battery = initialBattery
	for i := 0; i < steps; i++ {
		gridRaw := natural - battery
		target, ok := ComputeSetpoint(st, p, gridRaw, battery)
		if !ok {
			t.Fatalf("non-finite setpoint at step %d", i)
		}
		battery = target
		grid = natural - battery
	}
	return grid, battery
}

func approx(t *testing.T, name string, got, want, tol int32) {
	t.Helper()
	d := got - want
	if d < 0 {
		d = -d
	}
	if d > tol {
		t.Errorf("%s = %d, want %d (+/-%d)", name, got, want, tol)
	}
}

// TestConfigMdExamples reproduces the worked steady-state examples from
// CONFIG.md "Grid targets". The analytic fixed point is grid = clamp(natural,lo,hi)
// and battery = natural - grid.
func TestConfigMdExamples(t *testing.T) {
	const steps = 4000
	const tol = 3

	cases := []struct {
		name        string
		lo, hi      int32
		natural     int32
		wantGrid    int32
		wantBattery int32
	}{
		// lo = hi = 0 (pure self-consumption)
		{"selfconsume/surplus", 0, 0, -500, 0, -500},
		{"selfconsume/deficit", 0, 0, 500, 0, 500},
		// lo = -200, hi = 0 (export-preferred)
		{"export/surplus500", -200, 0, -500, -200, -300},
		{"export/deadzone", -200, 0, -100, -100, 0},
		{"export/hi-boundary", -200, 0, 0, 0, 0},
		{"export/deficit500", -200, 0, 500, 0, 500},
		// lo = -720, hi = -500 (the live setup)
		{"live/large-surplus", -720, -500, -900, -720, -180},
		{"live/deadzone-700", -720, -500, -700, -700, 0},
		{"live/deadzone-600", -720, -500, -600, -600, 0},
		{"live/hi-boundary", -720, -500, -500, -500, 0},
		{"live/rescue", -720, -500, -400, -500, 100},
		// lo = hi = -200 (hard: maintain 200 W export)
		{"hard/surplus", -200, -200, -500, -200, -300},
		{"hard/small", -200, -200, -100, -200, 100},
		{"hard/deficit", -200, -200, 500, -200, 700},
		// lo = 0, hi = +500 (allow some import before discharging)
		{"import/small-surplus", 0, 500, -100, 0, -100},
		{"import/deadzone", 0, 500, 200, 200, 0},
		{"import/importing", 0, 500, 800, 500, 300},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			p := makeParams(tc.lo, tc.hi)
			grid, battery := simulate(t, p, tc.natural, 0, steps)
			approx(t, "grid", grid, tc.wantGrid, tol)
			approx(t, "battery", battery, tc.wantBattery, tol)
		})
	}
}

// TestDeadzoneWindDown verifies that a battery entering the deadzone in a non-idle
// state is wound down to ~0 by the implicit-I term (firmware "passive in steady
// state" behaviour), rather than held.
func TestDeadzoneWindDown(t *testing.T) {
	p := makeParams(-720, -500)
	// natural = -600 sits inside the deadzone; start the battery charging at -180.
	grid, battery := simulate(t, p, -600, -180, 4000)
	approx(t, "grid", grid, -600, 3)
	approx(t, "battery", battery, 0, 3)
}

// TestForceModeBypass: force-mode commands the exact watts, bypassing the P loop.
func TestForceModeBypass(t *testing.T) {
	p := makeParams(0, 0)
	p.ForceW = -1500 // force charge 1500 W
	st := &LawState{}
	// First call seeds ema_setpoint = raw = -1500.
	got, ok := ComputeSetpoint(st, p, 0, 0)
	if !ok {
		t.Fatal("non-finite")
	}
	if got != -1500 {
		t.Errorf("force charge setpoint = %d, want -1500", got)
	}
}

// TestForceModeClampedByCaps: force power is still clamped by the operator's caps.
func TestForceModeClampedByCaps(t *testing.T) {
	p := makeParams(0, 0)
	p.ForceW = 5000
	p.MaxDischargeW = 1200
	st := &LawState{}
	got, _ := ComputeSetpoint(st, p, 0, 0)
	if got != 1200 {
		t.Errorf("force discharge clamped = %d, want 1200", got)
	}
}

// TestSocClampBlocksCharge: at 100% SoC a charge command is zeroed.
func TestSocClampBlocksCharge(t *testing.T) {
	p := makeParams(0, 0)
	p.SocPct = 100
	st := &LawState{}
	// natural = -500 would charge; SoC clamp must zero it.
	got, _ := ComputeSetpoint(st, p, -500, 0)
	if got != 0 {
		t.Errorf("setpoint at 100%% SoC = %d, want 0 (charge blocked)", got)
	}
}

// TestSocClampBlocksDischarge: at 0% SoC a discharge command is zeroed.
func TestSocClampBlocksDischarge(t *testing.T) {
	p := makeParams(0, 0)
	p.SocPct = 0
	st := &LawState{}
	got, _ := ComputeSetpoint(st, p, 500, 0)
	if got != 0 {
		t.Errorf("setpoint at 0%% SoC = %d, want 0 (discharge blocked)", got)
	}
}

// TestSaturationClamp: the battery cannot exceed max_charge_w even when the grid
// target is unreachable.
func TestSaturationClamp(t *testing.T) {
	p := makeParams(-750, -750)
	p.MaxChargeW = 100
	_, battery := simulate(t, p, -2000, 0, 2000)
	if battery != -100 {
		t.Errorf("battery = %d, want -100 (clamped to max_charge_w)", battery)
	}
}

// TestDirectionLockNoChargeToChaseHi: in the discharging regime (natural > hi) the
// controller never charges, even if the formula would.
func TestDirectionLockNoChargeToChaseHi(t *testing.T) {
	p := makeParams(-720, -500)
	// Steady discharge at the hi boundary: battery sourcing 1280 W, natural well
	// above hi. A single noise dip in ema must not slam the setpoint negative.
	// Seed: grid such that natural = ema+battery stays > hi.
	// natural = -400 (>hi=-500) with battery discharging 100 -> grid -500.
	grid, battery := simulate(t, p, -400, 100, 4000)
	if battery < 0 {
		t.Errorf("battery = %d, want >= 0 (never charge in discharging regime)", battery)
	}
	approx(t, "grid", grid, -500, 3)
}

// TestKeepalivePulseAlternates verifies the alternating-sign keep-alive selection.
func TestKeepalivePulseAlternates(t *testing.T) {
	next := false // start: discharge
	first := PickKeepalivePulse(50, 5000, 5000, 80, &next)
	second := PickKeepalivePulse(50, 5000, 5000, 80, &next)
	if first != 50 {
		t.Errorf("first pulse = %d, want 50 (discharge)", first)
	}
	if second != -50 {
		t.Errorf("second pulse = %d, want -50 (charge)", second)
	}
}

// TestKeepalivePulseRespectsSoc: at 100% SoC only discharge pulses are allowed.
func TestKeepalivePulseRespectsSoc(t *testing.T) {
	next := true // prefer charge
	got := PickKeepalivePulse(50, 5000, 5000, 100, &next)
	if got != 50 {
		t.Errorf("pulse at 100%% SoC = %d, want 50 (charge blocked -> discharge)", got)
	}
}

// TestKeepalivePulseBlockedBothDirections returns 0 when neither direction works.
func TestKeepalivePulseBlockedBothDirections(t *testing.T) {
	next := false
	if got := PickKeepalivePulse(50, 0, 0, 50, &next); got != 0 {
		t.Errorf("pulse with both caps 0 = %d, want 0", got)
	}
}
