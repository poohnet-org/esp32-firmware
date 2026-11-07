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

#define EVENT_LOG_PREFIX "sbse_ctrl"

#include "sbse_controller.h"

#include <algorithm>
#include <time.h>

#include "event_log_prefix.h"
#include "generated/module_dependencies.h"

#include "gcc_warnings.h"

// ---------------------------------------------------------------------------
// How long the `pause` command keeps the loop paused. Note: the SBSE does
// NOT auto-revert to internal control when external setpoints stop arriving
// -- it is configured for one mode or the other. So this is a UX timeout
// for the operator, not an inverter-watchdog window.
// ---------------------------------------------------------------------------

static constexpr micros_t PAUSE_DURATION             = 30_s;

// ---------------------------------------------------------------------------
// SMA OpMod values latched into sma_op_mod and consulted from the setpoint
// dispatch handler, which translates them into the protocol-neutral
// ModbusCommand. The wire-level protocol (register addresses + lengths,
// listener lifecycle) lives in SbseModbusServer; the SBSE register map (grid
// power, battery power, SoC, setpoint write) lives in sbse_control_loop.cpp.
//
//   2424 = Default     -> P-controller in charge (with whatever caps are set)
//   2289 = Battery charging   -> force charge at the BatChaMax value
//   2290 = Battery discharging-> force discharge at the BatDchgMax value
// ---------------------------------------------------------------------------

static constexpr uint16_t SMA_OPMOD_DEFAULT           = 2424;
static constexpr uint16_t SMA_OPMOD_FORCE_CHARGE      = 2289;
static constexpr uint16_t SMA_OPMOD_FORCE_DISCHARGE   = 2290;

static const char *modbus_command_name(SbseController::ModbusCommand cmd)
{
    switch (cmd) {
        case SbseController::ModbusCommand::Auto:           return "auto";
        case SbseController::ModbusCommand::ForceCharge:    return "force_charge";
        case SbseController::ModbusCommand::ForceDischarge: return "force_discharge";
        case SbseController::ModbusCommand::Idle:           return "idle";
        case SbseController::ModbusCommand::NoDischarge:    return "no_discharge";
        case SbseController::ModbusCommand::NoCharge:       return "no_charge";
        case SbseController::ModbusCommand::GridTarget:     return "grid_target";
        default:                                            return "auto";
    }
}

// ---------------------------------------------------------------------------

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
#endif

SbseController::SbseController() :
    GenericTCPClientPoolConnector("sbse_ctrl", "", modbus_tcp_client.get_pool())
{
    // The fixed-size buf_* members are sized to match the SBSE register
    // counts; the corresponding compile-time checks live alongside the
    // register-map constants in sbse_control_loop.cpp.
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

void SbseController::pre_setup()
{
    // Persistent boot defaults. Written to NVS on every PUT to
    // sbse_controller/config; intended for occasional policy changes,
    // not for steering the system in real time. See active_config below
    // for the flash-free runtime channel.
    config = ConfigRoot{Config::Object({
        {"enabled",          Config::Bool(false)},
        {"host",             Config::Str("", 0, 64)},
        {"port",             Config::Uint16(502)},
        {"tick_ms",          Config::Uint(300, 50, 5000)},
        {"soc_interval_ms",  Config::Uint(1000, 100, 60000)},
        {"grid_charge_target_w",    Config::Int(0, -750, 2500)},   // lower bound
        {"grid_discharge_target_w", Config::Int(0, -750, 2500)},   // upper bound
        {"max_charge_w",     Config::Uint(5000, 0, 10000)},
        {"max_discharge_w",  Config::Uint(5000, 0, 10000)},
        {"kp_milli",         Config::Uint(1000, 100, 2000)},  // Kp * 1000 in [0.1 .. 2.0]
        {"kd_milli",         Config::Uint(0,    0, 3000)},    // Kd * 1000, 0 disables the D term
        {"alpha_grid_milli", Config::Uint(300, 10, 1000)},    // alpha * 1000 in (0.01 .. 1.0]
        {"alpha_setpoint_milli", Config::Uint(700, 10, 1000)},
        {"deadband_w",       Config::Uint(50, 0, 1000)},
        {"safety_zero_after_failures", Config::Uint(5, 0, 100)},  // 0 disables
        // Keep-alive heartbeat. Prevents the inverter from entering its
        // 10-15 min standby (which costs a ~20-30 s wake-up lag on the next
        // non-zero setpoint). Live-tunable; mirror in active_config below.
        {"keepalive_interval_s", Config::Uint(480, 0, 1800)},  // 0 disables; default 8 min
        {"keepalive_pulse_w",    Config::Uint(50, 0, 500)},    // pulse magnitude in W
        // Inverter-emulating Modbus TCP server. These fields are init-only:
        // changing them stops/starts the server but doesn't touch active_config.
        {"modbus_server_enabled",     Config::Bool(false)},
        // Emulated inverter: 0 = SMA hybrid inverter [default], 1 = GoodWe
        // ET hybrid inverter, 2 = Solis S5/S6 hybrid inverter. Exactly one
        // protocol is served at a time; see SbseModbusProtocol in
        // sbse_modbus_server.h.
        {"modbus_server_protocol",    Config::Uint(0, 0, 2)},
        {"modbus_server_port",        Config::Uint16(502)},
        {"modbus_server_unit_id",     Config::Uint(3, 0, 247)},   // 0 = accept any
        {"modbus_server_watchdog_s",  Config::Uint(60, 0, 3600)},  // 0 disables
        // 0 = ForceOnly -- ignore BatChaMax / BatDchgMax / GridWSpt in
        //                  active_config; only OpMod-driven force-mode lives.
        // 1 = Caps      -- apply max_charge_w + max_discharge_w; grid targets
        //                  preserved. [default]
        // 2 = Full      -- also mirror GridWSpt into both grid targets.
        // See ModbusAuthority in sbse_controller.h and CONFIG.md.
        {"modbus_server_authority",   Config::Uint(1, 0, 2)},
    }), [](Config &cfg, ConfigSource /*source*/) -> String {
        // The two grid targets define an [lo, hi] deadzone. lo > hi would
        // mean "discharge to a higher grid value than I'm willing to charge
        // to" which is degenerate. lo == hi is hard mode; lo < hi is soft.
        // max_charge_w / max_discharge_w are actuator saturation limits and
        // remain orthogonal to the targets.
        if (cfg.get("grid_discharge_target_w")->asInt() < cfg.get("grid_charge_target_w")->asInt()) {
            return "grid_discharge_target_w must be >= grid_charge_target_w";
        }
        if (cfg.get("soc_interval_ms")->asUint() < cfg.get("tick_ms")->asUint()) {
            return "soc_interval_ms must be >= tick_ms";
        }
        if (cfg.get("enabled")->asBool() && cfg.get("host")->asString().isEmpty()) {
            return "host must be set when enabled";
        }
        return "";
    }};

    // Live-tunable runtime overlay. Updated via PUT sbse_controller/active_config.
    // NOT persisted -- changes survive only until reboot, at which point the
    // values are re-seeded from the persistent config above. Use this channel
    // for automation / frequent steering to avoid flash wear.
    active_config = ConfigRoot{Config::Object({
        {"grid_charge_target_w",    Config::Int(0, -750, 2500)},
        {"grid_discharge_target_w", Config::Int(0, -750, 2500)},
        {"max_charge_w",     Config::Uint(5000, 0, 10000)},
        {"max_discharge_w",  Config::Uint(5000, 0, 10000)},
        {"kp_milli",         Config::Uint(1000, 100, 2000)},
        {"kd_milli",         Config::Uint(0,    0, 3000)},
        {"alpha_grid_milli", Config::Uint(300, 10, 1000)},
        {"alpha_setpoint_milli", Config::Uint(700, 10, 1000)},
        {"deadband_w",       Config::Uint(50, 0, 1000)},
        {"safety_zero_after_failures", Config::Uint(5, 0, 100)},
        {"keepalive_interval_s", Config::Uint(480, 0, 1800)},
        {"keepalive_pulse_w",    Config::Uint(50, 0, 500)},
    }), [](Config &cfg, ConfigSource /*source*/) -> String {
        // The two grid targets define an [lo, hi] deadzone. lo > hi would
        // mean "discharge to a higher grid value than I'm willing to charge
        // to" which is degenerate.
        if (cfg.get("grid_discharge_target_w")->asInt() < cfg.get("grid_charge_target_w")->asInt()) {
            return "grid_discharge_target_w must be >= grid_charge_target_w";
        }
        return "";
    }};

    state = Config::Object({
        {"mode",              Config::Str("disabled", 0, 16)},
        {"last_setpoint_w",   Config::Int32(0)},
        {"last_write_age_ms", Config::Uint32(0)},
        {"grid_w_raw",        Config::Int32(0)},
        {"grid_w_ema",        Config::Int32(0)},
        {"battery_w",         Config::Int32(0)},
        {"battery_soc",       Config::Uint8(255)},
        {"write_ok_count",    Config::Uint32(0)},
        {"write_err_count",   Config::Uint32(0)},
        {"read_fail_streak",  Config::Uint32(0)},
        {"modbus_active",     Config::Bool(false)},
        // Protocol-neutral command from the Modbus client: "auto" |
        // "force_charge" | "force_discharge" | "idle" | "no_discharge" |
        // "no_charge" | "grid_target" (the last four: GoodWe only).
        {"modbus_cmd",        Config::Str("auto", 0, 16)},
        // Grid target commanded by the client (modbus_cmd "grid_target"
        // only, else 0). Positive = import.
        {"modbus_grid_target_w", Config::Int32(0)},
        // Last SMA OpMod received (SMA protocol only; stays 2424 in GoodWe mode).
        {"modbus_op_mod",     Config::Uint16(SMA_OPMOD_DEFAULT)},
        // Signed force setpoint (negative = charge, 0 when modbus_cmd is "auto").
        {"modbus_force_w",    Config::Int32(0)},
        // Bumped on every Modbus read (FC 3 / FC 4) served by the proxy
        // cache. The dashboard watches this for state changes to drive its
        // read-activity LED.
        {"modbus_read_count",  Config::Uint32(0)},
        // Bumped on every accepted Modbus write (FC 16 OpMod or setpoint
        // sub-block). Drives the dashboard's write-activity LED.
        {"modbus_write_count", Config::Uint32(0)},
        {"last_error",         Config::Str("", 0, 64)},
    });
}

void SbseController::setup()
{
    api.restorePersistentConfig("sbse_controller/config", &config);

    load_init_only_from_config();
    copy_live_tunable_to_active();
    apply_runtime_from_active();

    initialized = true;
}

void SbseController::load_init_only_from_config()
{
    enabled         = config.get("enabled")->asBool();
    tick_ms         = millis_t{static_cast<int64_t>(config.get("tick_ms")->asUint())};
    soc_interval_ms = millis_t{static_cast<int64_t>(config.get("soc_interval_ms")->asUint())};

    host = config.get("host")->asString();
    port = static_cast<uint16_t>(config.get("port")->asUint());

    load_modbus_server_config();
}

void SbseController::load_modbus_server_config()
{
    modbus_server_enabled       = config.get("modbus_server_enabled")->asBool();
    modbus_server_protocol      = static_cast<SbseModbusProtocol>(config.get("modbus_server_protocol")->asUint());
    modbus_server_port          = config.get("modbus_server_port")->asUint16();
    modbus_server_unit_id       = static_cast<uint8_t>(config.get("modbus_server_unit_id")->asUint());
    modbus_server_watchdog_ms   = config.get("modbus_server_watchdog_s")->asUint() * 1000u;
    modbus_server_authority     = static_cast<ModbusAuthority>(config.get("modbus_server_authority")->asUint());
}

void SbseController::copy_live_tunable_to_active()
{
    // Mirror the live-tunable subset of the persistent config into the
    // runtime overlay. Called at boot and after a persistent PUT so that
    // both endpoints stay coherent.
    active_config.get("grid_charge_target_w")   ->updateInt (config.get("grid_charge_target_w")   ->asInt());
    active_config.get("grid_discharge_target_w")->updateInt (config.get("grid_discharge_target_w")->asInt());
    active_config.get("max_charge_w")           ->updateUint(config.get("max_charge_w")         ->asUint());
    active_config.get("max_discharge_w")        ->updateUint(config.get("max_discharge_w")      ->asUint());
    active_config.get("kp_milli")               ->updateUint(config.get("kp_milli")             ->asUint());
    active_config.get("kd_milli")               ->updateUint(config.get("kd_milli")             ->asUint());
    active_config.get("alpha_grid_milli")       ->updateUint(config.get("alpha_grid_milli")     ->asUint());
    active_config.get("alpha_setpoint_milli")   ->updateUint(config.get("alpha_setpoint_milli") ->asUint());
    active_config.get("deadband_w")             ->updateUint(config.get("deadband_w")           ->asUint());
    active_config.get("safety_zero_after_failures")->updateUint(config.get("safety_zero_after_failures")->asUint());
    active_config.get("keepalive_interval_s")   ->updateUint(config.get("keepalive_interval_s") ->asUint());
    active_config.get("keepalive_pulse_w")      ->updateUint(config.get("keepalive_pulse_w")    ->asUint());
}

void SbseController::apply_runtime_from_active()
{
    // Refresh the cached fields the hot path reads.
    grid_charge_target_w    = active_config.get("grid_charge_target_w")->asInt();
    grid_discharge_target_w = active_config.get("grid_discharge_target_w")->asInt();
    max_charge_w    = static_cast<int32_t>(active_config.get("max_charge_w")->asUint());
    max_discharge_w = static_cast<int32_t>(active_config.get("max_discharge_w")->asUint());
    kp              = static_cast<float>(active_config.get("kp_milli")->asUint())             / 1000.0f;
    kd              = static_cast<float>(active_config.get("kd_milli")->asUint())             / 1000.0f;
    alpha_grid      = static_cast<float>(active_config.get("alpha_grid_milli")->asUint())     / 1000.0f;
    alpha_setpoint  = static_cast<float>(active_config.get("alpha_setpoint_milli")->asUint()) / 1000.0f;
    deadband_w      = static_cast<int32_t>(active_config.get("deadband_w")->asUint());
    safety_zero_after_failures = active_config.get("safety_zero_after_failures")->asUint();
    keepalive_interval_s = active_config.get("keepalive_interval_s")->asUint();
    keepalive_pulse_w    = static_cast<int32_t>(active_config.get("keepalive_pulse_w")->asUint());
    refresh_running_mode();
}

void SbseController::refresh_running_mode()
{
    // Mode pill is derived from the saturation caps + modbus command, so a
    // cap change from dashboard / HTTP / MQTT / Modbus or a new Modbus
    // command may flip the running family (running <-> block_charge <->
    // block_discharge <-> blocked or the force_* variants). Refresh the
    // published mode immediately when we're already in a running-family
    // state -- don't touch it when paused / stale / disabled / safety /
    // faulted, where the cycle owns the badge.
    const String &mode = state.get("mode")->asString();
    if (mode == "running"
        || mode == "blocked"
        || mode == "block_charge"
        || mode == "block_discharge"
        || mode == "force_charge"
        || mode == "force_discharge") {
        publish_mode(current_running_mode());
    }
}

void SbseController::register_urls()
{
    // Persistent config (writes to NVS on every PUT). We open-code the
    // state + "_update" command pair instead of api.addPersistentConfig
    // so we can hook a hot-reload callback after the flash write completes.
    api.addState("sbse_controller/config", &config);
    api.addCommand("sbse_controller/config_update", &config, {},
                   [this](Language /*language*/, String &/*errmsg*/) {
        API::writeConfig("sbse_controller/config", &config);

        // Hot-reload the live-tunable subset so PUTs to /config take effect
        // immediately. Init-only fields (host, port, tick_ms, ...) still
        // require a reboot.
        copy_live_tunable_to_active();
        apply_runtime_from_active();

        // Refresh Modbus server config. Bounce the listener only if the
        // bind-time fields (enabled / port / protocol) actually changed; the
        // rest (unit_id, watchdog, authority) are consulted live and don't
        // require a restart.
        const SbseModbusProtocol previous_protocol = modbus_server_protocol;
        load_modbus_server_config();
        const bool needs_restart = modbus_server.needs_restart_for(modbus_server_enabled,
                                                                   modbus_server_port,
                                                                   modbus_server_protocol);
        modbus_server.configure(modbus_server_enabled, modbus_server_port,
                                modbus_server_unit_id, modbus_server_protocol);
        if (needs_restart) {
            modbus_server.restart();
        }
        // A command latched by the old protocol's client must not outlive
        // the switch -- that client can no longer reach us to release it.
        if (modbus_server_protocol != previous_protocol) {
            revert_modbus_overrides();
        }
    }, false);

    // Live-tunable runtime overlay (no flash write). HTTP/MQTT writes go
    // through this command callback; internal Modbus dispatch mutates
    // active_config directly to avoid clearing its own force-mode state.
    api.addState("sbse_controller/active_config", &active_config);
    api.addCommand("sbse_controller/active_config_update", &active_config, {},
                   [this](Language /*language*/, String &/*errmsg*/) {
        // Operator (dashboard / HTTP / MQTT) takeover -- release any Modbus
        // force-mode setpoint so the P controller is back in charge of
        // hitting the new target. Cleared before apply_runtime_from_active()
        // so the refreshed mode pill no longer shows force_*.
        if (modbus_cmd != ModbusCommand::Auto || modbus_active) {
            logger.printfln("operator override -- releasing Modbus force-mode setpoint");
        }
        clear_modbus_command();
        apply_runtime_from_active();
    }, false);

    api.addState("sbse_controller/state", &state);

    // Live-trace ring buffer: GET returns the last ~5 minutes of
    // (grid, battery, setpoint, target) sampled at 1 Hz, so a freshly-loaded
    // dashboard can seed its chart instead of starting from a blank canvas.
    trace_history.register_url("/sbse_controller/history");

    api.addCommand("sbse_controller/pause", Config::Null(), {},
                   [this](Language /*language*/, String &/*errmsg*/) {
        paused       = true;
        paused_until = now_us() + PAUSE_DURATION;
        // Operator takeover: drop any Modbus force-mode state too.
        clear_modbus_command();
        send_zero_w();
        publish_mode(Mode::Paused);
        logger.printfln("pause: holding setpoint loop at 0 W for %lld s",
                        static_cast<long long>(PAUSE_DURATION.to<seconds_t>().t));
    }, true);

    api.addCommand("sbse_controller/resume", Config::Null(), {},
                   [this](Language /*language*/, String &/*errmsg*/) {
        if (!paused) {
            return;
        }
        paused       = false;
        paused_until = -1_us;
        // Reseed the EMAs and the D reference point: the loop has been idle
        // long enough that prev_ema_grid_w is stale, which would otherwise
        // produce a derivative kick on the first post-resume cycle.
        ema_grid_seeded       = false;
        prev_ema_grid_seeded  = false;
        ema_setpoint_seeded   = false;
        // Don't carry a stale battery-idle measurement across the pause,
        // and drop any in-flight two-tick keep-alive event.
        battery_idle_since_us  = -1_us;
        keepalive_pending_zero = false;
        // Pause parked the window at 0/0 behind the loop's back; force the
        // first post-resume cycle to re-assert the setpoint block.
        last_written_wsptmin   = INT32_MIN;
        // Operator takeover. (Idempotent if already cleared by pause.)
        clear_modbus_command();

        // Publish a plausible mode immediately so the dashboard doesn't sit on
        // "paused" until the next tick fires. The tick will correct it if
        // anything is off (e.g. read failure -> stale).
        if (!enabled) {
            publish_mode(Mode::Disabled);
        } else if (shared_client == nullptr) {
            publish_mode(Mode::NotConnected);
        } else {
            publish_mode(Mode::Stale);
        }
        logger.printfln("resume: ending pause early");
    }, true);
}

void SbseController::register_events()
{
    network.on_network_connected([this](const Config *connected) {
        if (connected->asBool() && enabled) {
            apply_enabled();
        } else {
            stop_connection();
        }
        return EventResult::OK;
    });

    // Kick the tick once the config-driven host/port is in place. The tick is
    // a no-op until connect_callback flips shared_client to non-null.
    tick_task_id = task_scheduler.scheduleWithFixedDelay([this]() {
        this->tick();
    }, tick_ms, tick_ms);

    // Wire the Modbus server's SMA protocol-adapter callbacks once, then
    // start if enabled.
    modbus_server.set_handlers(
        [this](uint32_t op_mod) {
            return this->on_sma_op_mod_write(op_mod);
        },
        [this](uint16_t start_address, uint16_t reg_count, const uint16_t *regs) {
            return this->on_sma_setpoint_write(start_address, reg_count, regs);
        },
        [this](TFModbusTCPFunctionCode fc, uint16_t start_address,
               uint16_t reg_count, uint16_t *out_regs) {
            return this->on_sma_read(fc, start_address, reg_count, out_regs);
        });
    modbus_server.set_map_handlers(
        [this](TFModbusTCPFunctionCode fc, uint16_t start_address,
               uint16_t reg_count, uint16_t *out_regs) {
            return modbus_server_protocol == SbseModbusProtocol::Solis
                 ? this->on_solis_read(fc, start_address, reg_count, out_regs)
                 : this->on_goodwe_read(fc, start_address, reg_count, out_regs);
        },
        [this](uint16_t start_address, uint16_t reg_count, const uint16_t *regs) {
            return modbus_server_protocol == SbseModbusProtocol::Solis
                 ? this->on_solis_write(start_address, reg_count, regs)
                 : this->on_goodwe_write(start_address, reg_count, regs);
        });
    modbus_server.configure(modbus_server_enabled, modbus_server_port,
                            modbus_server_unit_id, modbus_server_protocol);
    if (modbus_server_enabled) {
        modbus_server.start();
    }

    // Drive the proxy register cache. Refresh cadence is slower than the
    // control loop's so it doesn't compete with grid/battery/SoC reads, but
    // fast enough that cumulative energies and per-phase numbers stay
    // current for any evcc poll cycle. With ~9 cached groups this gives a
    // full refresh every ~4.5 s.
    modbus_proxy_task_id = task_scheduler.scheduleWithFixedDelay([this]() {
        this->run_proxy_poll();
    }, 500_ms, 500_ms);
}

void SbseController::pre_reboot()
{
    if (tick_task_id != 0) {
        task_scheduler.cancel(tick_task_id);
        tick_task_id = 0;
    }
    if (modbus_proxy_task_id != 0) {
        task_scheduler.cancel(modbus_proxy_task_id);
        modbus_proxy_task_id = 0;
    }
    if (map_apply_task_id != 0) {
        task_scheduler.cancel(map_apply_task_id);
        map_apply_task_id = 0;
    }

    modbus_server.stop();

    // Best-effort 0 W write so the inverter isn't left holding a stale
    // active setpoint across our reboot. Fire-and-forget; the connection
    // may already be down.
    send_zero_w();
    stop_connection();
}

void SbseController::apply_enabled()
{
    if (enabled && !host.isEmpty()) {
        start_connection();
    } else {
        stop_connection();
    }
}

// ---------------------------------------------------------------------------
// SMA protocol adapter
//
// These are the protocol-layer callbacks SbseModbusServer invokes on each
// accepted SMA request. The server has already validated the unit-id filter
// and the address/length tuple; here we translate the SMA wire format
// (sticky OpMod, setpoint block) into the protocol-neutral ModbusCommand and
// the active_config changes the authority level allows.
// ---------------------------------------------------------------------------

TFModbusTCPExceptionCode SbseController::on_sma_op_mod_write(uint32_t op_mod)
{
    // 40236: CmpBMS.OpMod (U32BE). Latch for the next 40793 write. We accept
    // the three documented SMA values (2289/2290/2424); anything else is
    // treated as "Default" so a buggy client can't trap us in a force-mode
    // we can't get out of without a reboot.
    if (op_mod == SMA_OPMOD_FORCE_CHARGE
        || op_mod == SMA_OPMOD_FORCE_DISCHARGE
        || op_mod == SMA_OPMOD_DEFAULT) {
        sma_op_mod = static_cast<uint16_t>(op_mod);
    } else {
        sma_op_mod = SMA_OPMOD_DEFAULT;
    }
    state.get("modbus_op_mod")->updateUint(sma_op_mod);
    ++modbus_write_count;
    state.get("modbus_write_count")->updateUint(modbus_write_count);
    return TFModbusTCPExceptionCode::Success;
}

TFModbusTCPExceptionCode SbseController::on_sma_setpoint_write(
    uint16_t start_address, uint16_t reg_count, const uint16_t *regs)
{
    // Setpoint sub-block at 40793..40802 (each field is U32BE = 2 regs).
    // The server already validated that start_address is even-aligned to a
    // uint32 boundary and that (start_address, reg_count) fits entirely
    // within the 10-register window. We only act on the fields actually
    // present in this write; absent fields keep their previous active_config
    // values (matches evcc's piecemeal write pattern).
    apply_sma_setpoint_partial(start_address, reg_count, regs);
    ++modbus_write_count;
    state.get("modbus_write_count")->updateUint(modbus_write_count);
    return TFModbusTCPExceptionCode::Success;
}

void SbseController::apply_sma_setpoint_partial(uint16_t start_address,
                                                uint16_t reg_count,
                                                const uint16_t *regs)
{
    // SMA setpoint field indices within the 5x uint32 block at 40793:
    //   0 = BatChaMinW    (ignored -- controller has no minimum-charge cap)
    //   1 = BatChaMaxW
    //   2 = BatDchgMinW   (ignored -- no minimum-discharge cap)
    //   3 = BatDchgMaxW
    //   4 = GridWSpt
    constexpr uint16_t SETP_ADDR_LO = 40793;
    const uint16_t first_field = static_cast<uint16_t>((start_address - SETP_ADDR_LO) / 2);
    const uint16_t field_count = static_cast<uint16_t>(reg_count / 2);

    bool      has_cha_max = false, has_dchg_max = false, has_grid_spt = false;
    uint32_t  bat_cha_max = 0, bat_dchg_max = 0;
    int32_t   grid_spt    = 0;

    for (uint16_t i = 0; i < field_count; ++i) {
        const uint32_t value = (static_cast<uint32_t>(regs[2 * i]) << 16) | regs[2 * i + 1];
        switch (first_field + i) {
            case 0: /* BatChaMinW  */ break;
            case 1: bat_cha_max  = value;                       has_cha_max  = true; break;
            case 2: /* BatDchgMinW */ break;
            case 3: bat_dchg_max = value;                       has_dchg_max = true; break;
            case 4: grid_spt     = static_cast<int32_t>(value); has_grid_spt = true; break;
            default: break;
        }
    }

    // Clamp to the SBSE controller's own range. The Config types would reject
    // out-of-range values; we mirror their bounds here so the Modbus client
    // sees Success instead of a generic error.
    auto clamp_w = [](uint32_t v) -> uint32_t { return v > 10000u ? 10000u : v; };
    const uint32_t cha_max_clamped  = clamp_w(bat_cha_max);
    const uint32_t dchg_max_clamped = clamp_w(bat_dchg_max);
    const int32_t  spt_clamped      = std::clamp(grid_spt, int32_t{-750}, int32_t{2500});

    // Interpret OpMod. Force modes bypass the P controller (see compute_and_write
    // step 3a). OpMod 2424 lets the P controller drive against the new caps and
    // target. If a partial write didn't include the relevant max field, fall
    // back to the controller's current cached cap so a single OpMod write
    // immediately following GridWSpt still picks a sensible force value.
    ModbusCommand cmd   = ModbusCommand::Auto;
    int32_t       power = 0;
    if (sma_op_mod == SMA_OPMOD_FORCE_CHARGE) {
        cmd   = ModbusCommand::ForceCharge;
        power = has_cha_max ? static_cast<int32_t>(cha_max_clamped) : max_charge_w;
    } else if (sma_op_mod == SMA_OPMOD_FORCE_DISCHARGE) {
        cmd   = ModbusCommand::ForceDischarge;
        power = has_dchg_max ? static_cast<int32_t>(dchg_max_clamped) : max_discharge_w;
    }
    // A force request at 0 W has always meant "P controller" on the SMA
    // path (the old signed force value used 0 as "no force"); keep that.
    if (power == 0) {
        cmd = ModbusCommand::Auto;
    }
    // Set before apply_runtime_from_active() below so its mode-pill refresh
    // already sees the new command.
    set_modbus_command(cmd, power);

    // Mutate active_config in place per the configured authority level.
    // Direct mutation auto-publishes via the API event bus (so MQTT /
    // dashboard see the new values immediately) but skips the
    // active_config_update command handler -- which would otherwise clear
    // the force-mode state we just set.
    //
    // ModbusAuthority::Caps and above   -> apply both saturation caps.
    // ModbusAuthority::Full             -> also mirror GridWSpt into both
    //                                      grid targets (hard-mode chase).
    // ModbusAuthority::ForceOnly        -> active_config untouched; the
    //                                      operator's caps / targets stand.
    //                                      The force_w computed above is the
    //                                      only effect of this write.
    bool dirty_active_config = false;
    if (modbus_server_authority >= ModbusAuthority::Caps) {
        if (has_cha_max) {
            active_config.get("max_charge_w")->updateUint(cha_max_clamped);
            dirty_active_config = true;
        }
        if (has_dchg_max) {
            active_config.get("max_discharge_w")->updateUint(dchg_max_clamped);
            dirty_active_config = true;
        }
    }
    if (modbus_server_authority >= ModbusAuthority::Full && has_grid_spt) {
        active_config.get("grid_charge_target_w")   ->updateInt(spt_clamped);
        active_config.get("grid_discharge_target_w")->updateInt(spt_clamped);
        dirty_active_config = true;
    }
    if (dirty_active_config) {
        apply_runtime_from_active();
    }
}

TFModbusTCPExceptionCode SbseController::on_sma_read(
    TFModbusTCPFunctionCode fc, uint16_t start_address,
    uint16_t reg_count, uint16_t *out_regs)
{
    // Server callback: pack the proxy cache + synthesized values into the
    // response buffer. Synthesis inputs are the latest live samples from
    // the control loop -- so SoC, grid, and battery W never lag behind the
    // controller's most recent read.
    const SbseModbusProxy::SynthesisInputs inputs{
        /* grid_w_raw    */ grid_w_raw,
        /* battery_w_raw */ battery_w_raw,
        /* soc_pct       */ soc_pct,
    };
    const TFModbusTCPExceptionCode rc =
        modbus_proxy.pack_response(fc, start_address, reg_count, out_regs, inputs);

    // Bump the read counter so the dashboard's read-activity LED can flash.
    // Tick every served request -- the value's a free-running counter, the
    // frontend only watches for changes.
    ++modbus_read_count;
    state.get("modbus_read_count")->updateUint(modbus_read_count);
    return rc;
}

// ---------------------------------------------------------------------------
// GoodWe protocol adapter
//
// Reads are answered from the GoodWe register map (sbse_goodwe_map.cpp) fed
// with the controller's live values and the SMA proxy cache. Writes land in
// the settings shadow; EMS mode / power are translated into the neutral
// ModbusCommand by apply_goodwe_ems(). Every write is logged with its
// values -- writes are rare, and they are what we need to see from a new
// client.
// ---------------------------------------------------------------------------

// Debounce between a command-register write and applying it: evcc writes
// GoodWe mode then power, the goodwe library mode then (optionally) power,
// Solis clients 43135 then the power then the timeout; other clients may use
// another order. A real inverter applies each register on its own; we wait
// so an intermediate (new mode, old power) pair never reaches the loop.
static constexpr millis_t MAP_APPLY_DEBOUNCE = 200_ms;

void SbseController::note_map_traffic()
{
    // A real GoodWe / Solis keeps its mode until told otherwise, and their
    // clients only write on a change. So in these modes any request --
    // reads included -- counts as "client still there" for the watchdog;
    // it fires once the client stops polling altogether.
    if (modbus_active) {
        last_modbus_write_us = now_us();
    }
}

void SbseController::log_map_write(uint16_t start_address, uint16_t reg_count,
                                   const uint16_t *regs)
{
    char values[64];
    size_t used = 0;
    values[0] = '\0';
    for (uint16_t i = 0; i < reg_count && i < 8 && used < sizeof(values); ++i) {
        used += static_cast<size_t>(snprintf(values + used, sizeof(values) - used,
                                             i == 0 ? "%u" : " %u", regs[i]));
    }
    logger.printfln("%s write %u+%u: %s%s", sbse_modbus_protocol_name(modbus_server_protocol),
                    start_address, reg_count, values, reg_count > 8 ? " ..." : "");
}

void SbseController::schedule_map_apply()
{
    if (map_apply_task_id != 0) {
        task_scheduler.cancel(map_apply_task_id);
    }
    map_apply_task_id = task_scheduler.scheduleOnce([this]() {
        map_apply_task_id = 0;
        if (modbus_server_protocol == SbseModbusProtocol::Solis) {
            this->apply_solis_rc();
        } else if (modbus_server_protocol == SbseModbusProtocol::Goodwe) {
            this->apply_goodwe_ems();
        }
    }, MAP_APPLY_DEBOUNCE);
}

void SbseController::build_emulated_serial(const char *prefix, char *buf)
{
    // prefix + the ESP32 Brick UID, '0'-padded to 16 characters.
    const String &uid = esp32_common.get_uid_str();
    size_t n = 0;
    for (size_t i = 0; prefix[i] != '\0' && n < 16; ++i) buf[n++] = prefix[i];
    for (size_t i = 0; i < uid.length() && n < 16; ++i) buf[n++] = uid[i];
    while (n < 16) buf[n++] = '0';
    buf[16] = '\0';
}

void SbseController::fill_goodwe_live(SbseGoodwe::Live *live)
{
    if (goodwe_serial[0] == '\0') {
        // "ETU": the goodwe library picks the ET family by serial substring.
        build_emulated_serial("SBSEETU", goodwe_serial);
    }

    const uint32_t rated_w = inverter_rated_w > 0 ? static_cast<uint32_t>(inverter_rated_w) : 5000u;
    // e.g. "GW5K-ET", "GW3.6K-ET". The register map truncates to 10 chars.
    static char model[32];
    if (rated_w % 1000u == 0) {
        snprintf(model, sizeof(model), "GW%luK-ET", rated_w / 1000u);
    } else {
        snprintf(model, sizeof(model), "GW%lu.%luK-ET", rated_w / 1000u, (rated_w % 1000u) / 100u);
    }

    uint64_t v;
    live->grid_w    = grid_w_raw;
    live->battery_w = battery_w_raw;
    live->soc_pct   = soc_pct;
    live->pv_w      = modbus_proxy.get_cached(30773, 2, &v) ? static_cast<int32_t>(v) : -1;   // DcMs.Watt[0]
    live->rated_w   = rated_w;
    live->pv_total_wh      = modbus_proxy.get_cached(30513, 4, &v) ? v : UINT64_MAX;  // Metering.TotWhOut
    live->grid_import_wh   = modbus_proxy.get_cached(30581, 2, &v) ? v : UINT64_MAX;  // GridMs.TotWhIn
    live->grid_export_wh   = modbus_proxy.get_cached(30583, 2, &v) ? v : UINT64_MAX;  // GridMs.TotWhOut
    live->bat_charge_wh    = UINT64_MAX;                                               // not polled
    live->bat_discharge_wh = modbus_proxy.get_cached(31401, 4, &v) ? v : UINT64_MAX;  // CmpBMS.GetBatDschWh
    live->serial = goodwe_serial;
    live->model  = model;

    const time_t now = time(nullptr);
    struct tm tm_now;
    live->time_valid = now > 1700000000 && localtime_r(&now, &tm_now) != nullptr;
    if (live->time_valid) {
        live->year   = static_cast<uint8_t>(tm_now.tm_year - 100);
        live->month  = static_cast<uint8_t>(tm_now.tm_mon + 1);
        live->day    = static_cast<uint8_t>(tm_now.tm_mday);
        live->hour   = static_cast<uint8_t>(tm_now.tm_hour);
        live->minute = static_cast<uint8_t>(tm_now.tm_min);
        live->second = static_cast<uint8_t>(tm_now.tm_sec);
    }
}

TFModbusTCPExceptionCode SbseController::on_goodwe_read(
    TFModbusTCPFunctionCode /*fc*/, uint16_t start_address,
    uint16_t reg_count, uint16_t *out_regs)
{
    SbseGoodwe::Live live{};
    fill_goodwe_live(&live);
    const TFModbusTCPExceptionCode rc =
        SbseGoodwe::read(start_address, reg_count, out_regs, live, goodwe_settings);

    note_map_traffic();
    ++modbus_read_count;
    state.get("modbus_read_count")->updateUint(modbus_read_count);
    return rc;
}

TFModbusTCPExceptionCode SbseController::on_goodwe_write(
    uint16_t start_address, uint16_t reg_count, const uint16_t *regs)
{
    log_map_write(start_address, reg_count, regs);

    if (!SbseGoodwe::is_writable(start_address, reg_count)) {
        return TFModbusTCPExceptionCode::IllegalDataAddress;
    }

    bool touches_ems = false;
    for (uint16_t i = 0; i < reg_count; ++i) {
        const uint16_t addr = static_cast<uint16_t>(start_address + i);
        if (addr == SbseGoodwe::REG_EMS_MODE && !SbseGoodwe::ems_mode_supported(regs[i])) {
            logger.printfln("goodwe: EMS mode %u (%s) not supported, write rejected",
                            regs[i], SbseGoodwe::ems_mode_name(regs[i]));
            return TFModbusTCPExceptionCode::IllegalDataValue;
        }
        touches_ems = touches_ems
                   || addr == SbseGoodwe::REG_EMS_MODE
                   || addr == SbseGoodwe::REG_EMS_POWER;
    }
    for (uint16_t i = 0; i < reg_count; ++i) {
        if (!goodwe_settings.set(static_cast<uint16_t>(start_address + i), regs[i])) {
            logger.printfln("goodwe: settings shadow full, write to %u dropped",
                            static_cast<unsigned>(start_address + i));
            return TFModbusTCPExceptionCode::ServerDeviceFailure;
        }
    }

    if (touches_ems) {
        schedule_map_apply();
    }

    note_map_traffic();
    ++modbus_write_count;
    state.get("modbus_write_count")->updateUint(modbus_write_count);
    return TFModbusTCPExceptionCode::Success;
}

void SbseController::apply_goodwe_ems()
{
    using EmsMode = SbseGoodwe::EmsMode;

    const uint16_t mode = goodwe_settings.get(SbseGoodwe::REG_EMS_MODE);
    const int32_t  xset = goodwe_settings.get(SbseGoodwe::REG_EMS_POWER);

    // EMS mode -> neutral command. Xset semantics per the goodwe library's
    // EMSMode docs; approximations are noted in CONFIG.md. Battery power is
    // still clamped by max_charge_w / max_discharge_w in the control loop.
    ModbusCommand cmd   = ModbusCommand::Auto;
    int32_t       power = 0;
    switch (static_cast<EmsMode>(mode)) {
        case EmsMode::ChargePv:          // Xmax + PV; Xmax 0 = PV only
        case EmsMode::ImportAc:          // Xset + PV, grid first
            cmd   = xset > 0 ? ModbusCommand::ForceCharge : ModbusCommand::NoDischarge;
            power = xset;
            break;
        case EmsMode::ChargeBattery:
            cmd   = xset > 0 ? ModbusCommand::ForceCharge : ModbusCommand::Idle;
            power = xset;
            break;
        case EmsMode::DischargePv:
        case EmsMode::ExportAc:
        case EmsMode::DischargeBattery:
            cmd   = xset > 0 ? ModbusCommand::ForceDischarge : ModbusCommand::Idle;
            power = xset;
            break;
        case EmsMode::Conserve:
            cmd = ModbusCommand::NoDischarge;
            break;
        case EmsMode::BatteryStandby:
            cmd = ModbusCommand::Idle;
            break;
        case EmsMode::BuyPower:
        case EmsMode::SellPower:
            if (modbus_server_authority >= ModbusAuthority::Full) {
                cmd   = ModbusCommand::GridTarget;
                // Same bounds as the configurable grid targets.
                power = std::clamp(mode == static_cast<uint16_t>(EmsMode::BuyPower) ? xset : -xset,
                                   int32_t{-750}, int32_t{2500});
            } else {
                logger.printfln("goodwe: EMS %s needs modbus_server_authority = full, running auto",
                                SbseGoodwe::ems_mode_name(mode));
            }
            break;
        case EmsMode::Auto:
        case EmsMode::OffGrid:           // rejected at write time
        default:
            break;
    }

    logger.printfln("goodwe: EMS %s (%u), Xset %ld W -> %s %ld W",
                    SbseGoodwe::ems_mode_name(mode), mode, static_cast<long>(xset),
                    modbus_command_name(cmd), static_cast<long>(power));
    set_modbus_command(cmd, power);
    // Refresh the mode pill right away instead of on the next loop cycle.
    refresh_running_mode();
}

// ---------------------------------------------------------------------------
// Solis protocol adapter
//
// Same pattern as GoodWe: reads from sbse_solis_map, writes into the
// settings shadow, every write logged. Remote control (43135 + 43136 / 43129
// + 43282) is translated into the neutral command; other writes (storage
// mode 43110, time-of-use slots, remote dispatch 441xx) are stored and
// logged but have no effect yet -- the logs show which ones a client uses.
// ---------------------------------------------------------------------------

void SbseController::fill_solis_live(SbseSolis::Live *live)
{
    if (solis_serial[0] == '\0') {
        build_emulated_serial("SBSE", solis_serial);
    }

    uint64_t v;
    live->grid_w    = grid_w_raw;
    live->battery_w = battery_w_raw;
    live->soc_pct   = soc_pct;
    live->pv_w      = modbus_proxy.get_cached(30773, 2, &v) ? static_cast<int32_t>(v) : -1;   // DcMs.Watt[0]
    live->pv_total_wh      = modbus_proxy.get_cached(30513, 4, &v) ? v : UINT64_MAX;  // Metering.TotWhOut
    live->bat_discharge_wh = modbus_proxy.get_cached(31401, 4, &v) ? v : UINT64_MAX;  // CmpBMS.GetBatDschWh
    // SMA NaN: 0xFFFFFFFF (all ones, filtered by get_cached) / 0x80000000.
    live->bat_voltage_cv = modbus_proxy.get_cached(30851, 2, &v) ? static_cast<uint32_t>(v) : 0;  // Bat.Vol
    live->bat_current_ma = modbus_proxy.get_cached(30843, 2, &v) ? static_cast<int32_t>(static_cast<uint32_t>(v)) : INT32_MIN;  // Bat.Amp
    live->bat_temp_dc    = modbus_proxy.get_cached(30849, 2, &v) ? static_cast<int32_t>(static_cast<uint32_t>(v)) : INT32_MIN;  // Bat.TmpVal
    live->max_charge_w    = max_charge_w;
    live->max_discharge_w = max_discharge_w;
    live->serial = solis_serial;

    const time_t now = time(nullptr);
    struct tm tm_now;
    live->time_valid = now > 1700000000 && localtime_r(&now, &tm_now) != nullptr;
    if (live->time_valid) {
        live->year   = static_cast<uint8_t>(tm_now.tm_year - 100);
        live->month  = static_cast<uint8_t>(tm_now.tm_mon + 1);
        live->day    = static_cast<uint8_t>(tm_now.tm_mday);
        live->hour   = static_cast<uint8_t>(tm_now.tm_hour);
        live->minute = static_cast<uint8_t>(tm_now.tm_min);
        live->second = static_cast<uint8_t>(tm_now.tm_sec);
    }
}

TFModbusTCPExceptionCode SbseController::on_solis_read(
    TFModbusTCPFunctionCode /*fc*/, uint16_t start_address,
    uint16_t reg_count, uint16_t *out_regs)
{
    SbseSolis::Live live{};
    fill_solis_live(&live);
    const TFModbusTCPExceptionCode rc =
        SbseSolis::read(start_address, reg_count, out_regs, live, solis_settings);

    note_map_traffic();
    ++modbus_read_count;
    state.get("modbus_read_count")->updateUint(modbus_read_count);
    return rc;
}

TFModbusTCPExceptionCode SbseController::on_solis_write(
    uint16_t start_address, uint16_t reg_count, const uint16_t *regs)
{
    log_map_write(start_address, reg_count, regs);

    if (!SbseSolis::is_writable(start_address, reg_count)) {
        return TFModbusTCPExceptionCode::IllegalDataAddress;
    }

    bool touches_rc = false;
    for (uint16_t i = 0; i < reg_count; ++i) {
        const uint16_t addr = static_cast<uint16_t>(start_address + i);
        if (addr == SbseSolis::REG_RC_FORCE && regs[i] > SbseSolis::RC_FORCE_DISCHARGE) {
            logger.printfln("solis: RC force mode %u not supported, write rejected", regs[i]);
            return TFModbusTCPExceptionCode::IllegalDataValue;
        }
        touches_rc = touches_rc
                  || addr == SbseSolis::REG_RC_FORCE
                  || addr == SbseSolis::REG_RC_CHARGE_W
                  || addr == SbseSolis::REG_RC_DISCHARGE_W
                  || addr == SbseSolis::REG_RC_TIMEOUT_MIN
                  || (addr >= SbseSolis::REG_RD_FIRST && addr <= SbseSolis::REG_RD_LAST);
    }
    for (uint16_t i = 0; i < reg_count; ++i) {
        if (!solis_settings.set(static_cast<uint16_t>(start_address + i), regs[i])) {
            logger.printfln("solis: settings shadow full, write to %u dropped",
                            static_cast<unsigned>(start_address + i));
            return TFModbusTCPExceptionCode::ServerDeviceFailure;
        }
    }

    if (touches_rc) {
        schedule_map_apply();
    }

    note_map_traffic();
    ++modbus_write_count;
    state.get("modbus_write_count")->updateUint(modbus_write_count);
    return TFModbusTCPExceptionCode::Success;
}

void SbseController::apply_solis_rc()
{
    // Remote dispatch (44100 = 1) takes precedence over the older remote
    // control (43135) path.
    if (solis_settings.get(SbseSolis::REG_RD_ENABLE) == 1) {
        apply_solis_dispatch();
        return;
    }

    const uint16_t force       = solis_settings.get(SbseSolis::REG_RC_FORCE);
    const int32_t  charge_w    = static_cast<int32_t>(solis_settings.get(SbseSolis::REG_RC_CHARGE_W)) * 10;
    const int32_t  discharge_w = static_cast<int32_t>(solis_settings.get(SbseSolis::REG_RC_DISCHARGE_W)) * 10;
    uint16_t       timeout_min = solis_settings.get(SbseSolis::REG_RC_TIMEOUT_MIN);
    if (timeout_min == 0) {
        timeout_min = 5;   // inverter default
    }

    ModbusCommand cmd   = ModbusCommand::Auto;
    int32_t       power = 0;
    if (force == SbseSolis::RC_FORCE_CHARGE) {
        power = charge_w;
        cmd   = power > 0 ? ModbusCommand::ForceCharge : ModbusCommand::Idle;
    } else if (force == SbseSolis::RC_FORCE_DISCHARGE) {
        power = discharge_w;
        cmd   = power > 0 ? ModbusCommand::ForceDischarge : ModbusCommand::Idle;
    }

    // Like the real inverter, a remote-control command lapses back to the
    // storage mode (auto) unless the client re-writes it within the timeout.
    solis_rc_deadline_us = cmd == ModbusCommand::Auto
                         ? -1_us
                         : now_us() + micros_t{static_cast<int64_t>(timeout_min) * 60000000LL};

    logger.printfln("solis: RC force %u, charge %ld W, discharge %ld W, timeout %u min -> %s %ld W",
                    force, static_cast<long>(charge_w), static_cast<long>(discharge_w),
                    timeout_min, modbus_command_name(cmd), static_cast<long>(power));
    set_modbus_command(cmd, power);
    refresh_running_mode();
}

void SbseController::apply_solis_dispatch()
{
    using RdMode = SbseSolis::RdMode;

    const uint16_t mode     = solis_settings.get(SbseSolis::REG_RD_MODE);
    const int32_t  raw      = static_cast<int32_t>(
        (static_cast<uint32_t>(solis_settings.get(SbseSolis::REG_RD_POWER)) << 16)
        | solis_settings.get(static_cast<uint16_t>(SbseSolis::REG_RD_POWER + 1)));
    const int32_t  power_w  = raw * 10;
    uint16_t failsafe_min   = solis_settings.get(SbseSolis::REG_RD_FAILSAFE_MIN);
    if (failsafe_min == 0) {
        failsafe_min = 5;
    }

    ModbusCommand cmd   = ModbusCommand::Auto;
    int32_t       power = 0;
    switch (static_cast<RdMode>(mode)) {
        case RdMode::Hold:
            cmd = ModbusCommand::Idle;
            break;
        case RdMode::BatteryPower:              // + charge, - discharge
            if (power_w > 0) {
                cmd = ModbusCommand::ForceCharge;    power = power_w;
            } else if (power_w < 0) {
                cmd = ModbusCommand::ForceDischarge; power = -power_w;
            } else {
                cmd = ModbusCommand::Idle;
            }
            break;
        case RdMode::GridPower:                 // + export, - import
        case RdMode::GridPortPower:
            if (modbus_server_authority >= ModbusAuthority::Full) {
                cmd   = ModbusCommand::GridTarget;
                power = std::clamp(-power_w, int32_t{-750}, int32_t{2500});   // controller: + = import
            } else {
                logger.printfln("solis: dispatch mode %u needs modbus_server_authority = full, running auto", mode);
            }
            break;
        case RdMode::FeedInPriority:            // PV to the grid first
            cmd = ModbusCommand::NoCharge;
            break;
        case RdMode::SelfConsumption:
        default:
            break;
    }

    // Failsafe: like the real inverter, the dispatch command lapses unless
    // the client re-writes it within the failsafe interval.
    solis_rc_deadline_us = cmd == ModbusCommand::Auto
                         ? -1_us
                         : now_us() + micros_t{static_cast<int64_t>(failsafe_min) * 60000000LL};

    logger.printfln("solis: dispatch mode %u, power %ld W, failsafe %u min -> %s %ld W",
                    mode, static_cast<long>(power_w), failsafe_min,
                    modbus_command_name(cmd), static_cast<long>(power));
    set_modbus_command(cmd, power);
    refresh_running_mode();
}

void SbseController::solis_rc_timeout_tick()
{
    if (solis_rc_deadline_us == -1_us || !deadline_elapsed(solis_rc_deadline_us)) {
        return;
    }
    logger.printfln("solis: remote control / dispatch timeout, back to auto");
    solis_rc_deadline_us = -1_us;
    solis_settings.set(SbseSolis::REG_RC_FORCE, SbseSolis::RC_FORCE_NONE);
    solis_settings.set(SbseSolis::REG_RD_ENABLE, 0);
    set_modbus_command(ModbusCommand::Auto, 0);
    refresh_running_mode();
}

// ---------------------------------------------------------------------------
// Protocol-neutral Modbus command handling
//
// Shared by every protocol adapter. Nothing below knows about SMA OpMod
// values or register addresses.
// ---------------------------------------------------------------------------

void SbseController::set_modbus_command(ModbusCommand cmd, int32_t power_w)
{
    // power_w: force magnitude for ForceCharge / ForceDischarge, signed grid
    // target (positive = import) for GridTarget, ignored otherwise.
    const bool forcing = cmd == ModbusCommand::ForceCharge || cmd == ModbusCommand::ForceDischarge;
    modbus_cmd           = cmd;
    modbus_cmd_power_w   = forcing ? std::max(power_w, int32_t{0}) : 0;
    modbus_grid_target_w = cmd == ModbusCommand::GridTarget ? power_w : 0;
    last_modbus_write_us = now_us();
    modbus_active        = true;
    state.get("modbus_active") ->updateBool(true);
    state.get("modbus_cmd")    ->updateString(modbus_command_name(modbus_cmd));
    state.get("modbus_force_w")->updateInt(modbus_force_setpoint_w());
    state.get("modbus_grid_target_w")->updateInt(modbus_grid_target_w);
}

void SbseController::clear_modbus_command()
{
    modbus_cmd           = ModbusCommand::Auto;
    modbus_cmd_power_w   = 0;
    modbus_grid_target_w = 0;
    sma_op_mod           = SMA_OPMOD_DEFAULT;
    // GoodWe adapter: back to EMS auto so a client reading 47511 / 47512
    // sees what is actually in force. Drop a pending debounced apply.
    if (map_apply_task_id != 0) {
        task_scheduler.cancel(map_apply_task_id);
        map_apply_task_id = 0;
    }
    goodwe_settings.set(SbseGoodwe::REG_EMS_MODE, static_cast<uint16_t>(SbseGoodwe::EmsMode::Auto));
    goodwe_settings.set(SbseGoodwe::REG_EMS_POWER, 0);
    // Solis adapter: remote control off.
    solis_settings.set(SbseSolis::REG_RC_FORCE, SbseSolis::RC_FORCE_NONE);
    solis_settings.set(SbseSolis::REG_RD_ENABLE, 0);
    solis_rc_deadline_us = -1_us;
    modbus_active        = false;
    last_modbus_write_us = -1_us;
    state.get("modbus_active") ->updateBool(false);
    state.get("modbus_cmd")    ->updateString(modbus_command_name(ModbusCommand::Auto));
    state.get("modbus_op_mod") ->updateUint(SMA_OPMOD_DEFAULT);
    state.get("modbus_force_w")->updateInt(0);
    state.get("modbus_grid_target_w")->updateInt(0);
}

int32_t SbseController::modbus_force_setpoint_w() const
{
    switch (modbus_cmd) {
        case ModbusCommand::ForceCharge:    return -modbus_cmd_power_w;
        case ModbusCommand::ForceDischarge: return  modbus_cmd_power_w;
        case ModbusCommand::Auto:           return 0;
        case ModbusCommand::Idle:           return 0;
        case ModbusCommand::NoDischarge:    return 0;
        case ModbusCommand::NoCharge:       return 0;
        case ModbusCommand::GridTarget:     return 0;
        default:                            return 0;
    }
}

bool SbseController::modbus_cmd_is_fixed() const
{
    return modbus_cmd == ModbusCommand::ForceCharge
        || modbus_cmd == ModbusCommand::ForceDischarge
        || modbus_cmd == ModbusCommand::Idle;
}

void SbseController::revert_modbus_overrides()
{
    // Restore the live-tunable subset from the persistent config and clear
    // every Modbus-driven runtime field. Called by the watchdog when no
    // Modbus traffic has arrived within the configured timeout, and when the
    // emulated protocol is switched. The command is cleared first so the
    // mode pill refreshed by apply_runtime_from_active() is already correct.
    clear_modbus_command();
    copy_live_tunable_to_active();
    apply_runtime_from_active();
}

void SbseController::watchdog_tick()
{
    solis_rc_timeout_tick();

    if (!modbus_active || modbus_server_watchdog_ms == 0 || last_modbus_write_us == -1_us) {
        return;
    }
    const micros_t timeout{static_cast<int64_t>(modbus_server_watchdog_ms) * 1000};
    if (!deadline_elapsed(last_modbus_write_us + timeout)) {
        return;
    }
    logger.printfln("modbus watchdog: no client traffic for %lus, reverting to persistent config",
                    modbus_server_watchdog_ms / 1000u);
    revert_modbus_overrides();
}

