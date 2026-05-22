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

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <TFTools/Micros.h>
#include "TFModbusTCPCommon.h"

// SbseModbusProxy keeps a small cache of SMA-hybrid-flavoured register
// values so the SbseModbusServer can answer evcc's reads (FC 3 / FC 4)
// without forwarding each request to the upstream inverter. The cache is
// populated by a round-robin polling cycle that the controller drives,
// re-using its existing TFModbusTCPSharedClient connection.
//
// Four hot registers are synthesized from the controller's live state at
// read time rather than polled, so the values evcc gets for them never lag
// behind the controller's most recent sample:
//
//   30775  GridMs.TotW            <- grid_w_raw       (int32nan)
//   30845  Bat.ChaStt             <- soc_pct          (uint32nan)
//   31393  BatChrg.CurBatCha      <- max(0, -bat_w)   (uint32nan)
//   31395  BatDsch.CurBatDsch     <- max(0,  bat_w)   (uint32nan)
//
// Everything else (cumulative energies, per-phase W/A, PV DC) is polled
// from the appropriate upstream unit (inverter at 3, grid meter at 2) and
// served verbatim from the cache; uncovered or not-yet-polled addresses
// answer with the per-type SMA NaN sentinel.
class SbseModbusProxy final
{
public:
    // Synthesis inputs supplied by the controller on every pack_response()
    // call. Kept tiny so the controller can fill it cheaply from cached
    // fields without going through Config::get().
    struct SynthesisInputs {
        int32_t grid_w_raw;      // matches SMA "GridMs.TotW" convention: positive = import
        int32_t battery_w_raw;   // positive = discharging, negative = charging
        uint8_t soc_pct;         // 255 = "not yet known" -> NaN
    };

    // Upstream Modbus units used to populate the cached groups. Matches the
    // existing control-loop constants; the proxy re-declares them here so
    // it doesn't drag the entire register-map header in.
    static constexpr uint8_t UPSTREAM_INVERTER_UNIT   = 3;
    static constexpr uint8_t UPSTREAM_GRID_METER_UNIT = 2;

    // Sizes exposed for static_asserts in the .cpp that verify the manual
    // cache layout fits within CACHE_SIZE.
    static constexpr size_t CACHE_SIZE = 64;     // sum of cached group reg_counts (≤ 64)
    static constexpr size_t MAX_GROUPS = 16;     // upper bound for fresh_mask

    SbseModbusProxy() = default;
    SbseModbusProxy(const SbseModbusProxy &) = delete;
    SbseModbusProxy &operator=(const SbseModbusProxy &) = delete;

    // Drop every group's freshness marker. Future reads on un-repolled
    // groups will return the NaN sentinel. Called on upstream disconnect.
    void invalidate_all();

    // Pick the next group that needs an upstream read. Returns false when
    // a poll is already in flight or no cached groups exist. Advances the
    // round-robin index eagerly, so a failing group doesn't block the rest.
    // Marks the proxy "poll in flight" -- caller MUST call mark_group_done()
    // when the transaction settles.
    bool next_poll(size_t *group_idx, uint8_t *upstream_unit,
                   uint16_t *addr, uint16_t *reg_count,
                   uint16_t **cache_dst);

    // Settle the in-flight poll. On success the group is marked fresh; on
    // failure the cache retains whatever it had (NaN sentinel until first
    // success, last-known value afterwards).
    void mark_group_done(size_t group_idx, bool success);

    // Fill response_buf[0..count-1] with the values for [start_addr,
    // start_addr+count). Always returns Success; uncovered registers get
    // the per-type NaN sentinel.
    TFModbusTCPExceptionCode pack_response(TFModbusTCPFunctionCode fc,
                                           uint16_t start_addr,
                                           uint16_t count,
                                           uint16_t *response_buf,
                                           const SynthesisInputs &inputs) const;

private:
    enum class Source : uint8_t { Cached, Synthesized };
    enum class NaNStyle : uint8_t {
        UnsignedMax,   // 0xFFFFFFFF / 0xFFFFFFFFFFFFFFFF -- uint32nan and uint64nan
        Int32Negative, // 0x80000000 -- int32nan
    };

    struct Group {
        uint16_t addr;          // SMA register address
        uint16_t reg_count;     // 2 (uint32), 4 (uint64), or wider blocks
        uint8_t  upstream_unit; // ignored for Synthesized
        uint8_t  cache_offset;  // index into cache[]; ignored for Synthesized
        Source   source;
        NaNStyle nan;           // sentinel kind for stale/uncovered registers
    };

    static const Group  GROUPS[];
    static const size_t NUM_GROUPS;

    // Locate the group whose [addr, addr+reg_count) range contains `addr`.
    // Returns NUM_GROUPS if no group covers it.
    size_t find_group(uint16_t addr) const;

    // Per-type NaN sentinel for one 16-bit register. off_in_u32 is 0 for
    // the high half of a uint32 / uint64, 1 for the low half.
    static uint16_t emit_nan_register(NaNStyle nan, uint16_t off_in_u32);

    // Write one register's worth of bytes to *out, handling cached / synth
    // / NaN-sentinel paths.
    void write_register(size_t group_idx, uint16_t addr,
                        const SynthesisInputs &inputs,
                        uint16_t *out) const;

    uint16_t cache[CACHE_SIZE]            = {};
    uint32_t fresh_mask                   = 0;   // bit i = group i has at least one good poll
    size_t   next_idx                     = 0;
    bool     poll_in_flight               = false;
};
