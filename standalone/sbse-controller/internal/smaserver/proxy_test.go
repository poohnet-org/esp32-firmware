package smaserver

import "testing"

func u32(regs []uint16) uint32 { return uint32(regs[0])<<16 | uint32(regs[1]) }

// TestUnpolledCachedReturnsNaN: a cached group that has not been polled answers
// with the uint32 NaN sentinel.
func TestUnpolledCachedReturnsNaN(t *testing.T) {
	p := NewProxy()
	regs := p.Pack(30513, 4, 0, 0, 255) // Metering.TotWhOut (uint64nan), never polled
	for i, r := range regs {
		if r != 0xFFFF {
			t.Errorf("reg[%d] = %#04x, want 0xFFFF (NaN)", i, r)
		}
	}
}

// TestUncoveredAddressReturnsNaN: an address no group covers answers 0xFFFF.
func TestUncoveredAddressReturnsNaN(t *testing.T) {
	p := NewProxy()
	regs := p.Pack(12345, 2, 0, 0, 255)
	if regs[0] != 0xFFFF || regs[1] != 0xFFFF {
		t.Errorf("uncovered = %#04x %#04x, want 0xFFFF 0xFFFF", regs[0], regs[1])
	}
}

// TestSynthGridPower: 30775 GridMs.TotW reflects grid_w_raw as a signed int32.
func TestSynthGridPower(t *testing.T) {
	p := NewProxy()
	// Positive (import).
	if got := u32(p.Pack(30775, 2, 1234, 0, 80)); got != 1234 {
		t.Errorf("grid power = %d, want 1234", got)
	}
	// Negative (export) -> two's complement int32.
	if got := int32(u32(p.Pack(30775, 2, -50, 0, 80))); got != -50 {
		t.Errorf("grid power = %d, want -50", got)
	}
}

// TestSynthSoc: 30845 Bat.ChaStt is the SoC %, NaN when unknown (255).
func TestSynthSoc(t *testing.T) {
	p := NewProxy()
	if got := u32(p.Pack(30845, 2, 0, 0, 80)); got != 80 {
		t.Errorf("soc = %d, want 80", got)
	}
	regs := p.Pack(30845, 2, 0, 0, 255)
	if regs[0] != 0xFFFF || regs[1] != 0xFFFF {
		t.Errorf("soc unknown = %#04x %#04x, want NaN", regs[0], regs[1])
	}
}

// TestSynthChargeDischarge: 31393/31395 are the unsigned charge/discharge magnitudes.
func TestSynthChargeDischarge(t *testing.T) {
	p := NewProxy()
	// Charging at 300 W: battery_w_raw = -300.
	if got := u32(p.Pack(31393, 2, 0, -300, 80)); got != 300 {
		t.Errorf("charge mag = %d, want 300", got)
	}
	if got := u32(p.Pack(31395, 2, 0, -300, 80)); got != 0 {
		t.Errorf("discharge mag (while charging) = %d, want 0", got)
	}
	// Discharging at 400 W: battery_w_raw = +400.
	if got := u32(p.Pack(31395, 2, 0, 400, 80)); got != 400 {
		t.Errorf("discharge mag = %d, want 400", got)
	}
	if got := u32(p.Pack(31393, 2, 0, 400, 80)); got != 0 {
		t.Errorf("charge mag (while discharging) = %d, want 0", got)
	}
}

// TestPollPopulatesCache: after a successful poll the cached group serves its values.
func TestPollPopulatesCache(t *testing.T) {
	p := NewProxy()
	// Fake upstream: group 30513 (4 regs) returns a recognizable pattern.
	read := func(unit uint8, addr, count uint16) ([]uint16, error) {
		if addr == 30513 && count == 4 {
			return []uint16{0x0011, 0x2233, 0x4455, 0x6677}, nil
		}
		return make([]uint16, count), nil
	}
	// 30513 is the first group, so the first PollOnce targets it.
	p.PollOnce(read)
	regs := p.Pack(30513, 4, 0, 0, 80)
	want := []uint16{0x0011, 0x2233, 0x4455, 0x6677}
	for i := range want {
		if regs[i] != want[i] {
			t.Errorf("reg[%d] = %#04x, want %#04x", i, regs[i], want[i])
		}
	}
}
