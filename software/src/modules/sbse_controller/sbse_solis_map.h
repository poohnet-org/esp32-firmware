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

// Solis S5/S6 hybrid inverter register map (Solis "ESINV-33000ID" hybrid
// protocol), as served by the emulating Modbus server
// (modbus_server_protocol = 2). Pure data mapping. Sources for the layout:
// the Pho3niX90/solis_modbus Home Assistant integration and evcc's
// solis-hybrid / solis-hybrid-s templates. The Tibber Bridge's Solis search
// reads 35000 (type) and 33004+15 (serial) on unit 1.
//
// Multi-register values are big-endian, high word first. Solis has no NaN
// convention: unknown values read 0.
//
// Readable windows (FC 3 and FC 4 alike; a read must lie entirely within
// one, everything else answers IllegalDataAddress):
//   33000..33299  live data (identification, PV, grid, battery, meter)
//   34000..34999  extended input registers; 34502 = 0xAA55 (remote
//                 dispatch offered), 34503 = dispatch version, rest 0
//   35000..35009  inverter type definition
//   43000..43999  settings -- shadow, 43000..43005 = live clock
//   44000..44299  remote dispatch settings -- shadow
// Writes are accepted in the two settings windows.
class SbseSolis final
{
public:
    SbseSolis() = delete;

    // 33001 / 33002 DSP / HMI firmware versions. Solis HMI versions look like
    // 0xFB00; the Tibber app asks for a firmware update below some
    // threshold, so report something newer than FB00.
    static constexpr uint16_t FIRMWARE_VERSION = 0xFB10;

    // 35000: high byte = protocol version, low byte = model code.
    // 0x40 = 1-phase HV hybrid (S5-EH1P class, matches the SBSE topology);
    // 0x80 would be the S6 1-phase HV hybrid.
    static constexpr uint16_t INVERTER_TYPE = 0x0340;

    // Remote control ("RC") registers.
    static constexpr uint16_t REG_STORAGE_CONTROL = 43110;   // storage mode bits
    static constexpr uint16_t REG_RC_DISCHARGE_W  = 43129;   // force discharge power, W/10
    static constexpr uint16_t REG_RC_FORCE        = 43135;   // 0 none, 1 charge, 2 discharge
    static constexpr uint16_t REG_RC_CHARGE_W     = 43136;   // force charge power, W/10
    static constexpr uint16_t REG_RC_TIMEOUT_MIN  = 43282;   // RC timeout [min]

    // Remote dispatch ("RD", protocol Ver3.4) registers. Offered via
    // 34502 = 0xAA55; the Tibber Bridge reads 34502+2, 34504 and 44100+17.
    static constexpr uint16_t REG_RD_CAPABILITY   = 34502;   // 0xAA55 = supported
    static constexpr uint16_t REG_RD_VERSION      = 34503;
    static constexpr uint16_t RD_CAPABILITY_MAGIC = 0xAA55;
    static constexpr uint16_t RD_VERSION          = 3;
    static constexpr uint16_t REG_RD_ENABLE       = 44100;   // 1 on, 0 off
    static constexpr uint16_t REG_RD_FAILSAFE_MIN = 44101;   // failsafe interval [min]
    static constexpr uint16_t REG_RD_MODE         = 44105;   // see RdMode
    static constexpr uint16_t REG_RD_POWER        = 44106;   // S32 (44106 high, 44107 low), W/10
    static constexpr uint16_t REG_RD_FIRST        = 44100;   // registers that re-trigger the apply
    static constexpr uint16_t REG_RD_LAST         = 44112;

    // 44105 modes. Power sign per mode (solis_modbus DISPATCH_MODES):
    // BatteryPower: + = charge, - = discharge; GridPower / GridPortPower:
    // + = export, - = import.
    enum class RdMode : uint16_t {
        Hold            = 1,
        BatteryPower    = 2,
        GridPower       = 3,
        GridPortPower   = 4,
        SelfConsumption = 5,
        FeedInPriority  = 6,
    };

    static constexpr uint16_t RC_FORCE_NONE      = 0;
    static constexpr uint16_t RC_FORCE_CHARGE    = 1;
    static constexpr uint16_t RC_FORCE_DISCHARGE = 2;

    struct Live {
        int32_t  grid_w;            // positive = import (controller convention)
        int32_t  battery_w;         // positive = discharging
        uint8_t  soc_pct;           // 255 = unknown
        int32_t  pv_w;              // -1 = unknown
        uint64_t pv_total_wh;       // UINT64_MAX = unknown
        uint64_t bat_charge_wh;     // UINT64_MAX = unknown
        uint64_t bat_discharge_wh;  // UINT64_MAX = unknown
        uint32_t bat_voltage_cv;    // battery voltage [0.01 V], 0 = unknown
        int32_t  bat_current_ma;    // battery current [mA], sign ignored; INT32_MIN = unknown
        int32_t  bat_temp_dc;       // battery temperature [0.1 degC], INT32_MIN = unknown
        int32_t  max_charge_w;      // operator caps, for the BMS current limits
        int32_t  max_discharge_w;
        const char *serial;         // ASCII, up to 30 chars (33004+15 regs)
        bool     time_valid;
        uint8_t  year, month, day, hour, minute, second;   // year since 2000
    };

    // Settings shadow (43000..43999 and 44000..44299). Construct with
    // settings_default. Time-of-use slots alone are 84 registers.
    using Settings = SbseRegisterShadow<160>;
    [[gnu::const]] static uint16_t settings_default(uint16_t addr);

    static TFModbusTCPExceptionCode read(uint16_t start, uint16_t count, uint16_t *out,
                                         const Live &live, const Settings &settings);

    [[gnu::const]] static bool is_writable(uint16_t start, uint16_t count);
};
