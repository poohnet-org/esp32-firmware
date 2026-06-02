package smaserver

import "sync"

// Proxy is the read-side register cache that lets evcc's sma-hybrid template (and
// any SMA-aware client) read the SBSE controller as if it were the hybrid inverter.
// Ported verbatim from sbse_modbus_proxy.{h,cpp}: four hot registers are synthesized
// from controller state at read time, the rest are populated by a round-robin
// background poll. Uncovered / un-polled registers answer with the SMA NaN sentinel.
//
// Upstream Modbus units (match the control-loop constants).
const (
	upstreamInverterUnit  = 3
	upstreamGridMeterUnit = 2
	cacheSize             = 64
)

type source uint8

const (
	srcCached source = iota
	srcSynthesized
)

type nanStyle uint8

const (
	nanUnsignedMax   nanStyle = iota // 0xFFFFFFFF / 0xFFFFFFFFFFFFFFFF
	nanInt32Negative                 // 0x80000000
)

type group struct {
	addr         uint16
	regCount     uint16
	upstreamUnit uint8
	cacheOffset  uint8
	src          source
	nan          nanStyle
}

// groups: order is the round-robin poll order; cache offsets are assigned manually.
// (Verbatim from sbse_modbus_proxy.cpp GROUPS.)
var groups = []group{
	// inverter (unit 3)
	{30513, 4, upstreamInverterUnit, 0, srcCached, nanUnsignedMax},  // Metering.TotWhOut
	{30773, 2, upstreamInverterUnit, 4, srcCached, nanUnsignedMax},  // DcMs.Watt[0]
	{30775, 2, 0, 0, srcSynthesized, nanInt32Negative},              // GridMs.TotW
	{30845, 2, 0, 0, srcSynthesized, nanUnsignedMax},                // Bat.ChaStt (SoC)
	{30961, 2, upstreamInverterUnit, 6, srcCached, nanUnsignedMax},  // DcMs.Amp[0]
	{30967, 2, upstreamInverterUnit, 8, srcCached, nanUnsignedMax},  // DcMs.Vol[0]
	{31393, 2, 0, 0, srcSynthesized, nanUnsignedMax},                // BatChrg.CurBatCha
	{31395, 2, 0, 0, srcSynthesized, nanUnsignedMax},                // BatDsch.CurBatDsch
	{31401, 4, upstreamInverterUnit, 10, srcCached, nanUnsignedMax}, // CmpBMS.GetBatDschWh
	// grid meter (unit 2)
	{30581, 4, upstreamGridMeterUnit, 14, srcCached, nanUnsignedMax},  // cumulative Wh
	{30865, 8, upstreamGridMeterUnit, 18, srcCached, nanUnsignedMax},  // TotWhIn / TotWhOut
	{31259, 10, upstreamGridMeterUnit, 26, srcCached, nanUnsignedMax}, // per-phase W
	{31435, 6, upstreamGridMeterUnit, 36, srcCached, nanUnsignedMax},  // per-phase A
}

// Proxy holds the cache and round-robin poll state.
type Proxy struct {
	mu           sync.Mutex
	cache        [cacheSize]uint16
	freshMask    uint32 // bit i = group i has at least one good poll
	nextIdx      int
	pollInFlight bool
}

// NewProxy returns an empty proxy (all registers answer NaN until polled).
func NewProxy() *Proxy { return &Proxy{} }

// InvalidateAll drops every freshness marker (called on upstream disconnect).
func (p *Proxy) InvalidateAll() {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.freshMask = 0
	p.pollInFlight = false
}

// PollOnce performs one round-robin upstream read using read(), updating the cache.
// It is a no-op when a poll is already in flight or there are no cached groups.
func (p *Proxy) PollOnce(read func(unit uint8, addr, count uint16) ([]uint16, error)) {
	p.mu.Lock()
	if p.pollInFlight {
		p.mu.Unlock()
		return
	}
	idx := -1
	for tries := 0; tries < len(groups); tries++ {
		i := p.nextIdx
		p.nextIdx = (p.nextIdx + 1) % len(groups)
		if groups[i].src == srcCached {
			idx = i
			break
		}
	}
	if idx < 0 {
		p.mu.Unlock()
		return
	}
	g := groups[idx]
	p.pollInFlight = true
	p.mu.Unlock()

	regs, err := read(g.upstreamUnit, g.addr, g.regCount)

	p.mu.Lock()
	p.pollInFlight = false
	if err == nil && len(regs) >= int(g.regCount) {
		copy(p.cache[g.cacheOffset:g.cacheOffset+uint8(g.regCount)], regs)
		p.freshMask |= 1 << uint(idx)
	}
	p.mu.Unlock()
}

// Pack fills count registers starting at startAddr from the cache + synthesis.
// grid/battery/soc are the live synthesis inputs (soc 255 = unknown -> NaN).
func (p *Proxy) Pack(startAddr, count uint16, gridWRaw, batteryWRaw int32, socPct uint8) []uint16 {
	p.mu.Lock()
	defer p.mu.Unlock()
	out := make([]uint16, count)
	for i := uint16(0); i < count; i++ {
		addr := startAddr + i
		gi := findGroup(addr)
		if gi < 0 {
			out[i] = 0xFFFF // unknown register -> uint32nan default
			continue
		}
		out[i] = p.writeRegisterLocked(gi, addr, gridWRaw, batteryWRaw, socPct)
	}
	return out
}

func findGroup(addr uint16) int {
	for i := range groups {
		g := groups[i]
		if addr >= g.addr && addr < g.addr+g.regCount {
			return i
		}
	}
	return -1
}

func emitNaN(style nanStyle, offInU32 uint16) uint16 {
	if style == nanInt32Negative {
		if offInU32 == 0 {
			return 0x8000
		}
		return 0x0000
	}
	return 0xFFFF
}

// writeRegisterLocked returns one register's value (cached / synth / NaN). Caller holds mu.
func (p *Proxy) writeRegisterLocked(gi int, addr uint16, gridWRaw, batteryWRaw int32, socPct uint8) uint16 {
	g := groups[gi]
	addrOff := addr - g.addr
	offInU32 := addrOff % 2

	if g.src == srcCached {
		if p.freshMask&(1<<uint(gi)) == 0 {
			return emitNaN(g.nan, offInU32)
		}
		return p.cache[uint16(g.cacheOffset)+addrOff]
	}

	// Synthesized: compute the uint32 value (or NaN), then slice the relevant half.
	var (
		value   uint32
		emitNan bool
	)
	switch g.addr {
	case 30775: // GridMs.TotW (int32, positive = import)
		value = uint32(gridWRaw)
	case 30845: // Bat.ChaStt (SoC %)
		if socPct == 255 {
			emitNan = true
		} else {
			value = uint32(socPct)
		}
	case 31393: // BatChrg.CurBatCha -- charge magnitude
		if batteryWRaw < 0 {
			value = uint32(-batteryWRaw)
		}
	case 31395: // BatDsch.CurBatDsch -- discharge magnitude
		if batteryWRaw > 0 {
			value = uint32(batteryWRaw)
		}
	default:
		emitNan = true
	}
	if emitNan {
		return emitNaN(g.nan, offInU32)
	}
	if offInU32 == 0 {
		return uint16(value >> 16)
	}
	return uint16(value & 0xFFFF)
}
