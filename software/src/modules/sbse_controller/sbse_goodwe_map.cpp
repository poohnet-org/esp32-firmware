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

#include "sbse_goodwe_map.h"

#include <math.h>
#include <string.h>

#include "sbse_register_writer.h"

#include "gcc_warnings.h"

struct SbseGoodweWindow {
    uint16_t lo;
    uint16_t hi;   // exclusive
};

static constexpr SbseGoodweWindow WIN_DEVICE_INFO = {35000, 35060};
static constexpr SbseGoodweWindow WIN_RUNTIME     = {35100, 35225};
static constexpr SbseGoodweWindow WIN_METER       = {36000, 36125};
static constexpr SbseGoodweWindow WIN_BMS         = {37000, 37024};
static constexpr SbseGoodweWindow WIN_SETTINGS    = {SbseGoodwe::SETTINGS_LO, SbseGoodwe::SETTINGS_HI};

static bool inside(const SbseGoodweWindow &w, uint32_t start, uint32_t count)
{
    return start >= w.lo && start + count <= w.hi;
}

static constexpr uint64_t UNKNOWN64 = UINT64_MAX;

// Wh -> GoodWe 0.1 kWh U32 counter; unknown -> 0xFFFFFFFF (evcc uint32nan).
static uint32_t wh_to_dkwh(uint64_t wh)
{
    if (wh == UNKNOWN64) return 0xFFFFFFFFu;
    const uint64_t v = wh / 100u;
    return v >= 0xFFFFFFFFu ? 0xFFFFFFFEu : static_cast<uint32_t>(v);
}

// Wh -> 0.01 kWh U64 counter (meter 36092+); unknown -> all ones.
static uint64_t wh_to_ckwh(uint64_t wh)
{
    return wh == UNKNOWN64 ? UNKNOWN64 : wh / 10u;
}

// Wh -> float Wh (meter 36015/36017, read by evcc with scale 0.001 -> kWh).
static float wh_to_float(uint64_t wh)
{
    return wh == UNKNOWN64 ? NAN : static_cast<float>(wh);
}

static void fill_device_info(SbseRegisterWriter &w, const SbseGoodwe::Live &live)
{
    w.u16(35000, 1);                                                // modbus protocol version
    w.u16(35001, live.rated_w > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(live.rated_w));
    w.u16(35002, 1);                                                // AC output: 3-phase 4-wire
    w.str(35003, 8, live.serial, ' ');                                   // 16 chars
    w.str(35011, 5, live.model, ' ');                                    // 10 chars
    w.u16(35016, 9);                                                // DSP1 version
    w.u16(35017, 9);                                                // DSP2 version
    w.u16(35018, 0);                                                // DSP svn
    w.u16(35019, 18);                                               // ARM version
    w.u16(35020, 0);                                                // ARM svn
    w.str(35021, 6, "SBSE-EMU-1.0", ' ');                                // firmware
    w.str(35027, 6, "SBSE-EMU-1.0", ' ');                                // ARM firmware
}

static void fill_runtime(SbseRegisterWriter &w, const SbseGoodwe::Live &live)
{
    if (live.time_valid) {
        w.u16(35100, static_cast<uint16_t>((live.year   << 8) | live.month));
        w.u16(35101, static_cast<uint16_t>((live.day    << 8) | live.hour));
        w.u16(35102, static_cast<uint16_t>((live.minute << 8) | live.second));
    }

    const bool    pv_known = live.pv_w >= 0;
    const int32_t pv_w     = pv_known ? live.pv_w : 0;

    // PV1 only: the SBSE proxy cache carries string 1 power. V/I unknown (0).
    w.u32(35105, pv_known ? static_cast<uint32_t>(pv_w) : 0xFFFFFFFFu);
    // 35120 low byte = PV1 mode: 1 = connected without power, 2 = producing.
    if (pv_known) {
        w.u16(35120, pv_w > 0 ? 2 : 1);
    }

    w.u16(35136, 1);                                                // grid mode: connected

    const int32_t inverter_w = pv_w + live.battery_w;               // AC output, losses ignored
    const int32_t export_w   = -live.grid_w;
    int32_t       load_w     = pv_w + live.battery_w + live.grid_w;
    if (load_w < 0) load_w = 0;

    w.s32(35137, inverter_w);                                       // total inverter power
    w.s32(35139, export_w);                                         // active power (export +)
    w.s32(35171, load_w);                                           // total load power

    w.s32(35182, live.battery_w);                                   // battery power (discharge +)
    // Battery mode: 1 standby, 2 discharge, 3 charge.
    w.u16(35184, live.battery_w > 0 ? 2 : (live.battery_w < 0 ? 3 : 1));
    w.u16(35187, 1);                                                // work mode: normal (on-grid)

    w.u32(35191, wh_to_dkwh(live.pv_total_wh));                     // total PV generation
    w.u32(35195, wh_to_dkwh(live.grid_export_wh));                  // total export
    w.u32(35200, wh_to_dkwh(live.grid_import_wh));                  // total import
    w.u32(35206, wh_to_dkwh(live.bat_charge_wh));                   // total battery charge
    w.u32(35209, wh_to_dkwh(live.bat_discharge_wh));                // total battery discharge
}

static void fill_meter(SbseRegisterWriter &w, const SbseGoodwe::Live &live)
{
    const int32_t export_w = -live.grid_w;

    w.u16(36000, 1);                                                // commode
    w.u16(36003, 1);                                                // meter test status: correct
    w.u16(36004, 1);                                                // meter comm status: OK
    w.s16(36008, export_w);                                         // total active power (S16)
    w.u16(36013, 1000);                                             // total power factor 1.000
    w.f32(36015, wh_to_float(live.grid_export_wh));                 // total export [Wh]
    w.f32(36017, wh_to_float(live.grid_import_wh));                 // total import [Wh]
    w.s32(36025, export_w);                                         // total active power (S32)
    w.u16(36043, 2);                                                // meter type: 3P4W
    w.u64(36104, wh_to_ckwh(live.grid_export_wh));                  // total export [0.01 kWh]
    w.u64(36120, wh_to_ckwh(live.grid_import_wh));                  // total import [0.01 kWh]
}

static void fill_bms(SbseRegisterWriter &w, const SbseGoodwe::Live &live)
{
    w.u16(37000, 1);                                                // BMS present
    w.u16(37007, live.soc_pct == 255 ? 0xFFFFu : live.soc_pct);     // SoC (evcc uint16nan)
    w.u16(37008, 100);                                              // SoH
}

const char *SbseGoodwe::ems_mode_name(uint16_t mode)
{
    switch (static_cast<EmsMode>(mode)) {
        case EmsMode::Auto:             return "auto";
        case EmsMode::ChargePv:         return "charge_pv";
        case EmsMode::DischargePv:      return "discharge_pv";
        case EmsMode::ImportAc:         return "import_ac";
        case EmsMode::ExportAc:         return "export_ac";
        case EmsMode::Conserve:         return "conserve";
        case EmsMode::OffGrid:          return "off_grid";
        case EmsMode::BatteryStandby:   return "battery_standby";
        case EmsMode::BuyPower:         return "buy_power";
        case EmsMode::SellPower:        return "sell_power";
        case EmsMode::ChargeBattery:    return "charge_battery";
        case EmsMode::DischargeBattery: return "discharge_battery";
        default:                        return "unknown";
    }
}

bool SbseGoodwe::ems_mode_supported(uint16_t mode)
{
    return mode >= static_cast<uint16_t>(EmsMode::Auto)
        && mode <= static_cast<uint16_t>(EmsMode::DischargeBattery)
        && mode != static_cast<uint16_t>(EmsMode::OffGrid);
}

uint16_t SbseGoodwe::settings_default(uint16_t addr)
{
    return addr == REG_EMS_MODE ? static_cast<uint16_t>(EmsMode::Auto) : 0;
}

bool SbseGoodwe::is_writable(uint16_t start, uint16_t count)
{
    return count > 0 && inside(WIN_SETTINGS, start, count);
}

TFModbusTCPExceptionCode SbseGoodwe::read(uint16_t start, uint16_t count, uint16_t *out,
                              const SbseGoodwe::Live &live, const Settings &settings)
{
    if (count == 0) {
        return TFModbusTCPExceptionCode::IllegalDataAddress;
    }

    memset(out, 0, sizeof(uint16_t) * count);
    SbseRegisterWriter w(start, count, out);

    if (inside(WIN_DEVICE_INFO, start, count)) {
        fill_device_info(w, live);
    } else if (inside(WIN_RUNTIME, start, count)) {
        fill_runtime(w, live);
    } else if (inside(WIN_METER, start, count)) {
        fill_meter(w, live);
    } else if (inside(WIN_BMS, start, count)) {
        fill_bms(w, live);
    } else if (inside(WIN_SETTINGS, start, count)) {
        for (uint16_t i = 0; i < count; ++i) {
            out[i] = settings.get(static_cast<uint16_t>(start + i));
        }
    } else {
        return TFModbusTCPExceptionCode::IllegalDataAddress;
    }
    return TFModbusTCPExceptionCode::Success;
}
