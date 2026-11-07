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
static constexpr SbseSolisWindow SOLIS_WIN_EXT       = {34000, 35000};
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

// Battery current limit [0.1 A] for a power cap: max_w / V, rounded up so
// V x I is never below the cap. The SBSE doesn't expose BMS limits; clients
// compute V x I as the available power. 100.0 A while the voltage is unknown.
static uint16_t solis_limit_da(const SbseSolis::Live &live, int32_t max_w)
{
    const uint32_t v_cv = live.bat_voltage_cv;
    if (v_cv == 0) return 1000;
    const uint32_t p  = static_cast<uint32_t>(max_w > 0 ? max_w : 0);
    const uint32_t da = (p * 1000u + v_cv - 1u) / v_cv;
    return static_cast<uint16_t>(da > 0xFFFFu ? 0xFFFFu : da);
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
    w.u16(33001, SbseSolis::FIRMWARE_VERSION);                 // DSP version
    w.u16(33002, SbseSolis::FIRMWARE_VERSION);                 // HMI version
    w.u16(33003, SbseSolis::INVERTER_TYPE >> 8);               // protocol version
    w.str(33004, 16, live.serial, '\0');                       // serial, 2 chars/reg
    solis_fill_clock(w, 33022, live);

    w.u32(33029, solis_wh_to_kwh(live.pv_total_wh));           // PV total energy [kWh]
    w.u32(33057, static_cast<uint32_t>(pv_w));                 // total PV power [W]
    w.s32(33079, inverter_w);                                  // active power [W]
    w.u16(33095, 3);                                           // status: generating

    w.s32(33130, export_w);                                    // meter active power (export +)
    w.u16(33132, settings.get(SbseSolis::REG_STORAGE_CONTROL)); // storage control mirror
    // Battery voltage / current / temperature from the SBSE (SMA 30843+).
    const uint32_t v_cv = live.bat_voltage_cv;
    if (live.bat_temp_dc != INT32_MIN) {
        w.s16(33043, live.bat_temp_dc);                        // battery temperature [0.1 degC]
    }
    if (v_cv > 0) {
        w.u16(33133, static_cast<uint16_t>(v_cv / 10u > 0xFFFFu ? 0xFFFFu : v_cv / 10u));   // [0.1 V]
        w.u16(33141, static_cast<uint16_t>(v_cv > 0xFFFFu ? 0xFFFFu : v_cv));               // BMS [0.01 V]
    }
    if (live.bat_current_ma != INT32_MIN) {
        const int32_t i_ma = live.bat_current_ma < 0 ? -live.bat_current_ma : live.bat_current_ma;
        w.u16(33134, static_cast<uint16_t>(i_ma / 100 > 0xFFFF ? 0xFFFF : i_ma / 100));    // [0.1 A]
        w.s16(33142, (live.battery_w > 0 ? 1 : -1) * (i_ma / 100));                         // BMS [0.1 A]
    }
    w.u16(33135, live.battery_w > 0 ? 1 : 0);                  // battery dir: 0 charge, 1 discharge
    w.u16(33139, live.soc_pct == 255 ? 0 : live.soc_pct);      // SoC [%]
    w.u16(33140, 100);                                         // SoH [%]
    // BMS current limits [0.1 A], derived from the operator's caps.
    w.u16(33143, solis_limit_da(live, live.max_charge_w));
    w.u16(33144, solis_limit_da(live, live.max_discharge_w));
    w.u16(33147, static_cast<uint16_t>(load_w));               // household load [W]
    // Battery power: signed, discharge positive. The direction flag 33135 is
    // authoritative for the Tibber app and evcc's S-series template (they use
    // |P|); evcc's RHI template would read the sign inverted.
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
        // SoC window: the Tibber app wants >= 50 % between min and max.
        case 43010:            return 100;   // max charge SoC [%]
        case 43011:            return 10;    // overdischarge (min) SoC [%]
        case 43018:            return 5;     // force-charge SoC [%]
        case 44109:            return 10;    // dispatch SoC lower limit [%]
        case 44110:            return 100;   // dispatch SoC upper limit [%]
        case 44111:            return 10;    // dispatch reserve SoC [%]
        case 43024:            return 10;    // battery reserve SoC [%]
        case REG_STORAGE_CONTROL: return 1;  // self-use
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
    } else if (solis_inside(SOLIS_WIN_EXT, start, count)) {
        // Extended block. The Tibber Bridge reads 34391, 34502..34504,
        // 34621 and 34753 here; only the dispatch capability is modelled.
        w.u16(REG_RD_CAPABILITY, RD_CAPABILITY_MAGIC);
        w.u16(REG_RD_VERSION,    RD_VERSION);
    } else if (solis_inside(SOLIS_WIN_TYPE, start, count)) {
        w.u16(35000, INVERTER_TYPE);
    } else if (solis_inside(SOLIS_WIN_SETTINGS, start, count)
               || solis_inside(SOLIS_WIN_RD, start, count)) {
        for (uint16_t i = 0; i < count; ++i) {
            out[i] = settings.get(static_cast<uint16_t>(start + i));
        }
        solis_fill_clock(w, 43000, live);
        // Configured charge / discharge current limits [0.1 A]: report the
        // same value as the BMS limits (33143 / 33144) unless the client set
        // its own. A configured limit far above the BMS limit reads as "BMS
        // is throttling right now" -- the Tibber app then warns about
        // reduced discharge power.
        static constexpr uint16_t CHARGE_LIMIT_REGS[]    = {43012, 43117};
        static constexpr uint16_t DISCHARGE_LIMIT_REGS[] = {43013, 43118};
        for (uint16_t addr : CHARGE_LIMIT_REGS) {
            if (!settings.contains(addr)) w.u16(addr, solis_limit_da(live, live.max_charge_w));
        }
        for (uint16_t addr : DISCHARGE_LIMIT_REGS) {
            if (!settings.contains(addr)) w.u16(addr, solis_limit_da(live, live.max_discharge_w));
        }
    } else {
        return TFModbusTCPExceptionCode::IllegalDataAddress;
    }
    return TFModbusTCPExceptionCode::Success;
}
