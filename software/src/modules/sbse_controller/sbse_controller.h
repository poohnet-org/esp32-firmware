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
#include "sbse_goodwe_map.h"
#include "sbse_modbus_proxy.h"
#include "sbse_modbus_server.h"
#include "sbse_solis_map.h"
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

    // How much authority an external Modbus client has over the controller's
    // active_config. Always-honoured: force_charge / force_discharge commands
    // (SMA: OpMod 2289 / 2290); the force power is taken from the payload
    // (SMA: BatChaMaxW / BatDchgMaxW) and clamped by the operator's caps at
    // write time. See CONFIG.md for the full table.
    enum class ModbusAuthority : uint8_t {
        ForceOnly = 0,   // ignore BatChaMax, BatDchgMax, GridWSpt in active_config
        Caps      = 1,   // apply max_charge_w + max_discharge_w; grid targets preserved
        Full      = 2,   // also apply GridWSpt to both grid targets (hard-mode chase)
    };

    // Protocol-neutral command from the external Modbus client. Each
    // protocol adapter (SMA OpMod + setpoint block, GoodWe EMS mode + power)
    // translates its wire format into this; the control loop only ever looks
    // at the neutral form. SMA only produces Auto / ForceCharge /
    // ForceDischarge.
    enum class ModbusCommand : uint8_t {
        Auto           = 0,   // P controller in charge (under whatever caps are set)
        ForceCharge    = 1,   // charge at modbus_cmd_power_w, P + D bypassed
        ForceDischarge = 2,   // discharge at modbus_cmd_power_w, P + D bypassed
        Idle           = 3,   // battery held at 0 W, P + D bypassed
        NoDischarge    = 4,   // P controller, but never discharge (charge from surplus only)
        NoCharge       = 5,   // P controller, but never charge
        GridTarget     = 6,   // P controller chasing modbus_grid_target_w (hard mode)
    };

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

    // SBSE Modbus staging-buffer sizes. Public so sbse_control_loop.cpp can
    // static_assert these against its register-count constants.
    static constexpr size_t BUF_GRID_LEN     = 2;
    static constexpr size_t BUF_BATTERY_LEN  = 8;
    static constexpr size_t BUF_SOC_LEN      = 2;
    static constexpr size_t BUF_SETPOINT_LEN = 4;
    static constexpr size_t BUF_IMPORT_FLOOR_LEN = 2;   // inverter WSptMin (41433), S32

private:
    // GenericTCPClientPoolConnector
    void connect_callback(TFGenericTCPClientConnectResult result, TFGenericTCPClientPoolShareLevel share_level) override;
    void disconnect_callback(TFGenericTCPClientDisconnectReason reason, TFGenericTCPClientPoolShareLevel share_level) override;

    // Cycle
    void tick();
    bool begin_cycle();                                // pre-condition checks; sets cycle_in_flight on success
    void read_grid_power();
    void read_battery_power();
    void after_battery_data();                         // SoC-if-due-or-compute continuation
    void read_rated_power();                            // one-shot rated-power read (import-floor clamp)
    void read_soc();
    void compute_and_write();
    int32_t wsptmin_for(int32_t watts) const;  // charge floor of the battery window
    void send_setpoint(int32_t watts);
    void send_import_floor(int32_t floor_w, bool then_setpoint, int32_t watts);  // refresh 41433, chain if due
    void send_zero_w();        // fire-and-forget 0 W write, used by pause + pre_reboot
    void send_safety_zero();
    int32_t pick_keepalive_pulse();  // alternating-sign small pulse, respecting SoC + caps

    // Modbus TCP server. The server class is just the network/protocol
    // adapter. The on_sma_* handlers translate the SMA wire format (sticky
    // OpMod, setpoint block) into the protocol-neutral ModbusCommand +
    // active_config changes; everything below them is protocol-independent.
    TFModbusTCPExceptionCode on_sma_op_mod_write(uint32_t op_mod);
    TFModbusTCPExceptionCode on_sma_setpoint_write(uint16_t start_address,
                                                   uint16_t reg_count,
                                                   const uint16_t *regs);
    TFModbusTCPExceptionCode on_sma_read(TFModbusTCPFunctionCode fc,
                                         uint16_t start_address,
                                         uint16_t reg_count,
                                         uint16_t *out_regs);
    void apply_sma_setpoint_partial(uint16_t start_address,
                                    uint16_t reg_count,
                                    const uint16_t *regs);

    // GoodWe protocol adapter: register reads from sbse_goodwe_map, writes
    // into the settings shadow; EMS mode (47511) + power (47512) are
    // translated into the neutral ModbusCommand after a short debounce, so
    // the order in which a client writes the two registers doesn't matter.
    TFModbusTCPExceptionCode on_goodwe_read(TFModbusTCPFunctionCode fc,
                                            uint16_t start_address,
                                            uint16_t reg_count,
                                            uint16_t *out_regs);
    TFModbusTCPExceptionCode on_goodwe_write(uint16_t start_address,
                                             uint16_t reg_count,
                                             const uint16_t *regs);
    void apply_goodwe_ems();
    void fill_goodwe_live(SbseGoodwe::Live *live);

    // Solis protocol adapter: same pattern. The remote-control registers
    // (43135 force mode, 43136 / 43129 power, 43282 timeout) are translated
    // after the debounce; the RC timeout expires like on the real inverter.
    TFModbusTCPExceptionCode on_solis_read(TFModbusTCPFunctionCode fc,
                                           uint16_t start_address,
                                           uint16_t reg_count,
                                           uint16_t *out_regs);
    TFModbusTCPExceptionCode on_solis_write(uint16_t start_address,
                                            uint16_t reg_count,
                                            const uint16_t *regs);
    void apply_solis_rc();
    void fill_solis_live(SbseSolis::Live *live);
    void solis_rc_timeout_tick();

    // Shared by the register-map adapters (GoodWe, Solis).
    void note_map_traffic();                  // watchdog liveness on any request
    void log_map_write(uint16_t start_address, uint16_t reg_count, const uint16_t *regs);
    void schedule_map_apply();                // (re)arm the debounced command apply
    void build_emulated_serial(const char *prefix, char *buf);   // 16 chars + NUL

    // Protocol-neutral Modbus command handling.
    void set_modbus_command(ModbusCommand cmd, int32_t power_w);  // + mark client active
    void clear_modbus_command();         // back to Auto, client inactive (no active_config change)
    void revert_modbus_overrides();      // watchdog expiry: clear + restore active_config
    void watchdog_tick();
    int32_t modbus_force_setpoint_w() const;  // signed force setpoint, 0 unless forcing
    bool modbus_cmd_is_fixed() const;         // fixed battery setpoint (P + D bypassed)?
    void load_modbus_server_config();    // mirror config.modbus_server_* into the cached fields

    // Proxy register cache for the SMA-hybrid read set. Polled on its own
    // scheduled tick, served by on_sma_read().
    void run_proxy_poll();
    void cycle_failed(const char *where,
                      TFModbusTCPClientTransactionResult result,
                      const char *error_message);
    void finish_cycle(Mode mode);

    void publish_setpoint(int32_t watts);
    void publish_mode(Mode mode);
    Mode current_running_mode() const;
    void refresh_running_mode();               // re-publish the pill if in a running-family mode

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
    // Two-target model: see "Grid targets" in CONFIG.md.
    // lo == hi  -> hard mode: chase the single value in both directions.
    // lo <  hi  -> soft mode: battery idle in the [lo, hi] grid deadzone;
    //             outside the deadzone the controller chases the nearer bound.
    int32_t  grid_charge_target_w    = 0;   // lower bound: charge to reach this
    int32_t  grid_discharge_target_w = 0;   // upper bound: discharge to reach this
    int32_t  max_charge_w       = 5000;
    int32_t  max_discharge_w    = 5000;
    float    kp                 = 1.0f;
    float    kd                 = 0.0f;          // D-on-measurement gain; 0 disables D
    float    alpha_grid         = 0.30f;
    float    alpha_setpoint     = 0.70f;
    int32_t  deadband_w         = 50;
    uint32_t safety_zero_after_failures = 5;  // 0 disables the safety net
    // Inverter standby keep-alive. The SBSE inverter enters a low-power
    // standby after ~10-15 min of battery_w_raw == 0, then needs ~20-30 s
    // to wake up the next time a non-zero setpoint is written. To keep it
    // warm, the controller emits a small alternating pulse (+/- keepalive_pulse_w)
    // every keepalive_interval_s of continuous battery idle. 0 disables.
    uint32_t keepalive_interval_s = 480;     // 0 disables; default 8 min (under the ~10-15 min standby threshold)
    int32_t  keepalive_pulse_w    = 50;

    // Modbus TCP server -- the network/protocol adapter. Its persistent
    // config is mirrored into the controller's cached fields below so the
    // dispatch handlers can read them without going through the server.
    SbseModbusServer modbus_server;
    SbseModbusProtocol modbus_server_protocol = SbseModbusProtocol::Sma;
    bool     modbus_server_enabled    = false;
    uint16_t modbus_server_port       = 502;
    uint8_t  modbus_server_unit_id    = 3;       // 0 = accept any unit id
    uint32_t modbus_server_watchdog_ms = 60000;  // 0 disables
    ModbusAuthority modbus_server_authority = ModbusAuthority::Caps;

    // Register cache that backs the FC 3 / FC 4 reads. Synthesizes the four
    // hot values from controller state, polls the rest from the appropriate
    // upstream unit -- see sbse_modbus_proxy.cpp for the group table.
    SbseModbusProxy modbus_proxy;
    uint64_t modbus_proxy_task_id = 0;

    // SMA adapter state: sticky OpMod (40236) latched between writes and
    // interpreted on the next setpoint-block write. 2424 = Default/Normal
    // (P loop), 2289 = Battery charging, 2290 = Battery discharging.
    uint16_t sma_op_mod              = 2424;

    // Protocol-neutral command currently in force. power_w is the magnitude
    // (>= 0); the sign comes from the command. Cleared by the watchdog,
    // operator takeover, or the client switching back to Auto.
    ModbusCommand modbus_cmd         = ModbusCommand::Auto;
    int32_t  modbus_cmd_power_w      = 0;
    int32_t  modbus_grid_target_w    = 0;   // GridTarget only; positive = import

    // Register-map adapter state. Settings shadows (whatever a client writes
    // into the protocol's settings window; GoodWe EMS mode / power, Solis
    // remote control), the pending debounced command apply, the emulated
    // serial numbers, and the Solis RC timeout.
    SbseGoodwe::Settings goodwe_settings{SbseGoodwe::settings_default};
    SbseSolis::Settings  solis_settings{SbseSolis::settings_default};
    uint64_t map_apply_task_id       = 0;
    char     goodwe_serial[17]       = {};
    char     solis_serial[17]        = {};
    micros_t solis_rc_deadline_us    = -1_us;

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
    // WSptMin as last written to the device by the setpoint block. Tracked
    // separately from last_written_w because the charge floor depends on
    // max_charge_w and the rated power, so it can change while the target --
    // and therefore the deadband decision -- stays put. INT32_MIN = unknown
    // (never written, or invalidated by pause exit / disconnect because the
    // device-side window may no longer match); forces the next cycle to
    // re-write the setpoint block.
    int32_t  last_written_wsptmin = INT32_MIN;
    uint32_t write_ok_count     = 0;
    uint32_t write_err_count    = 0;
    uint32_t modbus_read_count  = 0;   // FC 3 / FC 4 requests served by the proxy
    uint32_t modbus_write_count = 0;   // FC 16 OpMod + setpoint writes served
    uint32_t consecutive_failures = 0;
    bool     safety_zero_armed  = false;

    // Keep-alive bookkeeping. battery_idle_since_us is reset whenever the
    // most recent battery_w_raw read is non-zero; the keep-alive trigger
    // fires when (now_us - battery_idle_since_us) >= keepalive_interval_s.
    // The direction toggles between successive pulses so the long-run energy
    // contribution averages to zero. A keep-alive event spans two ticks:
    // the pulse itself, then a forced 0 W return so the inverter is wound
    // back to idle regardless of pulse magnitude vs deadband_w.
    micros_t battery_idle_since_us  = -1_us;
    bool     keepalive_next_charge  = false;
    bool     keepalive_pending_zero = false;  // next cycle: force a 0 W return write

    // Grid-import floor (inverter WSptMin, 41433). It defaults to 0 on firmware
    // >= 3.16 (blocks grid-charging) and the System Manager resets it to 0 on a
    // ~20 s beat, so the loop re-writes -min(max_charge_w, inverter_rated_w) on
    // EVERY tick while charging (no bookkeeping needed; see the register-map
    // comment in sbse_control_loop.cpp). inverter_rated_w is read once (0 = not
    // yet read) to bound the floor so the write is never rejected out-of-range.
    int32_t  inverter_rated_w    = 0;

    // --- Modbus staging buffers (per-cycle, owned by the module) ---
    // Sized via the BUF_*_LEN constants above; the static_asserts in
    // sbse_control_loop.cpp tie them back to the register-block lengths.
    // buf_zero is reserved for the fire-and-forget 0 W writes (pause /
    // pre_reboot / safety) so they can't race the in-flight cycle's
    // buf_setpoint contents if the underlying transact() doesn't copy the
    // payload synchronously.
    uint16_t buf_grid    [BUF_GRID_LEN];
    uint16_t buf_battery [BUF_BATTERY_LEN];
    uint16_t buf_soc     [BUF_SOC_LEN];
    uint16_t buf_setpoint[BUF_SETPOINT_LEN];
    uint16_t buf_zero    [BUF_SETPOINT_LEN];
    uint16_t buf_import_floor[BUF_IMPORT_FLOOR_LEN];   // inverter WSptMin (41433) refresh

    // --- 5-min live trace, served via GET /sbse_controller/history ---
    SbseTraceHistory trace_history;
};

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
