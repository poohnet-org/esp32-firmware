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

#include <stddef.h>
#include <stdint.h>

#include "TFModbusTCPCommon.h"
#include "sbse_register_shadow.h"

// GoodWe ET hybrid inverter register map, as served by the emulating Modbus
// server (modbus_server_protocol = 1). Pure data mapping: no networking, no
// controller semantics. Sources for the layout: the Tinkerforge GoodWe meter
// table (meters_modbus_tcp/goodwe.py), evcc's goodwe-hybrid template and the
// goodwe Python library (et.py) -- the client set we expect to see.
//
// All multi-register values are big-endian, high word first. Power signs
// follow GoodWe: battery positive = discharging, grid/meter positive =
// export. 32-bit powers are sign-extended, so clients that read only the
// low word (the goodwe library reads 35140 / 35138 as S16) see the same
// value as long as |P| < 32768 W.
//
// Readable windows (a read must lie entirely within one; everything else
// answers IllegalDataAddress, which is how real GoodWe firmware signals an
// unsupported block -- the goodwe library probes optional blocks that way):
//   35000..35059  device info (identification, serial, model, versions)
//   35100..35224  runtime data (PV, grid, load, battery, energy totals)
//   36000..36124  external meter
//   37000..37023  BMS (SoC)
//   45000..47999  settings -- read back from Settings (0 default)
// Writes are accepted only inside the settings window.
//
// A class with static members only, because the project's warning set
// forbids namespaces.
class SbseGoodwe final
{
public:
    SbseGoodwe() = delete;

    static constexpr uint16_t REG_EMS_MODE  = 47511;   // EMSPowerMode, see EmsMode
    static constexpr uint16_t REG_EMS_POWER = 47512;   // EMSPowerSet [W], "Xset"

    static constexpr uint16_t SETTINGS_LO   = 45000;
    static constexpr uint16_t SETTINGS_HI   = 48000;   // exclusive

    // EMS modes written to 47511 (numbering from the goodwe library's EMSMode).
    enum class EmsMode : uint16_t {
        Auto             = 1,   // self-use
        ChargePv         = 2,   // PBattery = Xmax + PV (charge); Xmax 0 = PV only
        DischargePv      = 3,   // PBattery = Xmax (discharge)
        ImportAc         = 4,   // PBattery = Xset + PV (charge, grid first)
        ExportAc         = 5,   // PBattery = Xset (discharge)
        Conserve         = 6,   // PBattery = PV (charge), never discharge on-grid
        OffGrid          = 7,   // forced off-grid -- not supported
        BatteryStandby   = 8,   // PBattery = 0
        BuyPower         = 9,   // grid import controlled to Xset
        SellPower        = 10,  // grid export controlled to Xset
        ChargeBattery    = 11,  // PBattery = Xset (charge)
        DischargeBattery = 12,  // PBattery = Xset (discharge)
    };

    [[gnu::const]] static const char *ems_mode_name(uint16_t mode);
    [[gnu::const]] static bool ems_mode_supported(uint16_t mode);

    // Live values the controller hands to every read. Unknown values use the
    // sentinels noted per field; the map turns them into GoodWe/evcc NaN
    // patterns (0xFFFF..., float NaN) or 0 where the protocol has no NaN.
    struct Live {
        int32_t  grid_w;            // positive = import (controller convention)
        int32_t  battery_w;         // positive = discharging
        uint8_t  soc_pct;           // 255 = unknown
        int32_t  pv_w;              // -1 = unknown
        uint32_t rated_w;           // inverter rated power
        uint64_t pv_total_wh;       // UINT64_MAX = unknown
        uint64_t grid_import_wh;    // UINT64_MAX = unknown
        uint64_t grid_export_wh;    // UINT64_MAX = unknown
        uint64_t bat_charge_wh;     // UINT64_MAX = unknown
        uint64_t bat_discharge_wh;  // UINT64_MAX = unknown
        const char *serial;         // 16 ASCII chars, contains "ETU" (ET family)
        const char *model;          // ASCII, served as 10 chars, e.g. "GW5K-ET"
        bool     time_valid;
        uint8_t  year, month, day, hour, minute, second;   // year since 2000
    };

    // Shadow of the writable settings window (clients write 47511 / 47512
    // and read them back). Construct with settings_default.
    using Settings = SbseRegisterShadow<32>;
    [[gnu::const]] static uint16_t settings_default(uint16_t addr);

    // Fill out[0..count-1] for a read of [start, start+count). Returns
    // IllegalDataAddress if the range isn't entirely inside one readable window.
    static TFModbusTCPExceptionCode read(uint16_t start, uint16_t count, uint16_t *out,
                                         const Live &live, const Settings &settings);

    // Is [start, start+count) entirely inside the writable settings window?
    [[gnu::const]] static bool is_writable(uint16_t start, uint16_t count);
};
