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

#include "sbse_solis_map.h"

#include <string.h>

#include "sbse_register_writer.h"

#include "gcc_warnings.h"

struct SbseSolisWindow {
    uint16_t lo;
    uint16_t hi;   // exclusive
};

static constexpr SbseSolisWindow SOLIS_WIN_LIVE      = {33000, 33300};
static constexpr SbseSolisWindow SOLIS_WIN_DISPATCH  = {34500, 34510};
static constexpr SbseSolisWindow SOLIS_WIN_TYPE      = {35000, 35010};
static constexpr SbseSolisWindow SOLIS_WIN_SETTINGS  = {43000, 44000};
static constexpr SbseSolisWindow SOLIS_WIN_RD        = {44000, 44300};

static bool solis_inside(const SbseSolisWindow &w, uint32_t start, uint32_t count)
{
    return start >= w.lo && start + count <= w.hi;
}

// Wh -> kWh U32 (Solis total counters have 1 kWh resolution); unknown -> 0.
static uint32_t solis_wh_to_kwh(uint64_t wh)
{
    return wh == UINT64_MAX ? 0u : static_cast<uint32_t>(wh / 1000u);
}

static void solis_fill_clock(SbseRegisterWriter &w, uint16_t base, const SbseSolis::Live &live)
{
    if (!live.time_valid) {
        return;
    }
    w.u16(base,                              live.year);
    w.u16(static_cast<uint16_t>(base + 1),   live.month);
    w.u16(static_cast<uint16_t>(base + 2),   live.day);
    w.u16(static_cast<uint16_t>(base + 3),   live.hour);
    w.u16(static_cast<uint16_t>(base + 4),   live.minute);
    w.u16(static_cast<uint16_t>(base + 5),   live.second);
}

static void solis_fill_live(SbseRegisterWriter &w, const SbseSolis::Live &live,
                            const SbseSolis::Settings &settings)
{
    const bool    pv_known = live.pv_w >= 0;
    const int32_t pv_w     = pv_known ? live.pv_w : 0;
    const int32_t export_w = -live.grid_w;
    const int32_t inverter_w = pv_w + live.battery_w;          // AC output, losses ignored
    int32_t       load_w   = pv_w + live.battery_w + live.grid_w;
    if (load_w < 0) load_w = 0;
    if (load_w > 0xFFFF) load_w = 0xFFFF;
    const int32_t bat_abs  = live.battery_w < 0 ? -live.battery_w : live.battery_w;

    // Identification.
    w.u16(33000, SbseSolis::INVERTER_TYPE);                    // model no
    w.u16(33001, 1);                                           // DSP version
    w.u16(33002, 1);                                           // HMI version
    w.u16(33003, SbseSolis::INVERTER_TYPE >> 8);               // protocol version
    w.str(33004, 16, live.serial, '\0');                       // serial, 2 chars/reg
    solis_fill_clock(w, 33022, live);

    w.u32(33029, solis_wh_to_kwh(live.pv_total_wh));           // PV total energy [kWh]
    w.u32(33057, static_cast<uint32_t>(pv_w));                 // total PV power [W]
    w.s32(33079, inverter_w);                                  // active power [W]
    w.u16(33095, 3);                                           // status: generating

    w.s32(33130, export_w);                                    // meter active power (export +)
    w.u16(33132, settings.get(SbseSolis::REG_STORAGE_CONTROL)); // storage control mirror
    w.u16(33135, live.battery_w > 0 ? 1 : 0);                  // battery dir: 0 charge, 1 discharge
    w.u16(33139, live.soc_pct == 255 ? 0 : live.soc_pct);      // SoC [%]
    w.u16(33140, 100);                                         // SoH [%]
    // BMS current limits [0.1 A]: the SBSE doesn't expose them; report a
    // non-limiting 100 A so clients don't think the battery is blocked.
    w.u16(33143, 1000);
    w.u16(33144, 1000);
    w.u16(33147, static_cast<uint16_t>(load_w));               // household load [W]
    // Battery power: magnitude with the sign of "charging". evcc's RHI
    // template negates it (signed), the S-series one uses |P| x 33135 --
    // both give discharge positive with this encoding.
    w.s32(33149, live.battery_w > 0 ? bat_abs : -bat_abs);
    w.s32(33151, inverter_w);                                  // AC grid port power
    w.u32(33165, solis_wh_to_kwh(live.bat_discharge_wh));      // battery discharge total [kWh]

    w.s32(33263, export_w);                                    // meter total active power (export +)
    w.s16(33281, 100);                                         // meter power factor [0.01]
}

uint16_t SbseSolis::settings_default(uint16_t addr)
{
    switch (addr) {
        case 43007:            return 190;   // inverter power: on
        case 43024:            return 10;    // battery reserve SoC [%]
        case REG_STORAGE_CONTROL: return 1;  // self-use
        case 43117:            return 1000;  // max charge current [0.1 A]
        case 43118:            return 1000;  // max discharge current [0.1 A]
        case REG_RC_TIMEOUT_MIN: return 5;   // RC timeout [min]
        default:               return 0;
    }
}

bool SbseSolis::is_writable(uint16_t start, uint16_t count)
{
    return count > 0
        && (solis_inside(SOLIS_WIN_SETTINGS, start, count) || solis_inside(SOLIS_WIN_RD, start, count));
}

TFModbusTCPExceptionCode SbseSolis::read(uint16_t start, uint16_t count, uint16_t *out,
                                         const Live &live, const Settings &settings)
{
    if (count == 0) {
        return TFModbusTCPExceptionCode::IllegalDataAddress;
    }

    memset(out, 0, sizeof(uint16_t) * count);
    SbseRegisterWriter w(start, count, out);

    if (solis_inside(SOLIS_WIN_LIVE, start, count)) {
        solis_fill_live(w, live, settings);
    } else if (solis_inside(SOLIS_WIN_DISPATCH, start, count)) {
        // 34502 != 0xAA55: remote dispatch (44100+) not offered.
    } else if (solis_inside(SOLIS_WIN_TYPE, start, count)) {
        w.u16(35000, INVERTER_TYPE);
    } else if (solis_inside(SOLIS_WIN_SETTINGS, start, count)
               || solis_inside(SOLIS_WIN_RD, start, count)) {
        for (uint16_t i = 0; i < count; ++i) {
            out[i] = settings.get(static_cast<uint16_t>(start + i));
        }
        solis_fill_clock(w, 43000, live);
    } else {
        return TFModbusTCPExceptionCode::IllegalDataAddress;
    }
    return TFModbusTCPExceptionCode::Success;
}
