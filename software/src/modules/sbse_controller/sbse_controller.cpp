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
// SMA OpMod values latched into modbus_op_mod and consulted from the setpoint
// dispatch handler. The wire-level protocol (register addresses + lengths,
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
        {"grid_charge_target_w",    Config::Int(0, -10000, 10000)},   // lower bound
        {"grid_discharge_target_w", Config::Int(0, -10000, 10000)},   // upper bound
        {"max_charge_w",     Config::Uint(5000, 0, 10000)},
        {"max_discharge_w",  Config::Uint(5000, 0, 10000)},
        {"kp_milli",         Config::Uint(1000, 100, 2000)},  // Kp * 1000 in [0.1 .. 2.0]
        {"kd_milli",         Config::Uint(0,    0, 3000)},    // Kd * 1000, 0 disables the D term
        {"alpha_grid_milli", Config::Uint(300, 10, 1000)},    // alpha * 1000 in (0.01 .. 1.0]
        {"alpha_setpoint_milli", Config::Uint(700, 10, 1000)},
        {"deadband_w",       Config::Uint(50, 0, 1000)},
        {"safety_zero_after_failures", Config::Uint(5, 0, 100)},  // 0 disables
        // SMA-compatible Modbus TCP server. All four fields are init-only:
        // changing them stops/starts the server but doesn't touch active_config.
        {"modbus_server_enabled",     Config::Bool(false)},
        {"modbus_server_port",        Config::Uint16(502)},
        {"modbus_server_unit_id",     Config::Uint(3, 0, 247)},   // 0 = accept any
        {"modbus_server_watchdog_s",  Config::Uint(60, 0, 3600)},  // 0 disables
        // false (default): Modbus writes to 40793 leave target_grid_w alone.
        // WARP always sends GridWSpt = 0, so without this guard a Block /
        // Normal / Block-* write would always zero the operator's target.
        // true: GridWSpt is mirrored into active_config.target_grid_w.
        {"modbus_server_use_grid_spt", Config::Bool(false)},
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
        {"grid_charge_target_w",    Config::Int(0, -10000, 10000)},
        {"grid_discharge_target_w", Config::Int(0, -10000, 10000)},
        {"max_charge_w",     Config::Uint(5000, 0, 10000)},
        {"max_discharge_w",  Config::Uint(5000, 0, 10000)},
        {"kp_milli",         Config::Uint(1000, 100, 2000)},
        {"kd_milli",         Config::Uint(0,    0, 3000)},
        {"alpha_grid_milli", Config::Uint(300, 10, 1000)},
        {"alpha_setpoint_milli", Config::Uint(700, 10, 1000)},
        {"deadband_w",       Config::Uint(50, 0, 1000)},
        {"safety_zero_after_failures", Config::Uint(5, 0, 100)},
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
        {"modbus_op_mod",     Config::Uint16(SMA_OPMOD_DEFAULT)},
        {"modbus_force_w",    Config::Int32(0)},
        {"last_error",        Config::Str("", 0, 64)},
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

    modbus_server_enabled       = config.get("modbus_server_enabled")->asBool();
    modbus_server_port          = config.get("modbus_server_port")->asUint16();
    modbus_server_unit_id       = static_cast<uint8_t>(config.get("modbus_server_unit_id")->asUint());
    modbus_server_watchdog_ms   = config.get("modbus_server_watchdog_s")->asUint() * 1000u;
    modbus_server_use_grid_spt  = config.get("modbus_server_use_grid_spt")->asBool();
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
        // bind-time fields (enabled / port) actually changed; the rest
        // (unit_id, watchdog, use_grid_spt) are consulted live and don't
        // require a restart.
        modbus_server_enabled       = config.get("modbus_server_enabled")->asBool();
        modbus_server_port          = config.get("modbus_server_port")->asUint16();
        modbus_server_unit_id       = static_cast<uint8_t>(config.get("modbus_server_unit_id")->asUint());
        modbus_server_watchdog_ms   = config.get("modbus_server_watchdog_s")->asUint() * 1000u;
        modbus_server_use_grid_spt  = config.get("modbus_server_use_grid_spt")->asBool();
        if (modbus_server.needs_restart_for(modbus_server_enabled, modbus_server_port)) {
            modbus_server.configure(modbus_server_enabled, modbus_server_port, modbus_server_unit_id);
            modbus_server.restart();
        } else {
            modbus_server.configure(modbus_server_enabled, modbus_server_port, modbus_server_unit_id);
        }
    }, false);

    // Live-tunable runtime overlay (no flash write). HTTP/MQTT writes go
    // through this command callback; internal Modbus dispatch mutates
    // active_config directly to avoid clearing its own force-mode state.
    api.addState("sbse_controller/active_config", &active_config);
    api.addCommand("sbse_controller/active_config_update", &active_config, {},
                   [this](Language /*language*/, String &/*errmsg*/) {
        apply_runtime_from_active();
        // Operator (dashboard / HTTP / MQTT) takeover -- release any Modbus
        // force-mode setpoint so the P controller is back in charge of
        // hitting the new target.
        if (modbus_force_w != 0 || modbus_active) {
            logger.printfln("operator override -- releasing Modbus force-mode setpoint");
        }
        modbus_force_w = 0;
        modbus_op_mod  = SMA_OPMOD_DEFAULT;
        modbus_active  = false;
        state.get("modbus_active")->updateBool(false);
        state.get("modbus_op_mod")->updateUint(SMA_OPMOD_DEFAULT);
        state.get("modbus_force_w")->updateInt(0);
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
        modbus_force_w = 0;
        modbus_op_mod  = SMA_OPMOD_DEFAULT;
        modbus_active  = false;
        state.get("modbus_active")->updateBool(false);
        state.get("modbus_op_mod")->updateUint(SMA_OPMOD_DEFAULT);
        state.get("modbus_force_w")->updateInt(0);
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
        // Operator takeover. (Idempotent if already cleared by pause.)
        modbus_force_w = 0;
        modbus_op_mod  = SMA_OPMOD_DEFAULT;
        modbus_active  = false;
        state.get("modbus_active")->updateBool(false);
        state.get("modbus_op_mod")->updateUint(SMA_OPMOD_DEFAULT);
        state.get("modbus_force_w")->updateInt(0);

        // Publish a plausible mode immediately so the dashboard doesn't sit on
        // "paused" until the next tick fires. The tick will correct it if
        // anything is off (e.g. read failure -> stale).
        if (!enabled) {
            publish_mode(Mode::Disabled);
        } else if (connected_client == nullptr) {
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
    // a no-op until connect_callback flips connected_client to non-null.
    tick_task_id = task_scheduler.scheduleWithFixedDelay([this]() {
        this->tick();
    }, tick_ms, tick_ms);

    // Wire the SMA Modbus server's protocol-adapter callbacks once, then
    // start if enabled.
    modbus_server.set_handlers(
        [this](uint32_t op_mod) {
            return this->on_modbus_op_mod_write(op_mod);
        },
        [this](const uint16_t *regs10) {
            return this->on_modbus_setpoint_write(regs10);
        });
    modbus_server.configure(modbus_server_enabled, modbus_server_port, modbus_server_unit_id);
    if (modbus_server_enabled) {
        modbus_server.start();
    }
}

void SbseController::pre_reboot()
{
    if (tick_task_id != 0) {
        task_scheduler.cancel(tick_task_id);
        tick_task_id = 0;
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
// SMA Modbus server handlers
//
// These are the protocol-layer callbacks SbseModbusServer invokes on each
// accepted WriteMultipleRegisters. The server has already validated the
// unit-id filter and the address/length tuple; here we carry the semantic
// state (sticky OpMod, force-mode, active_config wiring, watchdog timer).
// ---------------------------------------------------------------------------

TFModbusTCPExceptionCode SbseController::on_modbus_op_mod_write(uint32_t op_mod)
{
    // 40236: CmpBMS.OpMod (U32BE). Latch for the next 40793 write. We accept
    // the three documented SMA values (2289/2290/2424); anything else is
    // treated as "Default" so a buggy client can't trap us in a force-mode
    // we can't get out of without a reboot.
    if (op_mod == SMA_OPMOD_FORCE_CHARGE
        || op_mod == SMA_OPMOD_FORCE_DISCHARGE
        || op_mod == SMA_OPMOD_DEFAULT) {
        modbus_op_mod = static_cast<uint16_t>(op_mod);
    } else {
        modbus_op_mod = SMA_OPMOD_DEFAULT;
    }
    state.get("modbus_op_mod")->updateUint(modbus_op_mod);
    return TFModbusTCPExceptionCode::Success;
}

TFModbusTCPExceptionCode SbseController::on_modbus_setpoint_write(const uint16_t *regs10)
{
    // 40793: 5 x U32BE -- handed off to the semantic layer which decodes
    // the registers, applies the SMA OpMod interpretation, and mirrors
    // values into active_config.
    apply_modbus_setpoint_block(regs10);
    return TFModbusTCPExceptionCode::Success;
}

void SbseController::apply_modbus_setpoint_block(const uint16_t *data_values)
{
    // 40793: 5 x U32BE. Indices: [0]=BatChaMinW, [1]=BatChaMaxW,
    // [2]=BatDchgMinW, [3]=BatDchgMaxW, [4]=GridWSpt.
    auto u32 = [&](int i) -> uint32_t {
        return (static_cast<uint32_t>(data_values[2 * i]) << 16) | data_values[2 * i + 1];
    };
    const uint32_t bat_cha_max  = u32(1);
    const uint32_t bat_dchg_max = u32(3);
    const int32_t  grid_spt     = static_cast<int32_t>(u32(4));

    // Clamp to the SBSE controller's own range. The Config types would reject
    // out-of-range values; we mirror their bounds here so the Modbus client
    // sees Success instead of a generic error.
    auto clamp_w = [](uint32_t v) -> uint32_t { return v > 10000u ? 10000u : v; };
    const uint32_t max_charge_clamped    = clamp_w(bat_cha_max);
    const uint32_t max_discharge_clamped = clamp_w(bat_dchg_max);
    const int32_t  target_clamped        = std::clamp(grid_spt, int32_t{-10000}, int32_t{10000});

    // Interpret OpMod. Force modes bypass the P controller (see compute_and_write
    // step 3a). OpMod 2424 lets the P controller drive against the new caps and
    // target.
    if (modbus_op_mod == SMA_OPMOD_FORCE_CHARGE) {
        modbus_force_w = -static_cast<int32_t>(max_charge_clamped);
    } else if (modbus_op_mod == SMA_OPMOD_FORCE_DISCHARGE) {
        modbus_force_w =  static_cast<int32_t>(max_discharge_clamped);
    } else {
        modbus_force_w = 0;
    }

    // Mutate active_config in place. This auto-publishes via the API event
    // bus (so MQTT / dashboard see the new values immediately) but skips the
    // active_config_update command handler -- which would otherwise clear
    // the force-mode state we just set.
    //
    // GridWSpt is only mirrored when the operator opted in. WARP always
    // sends GridWSpt = 0 in every mode, so the default (off) preserves the
    // operator's configured grid targets across Modbus traffic. When opted
    // in, GridWSpt is a single value -- write it to BOTH targets so the P
    // controller chases it in both directions (hard-mode semantics, matching
    // what WARP expects from a real SMA inverter).
    if (modbus_server_use_grid_spt) {
        active_config.get("grid_charge_target_w")   ->updateInt(target_clamped);
        active_config.get("grid_discharge_target_w")->updateInt(target_clamped);
    }
    active_config.get("max_charge_w")   ->updateUint(max_charge_clamped);
    active_config.get("max_discharge_w")->updateUint(max_discharge_clamped);
    apply_runtime_from_active();

    last_modbus_write_us = now_us();
    modbus_active        = true;
    state.get("modbus_active") ->updateBool(true);
    state.get("modbus_force_w")->updateInt (modbus_force_w);
}

void SbseController::revert_modbus_overrides()
{
    // Restore the live-tunable subset from the persistent config and clear
    // every Modbus-driven runtime field. Called by the watchdog when no
    // Modbus traffic has arrived within the configured timeout.
    copy_live_tunable_to_active();
    apply_runtime_from_active();

    modbus_force_w = 0;
    modbus_op_mod  = SMA_OPMOD_DEFAULT;
    modbus_active  = false;
    last_modbus_write_us = -1_us;
    state.get("modbus_active") ->updateBool(false);
    state.get("modbus_op_mod") ->updateUint(SMA_OPMOD_DEFAULT);
    state.get("modbus_force_w")->updateInt (0);
}

void SbseController::watchdog_tick()
{
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

