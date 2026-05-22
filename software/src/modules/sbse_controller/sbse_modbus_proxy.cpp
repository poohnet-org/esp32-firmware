/* esp32-firmware
 * Copyright (C) 2026 Thomas Hein
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "sbse_modbus_proxy.h"

#include "gcc_warnings.h"

// find_group / emit_nan_register read only static-const tables and their
// arguments, so GCC suggests __attribute__((const)). Annotating one but not
// the other would be inconsistent; silencing once here keeps the file tidy.
#pragma GCC diagnostic ignored "-Wsuggest-attribute=const"

// ---------------------------------------------------------------------------
// Register groups served by the proxy. Order is the round-robin polling
// order for cached groups; synthesized groups are skipped during polling
// and only consulted at read time. Cache offsets are assigned manually to
// keep the layout obvious at a glance; total cached registers must stay
// <= CACHE_SIZE (compile-time-checked further down).
//
// Sources of the cached values: the SBSE inverter exposes its own and
// downstream PV/grid telemetry on unit 3, while the dedicated grid meter
// lives on unit 2 (matches the addresses the existing control loop reads
// from -- see GRID_METER_UNIT_ID / INVERTER_UNIT_ID in
// sbse_control_loop.cpp).
//
// NaN sentinels: SMA defines 0xFFFFFFFF for uint32nan / uint64nan and
// 0x80000000 for int32nan. We pick UnsignedMax for everything that's
// known to be unsigned; Int32Negative is reserved for the one signed
// register we hit (30775 GridMs.TotW), and that one is synthesized so
// the NaN sentinel only matters during the brief startup window before
// the controller has its first grid sample.
// ---------------------------------------------------------------------------

const SbseModbusProxy::Group SbseModbusProxy::GROUPS[] = {
    // ---- inverter (unit 3) ------------------------------------------------
    // 30513  Metering.TotWhOut             uint64nan -- cumulative energy out (Wh, ×0.001)
    { 30513, 4, UPSTREAM_INVERTER_UNIT,    0, Source::Cached,      NaNStyle::UnsignedMax   },
    // 30773  DcMs.Watt[0]                  uint32nan -- PV string 1 DC power
    { 30773, 2, UPSTREAM_INVERTER_UNIT,    4, Source::Cached,      NaNStyle::UnsignedMax   },
    // 30775  GridMs.TotW                   int32nan  -- current grid power (synthesized)
    { 30775, 2, 0,                          0, Source::Synthesized, NaNStyle::Int32Negative },
    // 30845  Bat.ChaStt                    uint32nan -- SoC % (synthesized)
    { 30845, 2, 0,                          0, Source::Synthesized, NaNStyle::UnsignedMax   },
    // 30961  DcMs.Amp[0]                   uint32nan -- PV string 1 DC current (×0.001)
    { 30961, 2, UPSTREAM_INVERTER_UNIT,    6, Source::Cached,      NaNStyle::UnsignedMax   },
    // 30967  DcMs.Vol[0]                   uint32nan -- PV string 1 DC voltage (×0.01)
    { 30967, 2, UPSTREAM_INVERTER_UNIT,    8, Source::Cached,      NaNStyle::UnsignedMax   },
    // 31393  BatChrg.CurBatCha             uint32nan -- battery charge power (synthesized)
    { 31393, 2, 0,                          0, Source::Synthesized, NaNStyle::UnsignedMax   },
    // 31395  BatDsch.CurBatDsch            uint32nan -- battery discharge power (synthesized)
    { 31395, 2, 0,                          0, Source::Synthesized, NaNStyle::UnsignedMax   },
    // 31401  CmpBMS.GetBatDschWh           uint64nan -- cumulative discharge energy (Wh, ×0.001)
    { 31401, 4, UPSTREAM_INVERTER_UNIT,   10, Source::Cached,      NaNStyle::UnsignedMax   },

    // ---- grid meter (unit 2) ---------------------------------------------
    // 30581  Metering cumulative Wh        uint64nan -- per evcc sma-hybrid template
    { 30581, 4, UPSTREAM_GRID_METER_UNIT, 14, Source::Cached,      NaNStyle::UnsignedMax   },
    // 30865..30872  Metering totals        uint64nan x2 (TotWhIn / TotWhOut)
    { 30865, 8, UPSTREAM_GRID_METER_UNIT, 18, Source::Cached,      NaNStyle::UnsignedMax   },
    // 31259..31268  per-phase W block       (TotW + 3x phase W = 4x uint32 = 8 reg + 2 reserved)
    { 31259, 10, UPSTREAM_GRID_METER_UNIT, 26, Source::Cached,      NaNStyle::UnsignedMax   },
    // 31435..31440  per-phase A             3x uint32
    { 31435, 6, UPSTREAM_GRID_METER_UNIT, 36, Source::Cached,      NaNStyle::UnsignedMax   },
};

const size_t SbseModbusProxy::NUM_GROUPS = sizeof(GROUPS) / sizeof(GROUPS[0]);

// Manual cache layout (offsets above) -- the last group ends at
// cache_offset 36 + reg_count 6 = 42, well below CACHE_SIZE (64). Add a
// new group? Reserve a slot at the next free offset and bump the bound.

// ---------------------------------------------------------------------------

void SbseModbusProxy::invalidate_all()
{
    fresh_mask     = 0;
    poll_in_flight = false;
}

bool SbseModbusProxy::next_poll(size_t *group_idx, uint8_t *upstream_unit,
                                uint16_t *addr, uint16_t *reg_count,
                                uint16_t **cache_dst)
{
    if (poll_in_flight) {
        return false;
    }
    for (size_t tries = 0; tries < NUM_GROUPS; ++tries) {
        const size_t idx = next_idx;
        next_idx = (next_idx + 1) % NUM_GROUPS;
        const Group &g = GROUPS[idx];
        if (g.source != Source::Cached) {
            continue;
        }
        *group_idx     = idx;
        *upstream_unit = g.upstream_unit;
        *addr          = g.addr;
        *reg_count     = g.reg_count;
        *cache_dst     = &cache[g.cache_offset];
        poll_in_flight = true;
        return true;
    }
    return false;
}

void SbseModbusProxy::mark_group_done(size_t group_idx, bool success)
{
    poll_in_flight = false;
    if (success && group_idx < NUM_GROUPS) {
        fresh_mask |= (1u << group_idx);
    }
}

// ---------------------------------------------------------------------------
// Read-path: pack a server response from cache + synthesis.
// ---------------------------------------------------------------------------

size_t SbseModbusProxy::find_group(uint16_t addr) const
{
    for (size_t i = 0; i < NUM_GROUPS; ++i) {
        const Group &g = GROUPS[i];
        if (addr >= g.addr && addr < g.addr + g.reg_count) {
            return i;
        }
    }
    return NUM_GROUPS;
}

uint16_t SbseModbusProxy::emit_nan_register(NaNStyle nan, uint16_t off_in_u32)
{
    // off_in_u32 is 0 for the high half, 1 for the low half of a uint32.
    // For uint64nan the same pattern repeats across all four registers, so
    // we don't need a special case.
    if (nan == NaNStyle::Int32Negative) {
        return off_in_u32 == 0 ? 0x8000u : 0x0000u;
    }
    return 0xFFFFu;
}

void SbseModbusProxy::write_register(size_t group_idx, uint16_t addr,
                                     const SynthesisInputs &inputs,
                                     uint16_t *out) const
{
    const Group &g = GROUPS[group_idx];
    const uint16_t addr_off = addr - g.addr;
    const uint16_t off_in_u32 = static_cast<uint16_t>(addr_off % 2);

    if (g.source == Source::Cached) {
        if ((fresh_mask & (1u << group_idx)) == 0) {
            *out = emit_nan_register(g.nan, off_in_u32);
            return;
        }
        *out = cache[g.cache_offset + addr_off];
        return;
    }

    // Synthesized. Compute the uint32 value (or signal NaN) keyed on the
    // group's first address, then slice out the relevant half.
    bool     emit_nan = false;
    uint32_t value    = 0;

    switch (g.addr) {
        case 30775: { // GridMs.TotW (int32, signed)
            value = static_cast<uint32_t>(inputs.grid_w_raw);
            break;
        }
        case 30845: { // Bat.ChaStt (uint32, %)
            if (inputs.soc_pct == 255) {
                emit_nan = true;
            } else {
                value = static_cast<uint32_t>(inputs.soc_pct);
            }
            break;
        }
        case 31393: { // BatChrg.CurBatCha (uint32, W) -- charge magnitude
            value = inputs.battery_w_raw < 0
                  ? static_cast<uint32_t>(-inputs.battery_w_raw)
                  : 0u;
            break;
        }
        case 31395: { // BatDsch.CurBatDsch (uint32, W) -- discharge magnitude
            value = inputs.battery_w_raw > 0
                  ? static_cast<uint32_t>(inputs.battery_w_raw)
                  : 0u;
            break;
        }
        default: {
            // Group flagged Synthesized but no case here -- shouldn't happen.
            emit_nan = true;
            break;
        }
    }

    if (emit_nan) {
        *out = emit_nan_register(g.nan, off_in_u32);
        return;
    }
    *out = off_in_u32 == 0
         ? static_cast<uint16_t>(value >> 16)
         : static_cast<uint16_t>(value & 0xFFFFu);
}

TFModbusTCPExceptionCode SbseModbusProxy::pack_response(
    TFModbusTCPFunctionCode /*fc*/,
    uint16_t start_addr,
    uint16_t count,
    uint16_t *response_buf,
    const SynthesisInputs &inputs) const
{
    // The library has already bounds-checked count; we trust it and answer
    // every requested register. evcc's templates always pick FC according
    // to SMA's holding/input convention, but we don't distinguish: the
    // cached value is the same regardless of FC.
    for (uint16_t i = 0; i < count; ++i) {
        const uint16_t addr = static_cast<uint16_t>(start_addr + i);
        const size_t   idx  = find_group(addr);
        if (idx == NUM_GROUPS) {
            response_buf[i] = 0xFFFFu;  // unknown register -> uint32nan default
            continue;
        }
        write_register(idx, addr, inputs, &response_buf[i]);
    }
    return TFModbusTCPExceptionCode::Success;
}
