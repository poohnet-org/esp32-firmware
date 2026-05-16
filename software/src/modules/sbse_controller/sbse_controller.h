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

#include <TFModbusTCPClient.h>
#include <TFModbusTCPClientPool.h>
#include <TFTools/Micros.h>

#include "config.h"
#include "module.h"
#include "modules/network_lib/generic_tcp_client_pool_connector.h"
#include "sbse_modbus_server.h"
#include "sbse_trace_history.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#include "gcc_warnings.h"
#pragma GCC diagnostic ignored "-Weffc++"
#endif

class SbseController final : public IModule, protected GenericTCPClientPoolConnector
{
public:
    SbseController();

    void pre_setup() override;
    void setup() override;
    void register_urls() override;
    void register_events() override;
    void pre_reboot() override;

    enum class Mode : uint8_t {
        Disabled        = 0,
        NotConnected    = 1,
        Stale           = 2,
        Running         = 3,
        Faulted         = 4,
        Paused          = 5,
        Safety          = 6,
        ForceCharge     = 7,
        ForceDischarge  = 8,
        Blocked         = 9,    // max_charge_w == 0 && max_discharge_w == 0
        BlockCharge     = 10,   // max_charge_w == 0 (charge blocked, discharge allowed)
        BlockDischarge  = 11,   // max_discharge_w == 0 (discharge blocked, charge allowed)
    };

private:
    // GenericTCPClientPoolConnector
    void connect_callback(TFGenericTCPClientConnectResult result) override;
    void disconnect_callback(TFGenericTCPClientDisconnectReason reason) override;

    // Cycle
    void tick();
    bool begin_cycle();                                // pre-condition checks; sets cycle_in_flight on success
    void read_grid_power();
    void read_battery_power();
    void read_soc();
    void compute_and_write();
    void send_setpoint(int32_t watts);
    void send_release();
    void send_safety_zero();

    // SMA Modbus TCP server -- the server class itself is just the protocol
    // adapter; these methods carry the semantics (sticky OpMod, force-mode,
    // active_config wiring, watchdog).
    TFModbusTCPExceptionCode on_modbus_op_mod_write(uint32_t op_mod);
    TFModbusTCPExceptionCode on_modbus_setpoint_write(const uint16_t *regs10);
    void apply_modbus_setpoint_block(const uint16_t *data_values);
    void revert_modbus_overrides();      // watchdog expiry / operator takeover
    void watchdog_tick();
    void cycle_failed(const char *where,
                      TFModbusTCPClientTransactionResult result,
                      const char *error_message);
    void finish_cycle(Mode mode);

    void publish_setpoint(int32_t watts);
    void publish_mode(Mode mode);
    Mode current_running_mode() const;

    // Reconfiguration helpers
    void load_init_only_from_config();         // init-only fields (host, port, tick_ms, ...)
    void apply_runtime_from_active();          // live-tunable cached fields <- active_config
    void copy_live_tunable_to_active();        // active_config <- live-tunable subset of config
    void apply_enabled();

    // --- persistent boot defaults ---
    ConfigRoot config;

    // --- live-tunable runtime overlay (NOT persisted; survives only in RAM) ---
    ConfigRoot active_config;

    // --- runtime state ---
    ConfigRoot state;

    // --- cached config (refreshed on update; avoids hot-path Config::get()) ---
    bool     enabled            = false;
    millis_t tick_ms            = 300_ms;
    millis_t soc_interval_ms    = 1000_ms;
    int32_t  target_grid_w      = 0;
    int32_t  max_charge_w       = 5000;
    int32_t  max_discharge_w    = 5000;
    float    kp                 = 1.0f;
    float    kd                 = 0.0f;          // D-on-measurement gain; 0 disables D
    float    alpha_grid         = 0.30f;
    float    alpha_setpoint     = 0.70f;
    int32_t  deadband_w         = 50;
    uint32_t safety_zero_after_failures = 5;  // 0 disables the safety net
    bool     simulation_mode    = false;       // skips actual Modbus writes when true

    // SMA Modbus TCP server -- the network/protocol adapter. Its persistent
    // config is mirrored into the controller's cached fields below so the
    // dispatch handlers can read them without going through the server.
    SbseModbusServer modbus_server;
    bool     modbus_server_enabled    = false;
    uint16_t modbus_server_port       = 502;
    uint8_t  modbus_server_unit_id    = 3;       // 0 = accept any unit id
    uint32_t modbus_server_watchdog_ms = 60000;  // 0 disables
    bool     modbus_server_use_grid_spt = false; // mirror GridWSpt -> target_grid_w

    // Sticky OpMod (40236) latched between writes. 2424 = Default/Normal (P loop).
    // 2289 = Battery charging (force-charge). 2290 = Battery discharging.
    uint16_t modbus_op_mod           = 2424;

    // Non-zero -> bypass the P controller and command this value directly
    // (positive = discharge, negative = charge). Cleared by the watchdog,
    // operator takeover, or a 2424 OpMod write.
    int32_t  modbus_force_w          = 0;

    // True while a Modbus client is the most recent source of truth.
    bool     modbus_active           = false;
    micros_t last_modbus_write_us    = -1_us;

    // --- runtime ---
    uint64_t tick_task_id       = 0;
    bool     cycle_in_flight    = false;
    bool     paused             = false;
    micros_t paused_until       = -1_us;

    micros_t last_soc_read_ok     = -1_us;
    micros_t last_write_ok        = -1_us;

    int32_t  grid_w_raw         = 0;
    int32_t  battery_w_raw      = 0;
    uint8_t  soc_pct            = 255;

    bool     ema_grid_seeded    = false;
    float    ema_grid_w         = 0.0f;
    bool     prev_ema_grid_seeded = false;       // gates D so the first cycle doesn't produce a kick
    float    prev_ema_grid_w    = 0.0f;
    bool     ema_setpoint_seeded = false;
    float    ema_setpoint_w     = 0.0f;

    int32_t  last_written_w     = 0;
    uint32_t write_ok_count     = 0;
    uint32_t write_err_count    = 0;
    uint32_t consecutive_failures = 0;
    bool     safety_zero_armed  = false;

    // --- Modbus staging buffers (per-cycle, owned by the module) ---
    // sized in the .cpp via the register-count constants
    uint16_t buf_grid    [2];
    uint16_t buf_battery [8];
    uint16_t buf_soc     [2];
    uint16_t buf_setpoint[4];

    // --- 5-min live trace, served via GET /sbse_controller/history ---
    SbseTraceHistory trace_history;
};

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
