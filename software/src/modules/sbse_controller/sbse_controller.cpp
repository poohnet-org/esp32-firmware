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

#include <math.h>
#include <stdlib.h>
#include <algorithm>

#include "event_log_prefix.h"
#include "generated/module_dependencies.h"
#include "tools/net.h"

#include "gcc_warnings.h"

// ---------------------------------------------------------------------------
// SBSE register map -- taken verbatim from the production Node-RED flow
// (SMA Sunny Boy Smart Energy 5.0). Hard-coded by design: this module is
// SBSE-only. Address values are SMA "Modbus addresses" passed unchanged to
// TFModbusTCPClient::transact().
// ---------------------------------------------------------------------------

static constexpr uint8_t  GRID_METER_UNIT_ID         = 2;
static constexpr uint16_t GRID_POWER_ADDR            = 31249;  // int32be [W], positive = export, negated below
static constexpr uint16_t GRID_POWER_REG_COUNT       = 2;

static constexpr uint8_t  INVERTER_UNIT_ID           = 3;

// 4 successive uint32be registers starting at 31585:
//   [0] charging power [W],   [1] _,   [2] _,   [3] discharging power [W]
// battery_w = discharge - charge (positive = discharging, negative = charging)
static constexpr uint16_t BATTERY_POWER_ADDR         = 31585;
static constexpr uint16_t BATTERY_POWER_REG_COUNT    = 8;
static constexpr size_t   BATTERY_CHARGE_REG_OFFSET    = 0;
static constexpr size_t   BATTERY_DISCHARGE_REG_OFFSET = 6;  // register index of 4th uint32 inside the block

static constexpr uint16_t BATTERY_SOC_ADDR           = 30845;  // uint32be [%]
static constexpr uint16_t BATTERY_SOC_REG_COUNT      = 2;

// Active-power setpoint block. Writes two consecutive int32be values:
//   [0] battery active-power setpoint [W]
//   [1] companion value, see SBSE_COMPANION_VALUE comment.
static constexpr uint16_t POWER_SETPOINT_ADDR        = 41467;
static constexpr uint16_t POWER_SETPOINT_REG_COUNT   = 4;

// Second int32 written alongside the setpoint. Empirically -15000 in the
// production Node-RED flow (working setup). FIXME: cross-reference with the
// SMA SBSE Modbus profile to give this a proper name. Most likely a reactive-
// power / mode-flag value tied to register 41469.
static constexpr int32_t  SBSE_COMPANION_VALUE       = -15000;

// Modbus transaction timeout. The Node-RED flow uses 2 s with this device.
static constexpr micros_t MODBUS_TIMEOUT             = 2_s;

// How long `force_release` keeps the loop paused. Note: the SBSE does NOT
// auto-revert to internal control when external setpoints stop arriving --
// it is configured for one mode or the other. So this is a UX timeout for
// the operator, not an inverter-watchdog window.
static constexpr micros_t FORCE_RELEASE_HOLD         = 30_s;

// ---------------------------------------------------------------------------
// SMA OpMod values latched into modbus_op_mod and consulted from the setpoint
// dispatch handler. The wire-level protocol (register addresses + lengths,
// listener lifecycle) lives in SbseModbusServer.
//
//   2424 = Default     -> P-controller in charge (with whatever caps are set)
//   2289 = Battery charging   -> force charge at the BatChaMax value
//   2290 = Battery discharging-> force discharge at the BatDchgMax value
// ---------------------------------------------------------------------------

static constexpr uint16_t SMA_OPMOD_DEFAULT           = 2424;
static constexpr uint16_t SMA_OPMOD_FORCE_CHARGE      = 2289;
static constexpr uint16_t SMA_OPMOD_FORCE_DISCHARGE   = 2290;

// ---------------------------------------------------------------------------

static int32_t read_int32be(const uint16_t *buf)
{
    // Pool was constructed with TFModbusTCPByteOrder::Host: each register
    // arrives in host byte order. SBSE registers are MSB-first on the wire,
    // so the first register holds the high half.
    return static_cast<int32_t>((static_cast<uint32_t>(buf[0]) << 16) | buf[1]);
}

static uint32_t read_uint32be(const uint16_t *buf)
{
    return (static_cast<uint32_t>(buf[0]) << 16) | buf[1];
}

static void write_int32be(uint16_t *buf, int32_t value)
{
    uint32_t u = static_cast<uint32_t>(value);
    buf[0] = static_cast<uint16_t>(u >> 16);
    buf[1] = static_cast<uint16_t>(u & 0xFFFFu);
}

static const char *mode_name(SbseController::Mode m)
{
    switch (m) {
        case SbseController::Mode::Disabled:        return "disabled";
        case SbseController::Mode::NotConnected:    return "not_connected";
        case SbseController::Mode::Stale:           return "stale";
        case SbseController::Mode::Running:         return "running";
        case SbseController::Mode::Faulted:         return "faulted";
        case SbseController::Mode::Paused:          return "paused";
        case SbseController::Mode::Safety:          return "safety";
        case SbseController::Mode::ForceCharge:     return "force_charge";
        case SbseController::Mode::ForceDischarge:  return "force_discharge";
        case SbseController::Mode::Blocked:         return "blocked";
        case SbseController::Mode::BlockCharge:     return "block_charge";
        case SbseController::Mode::BlockDischarge:  return "block_discharge";
        default:                                    return "?";
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
    static_assert(sizeof(buf_grid)     / sizeof(buf_grid[0])     == GRID_POWER_REG_COUNT,    "buf_grid size");
    static_assert(sizeof(buf_battery)  / sizeof(buf_battery[0])  == BATTERY_POWER_REG_COUNT, "buf_battery size");
    static_assert(sizeof(buf_soc)      / sizeof(buf_soc[0])      == BATTERY_SOC_REG_COUNT,   "buf_soc size");
    static_assert(sizeof(buf_setpoint) / sizeof(buf_setpoint[0]) == POWER_SETPOINT_REG_COUNT, "buf_setpoint size");
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
        {"target_grid_w",    Config::Int(0, -10000, 10000)},
        {"max_charge_w",     Config::Uint(5000, 0, 10000)},
        {"max_discharge_w",  Config::Uint(5000, 0, 10000)},
        {"kp_milli",         Config::Uint(1000, 100, 2000)},  // Kp * 1000 in [0.1 .. 2.0]
        {"kd_milli",         Config::Uint(0,    0, 3000)},    // Kd * 1000, 0 disables the D term
        {"alpha_grid_milli", Config::Uint(300, 10, 1000)},    // alpha * 1000 in (0.01 .. 1.0]
        {"alpha_setpoint_milli", Config::Uint(700, 10, 1000)},
        {"deadband_w",       Config::Uint(50, 0, 1000)},
        {"safety_zero_after_failures", Config::Uint(5, 0, 100)},  // 0 disables
        {"simulation_mode",  Config::Bool(false)},
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
        // target_grid_w is a setpoint (a desired value), max_charge_w /
        // max_discharge_w are actuator saturation limits. They're orthogonal:
        // an "unreachable" target (e.g. +1000 W with max_charge_w = 100) is
        // not invalid -- the controller just saturates at the limit and the
        // grid balances wherever it ends up.
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
        {"target_grid_w",    Config::Int(0, -10000, 10000)},
        {"max_charge_w",     Config::Uint(5000, 0, 10000)},
        {"max_discharge_w",  Config::Uint(5000, 0, 10000)},
        {"kp_milli",         Config::Uint(1000, 100, 2000)},
        {"kd_milli",         Config::Uint(0,    0, 3000)},
        {"alpha_grid_milli", Config::Uint(300, 10, 1000)},
        {"alpha_setpoint_milli", Config::Uint(700, 10, 1000)},
        {"deadband_w",       Config::Uint(50, 0, 1000)},
        {"safety_zero_after_failures", Config::Uint(5, 0, 100)},
        {"simulation_mode",  Config::Bool(false)},
    }), [](Config & /*cfg*/, ConfigSource /*source*/) -> String {
        // No cross-field constraints. target_grid_w is a setpoint;
        // max_charge_w / max_discharge_w are saturation limits. Per-field
        // range constraints are enforced by the Config types themselves.
        return "";
    }};

    state = Config::Object({
        {"mode",              Config::Str(mode_name(Mode::Disabled), 0, 16)},
        {"last_setpoint_w",   Config::Int32(0)},
        {"last_write_age_ms", Config::Uint32(0)},
        {"grid_w_raw",        Config::Int32(0)},
        {"grid_w_ema",        Config::Int32(0)},
        {"battery_w",         Config::Int32(0)},
        {"battery_soc",       Config::Uint8(255)},
        {"write_ok_count",    Config::Uint32(0)},
        {"write_err_count",   Config::Uint32(0)},
        {"read_fail_streak",  Config::Uint32(0)},
        {"simulation_mode",   Config::Bool(false)},
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
    active_config.get("target_grid_w")          ->updateInt (config.get("target_grid_w")        ->asInt());
    active_config.get("max_charge_w")           ->updateUint(config.get("max_charge_w")         ->asUint());
    active_config.get("max_discharge_w")        ->updateUint(config.get("max_discharge_w")      ->asUint());
    active_config.get("kp_milli")               ->updateUint(config.get("kp_milli")             ->asUint());
    active_config.get("kd_milli")               ->updateUint(config.get("kd_milli")             ->asUint());
    active_config.get("alpha_grid_milli")       ->updateUint(config.get("alpha_grid_milli")     ->asUint());
    active_config.get("alpha_setpoint_milli")   ->updateUint(config.get("alpha_setpoint_milli") ->asUint());
    active_config.get("deadband_w")             ->updateUint(config.get("deadband_w")           ->asUint());
    active_config.get("safety_zero_after_failures")->updateUint(config.get("safety_zero_after_failures")->asUint());
    active_config.get("simulation_mode")        ->updateBool(config.get("simulation_mode")      ->asBool());
}

void SbseController::apply_runtime_from_active()
{
    // Refresh the cached fields the hot path reads.
    target_grid_w   = active_config.get("target_grid_w")->asInt();
    max_charge_w    = static_cast<int32_t>(active_config.get("max_charge_w")->asUint());
    max_discharge_w = static_cast<int32_t>(active_config.get("max_discharge_w")->asUint());
    kp              = static_cast<float>(active_config.get("kp_milli")->asUint())             / 1000.0f;
    kd              = static_cast<float>(active_config.get("kd_milli")->asUint())             / 1000.0f;
    alpha_grid      = static_cast<float>(active_config.get("alpha_grid_milli")->asUint())     / 1000.0f;
    alpha_setpoint  = static_cast<float>(active_config.get("alpha_setpoint_milli")->asUint()) / 1000.0f;
    deadband_w      = static_cast<int32_t>(active_config.get("deadband_w")->asUint());
    safety_zero_after_failures = active_config.get("safety_zero_after_failures")->asUint();
    simulation_mode = active_config.get("simulation_mode")->asBool();

    state.get("simulation_mode")->updateBool(simulation_mode);
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

    api.addCommand("sbse_controller/force_release", Config::Null(), {},
                   [this](Language /*language*/, String &/*errmsg*/) {
        paused       = true;
        paused_until = now_us() + FORCE_RELEASE_HOLD;
        // Operator takeover: drop any Modbus force-mode state too.
        modbus_force_w = 0;
        modbus_op_mod  = SMA_OPMOD_DEFAULT;
        modbus_active  = false;
        state.get("modbus_active")->updateBool(false);
        state.get("modbus_op_mod")->updateUint(SMA_OPMOD_DEFAULT);
        state.get("modbus_force_w")->updateInt(0);
        send_release();
        publish_mode(Mode::Paused);
        logger.printfln("force_release: pausing setpoint loop for %lld s",
                        static_cast<long long>(FORCE_RELEASE_HOLD.to<seconds_t>().t));
    }, true);

    api.addCommand("sbse_controller/resume", Config::Null(), {},
                   [this](Language /*language*/, String &/*errmsg*/) {
        if (!paused) {
            return;
        }
        paused       = false;
        paused_until = -1_us;
        // Operator takeover. (Idempotent if already cleared by force_release.)
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
    send_release();
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

void SbseController::connect_callback(TFGenericTCPClientConnectResult result)
{
    if (result == TFGenericTCPClientConnectResult::Connected) {
        // Stay in Stale until the first successful read sequence promotes us
        // to Running. Same outcome as a transient read failure -- no write.
        publish_mode(Mode::Stale);
    } else {
        publish_mode(Mode::NotConnected);
    }
}

void SbseController::disconnect_callback(TFGenericTCPClientDisconnectReason /*reason*/)
{
    cycle_in_flight       = false;
    ema_grid_seeded       = false;
    prev_ema_grid_seeded  = false;
    ema_setpoint_seeded   = false;
    // Reset the safety-net streak so a fresh reconnect starts clean and can
    // re-trip the safety setpoint if failures resume.
    consecutive_failures = 0;
    safety_zero_armed   = false;
    state.get("read_fail_streak")->updateUint(0);
    publish_mode(Mode::NotConnected);
}

// ---------------------------------------------------------------------------
// One control cycle: grid read -> battery read -> [soc read if due] ->
// compute -> setpoint write.
//
// Callbacks chain sequentially; the pool serialises Modbus transactions on
// this connection, so there is no overlap with other modules sharing it.
// ---------------------------------------------------------------------------

bool SbseController::begin_cycle()
{
    if (!enabled) {
        publish_mode(Mode::Disabled);
        return false;
    }

    if (paused) {
        if (now_us() >= paused_until) {
            paused = false;
        } else {
            publish_mode(Mode::Paused);
            return false;
        }
    }

    if (connected_client == nullptr) {
        publish_mode(Mode::NotConnected);
        return false;
    }

    if (cycle_in_flight) {
        // Previous cycle still running -- back off; the next tick will retry.
        return false;
    }

    cycle_in_flight = true;
    return true;
}

void SbseController::tick()
{
    watchdog_tick();

    if (!begin_cycle()) {
        return;
    }

    read_grid_power();
}

void SbseController::read_grid_power()
{
    auto *client = static_cast<TFModbusTCPSharedClient *>(connected_client);

    client->transact(GRID_METER_UNIT_ID,
                     TFModbusTCPFunctionCode::ReadInputRegisters,
                     GRID_POWER_ADDR,
                     GRID_POWER_REG_COUNT,
                     buf_grid,
                     MODBUS_TIMEOUT,
    [this](TFModbusTCPClientTransactionResult result, const char *err) {
        if (result != TFModbusTCPClientTransactionResult::Success) {
            cycle_failed("grid_power", result, err);
            return;
        }
        // Wire is signed power in W; Node-RED flow applies scale -1 so that
        // positive = import from grid. Match that here.
        grid_w_raw = -read_int32be(buf_grid);
        read_battery_power();
    });
}

void SbseController::read_battery_power()
{
    auto *client = static_cast<TFModbusTCPSharedClient *>(connected_client);

    client->transact(INVERTER_UNIT_ID,
                     TFModbusTCPFunctionCode::ReadInputRegisters,
                     BATTERY_POWER_ADDR,
                     BATTERY_POWER_REG_COUNT,
                     buf_battery,
                     MODBUS_TIMEOUT,
    [this](TFModbusTCPClientTransactionResult result, const char *err) {
        if (result != TFModbusTCPClientTransactionResult::Success) {
            cycle_failed("battery_power", result, err);
            return;
        }
        const uint32_t charge_w    = read_uint32be(buf_battery + BATTERY_CHARGE_REG_OFFSET);
        const uint32_t discharge_w = read_uint32be(buf_battery + BATTERY_DISCHARGE_REG_OFFSET);
        // Positive = discharging, negative = charging. (At most one of the
        // two registers is non-zero in normal operation, but the subtraction
        // is robust either way.)
        battery_w_raw = static_cast<int32_t>(discharge_w) - static_cast<int32_t>(charge_w);

        if (last_soc_read_ok == -1_us
            || deadline_elapsed(last_soc_read_ok + static_cast<micros_t>(soc_interval_ms))) {
            read_soc();
        } else {
            compute_and_write();
        }
    });
}

void SbseController::read_soc()
{
    auto *client = static_cast<TFModbusTCPSharedClient *>(connected_client);

    client->transact(INVERTER_UNIT_ID,
                     TFModbusTCPFunctionCode::ReadInputRegisters,
                     BATTERY_SOC_ADDR,
                     BATTERY_SOC_REG_COUNT,
                     buf_soc,
                     MODBUS_TIMEOUT,
    [this](TFModbusTCPClientTransactionResult result, const char *err) {
        if (result != TFModbusTCPClientTransactionResult::Success) {
            // SoC failure is not fatal for the control loop -- log and proceed
            // with the most recent known value. The SoC clamp will simply not
            // be tighter than it was last cycle.
            logger.printfln("soc read failed: %s (%d)%s%s",
                            get_tf_modbus_tcp_client_transaction_result_name(result),
                            static_cast<int>(result),
                            err != nullptr ? " / " : "",
                            err != nullptr ? err   : "");
            compute_and_write();
            return;
        }
        const uint32_t v = read_uint32be(buf_soc);
        soc_pct           = v > 100u ? 100u : static_cast<uint8_t>(v);
        last_soc_read_ok  = now_us();
        compute_and_write();
    });
}

void SbseController::compute_and_write()
{
    // If a pause was requested mid-cycle (e.g. force_release fired between
    // reads and write), drop the write -- the release write has already gone
    // out and we don't want to immediately overwrite it.
    if (paused) {
        finish_cycle(Mode::Paused);
        return;
    }

    // All required reads for this cycle landed -- safety net resets.
    if (consecutive_failures != 0 || safety_zero_armed) {
        consecutive_failures = 0;
        safety_zero_armed    = false;
        state.get("read_fail_streak")->updateUint(0);
    }

    // 1) Smooth grid (seed on first sample so the first cycle is correct).
    if (!ema_grid_seeded) {
        ema_grid_w        = static_cast<float>(grid_w_raw);
        ema_grid_seeded   = true;
    } else {
        ema_grid_w = alpha_grid * static_cast<float>(grid_w_raw)
                   + (1.0f - alpha_grid) * ema_grid_w;
    }

    // 2) Derivative on the (smoothed) measurement, not on the error. This
    //    anticipates fast load steps that the grid EMA would otherwise see
    //    only after one or two cycles of lag, and -- by deriving on the
    //    measurement -- avoids a derivative kick when the operator changes
    //    target_grid_w. The first cycle after seeding contributes nothing.
    float d_grid = 0.0f;
    if (prev_ema_grid_seeded) {
        d_grid = ema_grid_w - prev_ema_grid_w;
    }
    prev_ema_grid_w      = ema_grid_w;
    prev_ema_grid_seeded = true;

    // 3a) Force-mode bypass. A Modbus client commanded a fixed battery power
    //     via SMA OpMod 2289 (charge) or 2290 (discharge). Skip the P loop,
    //     skip the grid EMA derivative -- command the exact requested watts.
    //     Output filtering (EMA on setpoint, deadband, SoC clamp) still
    //     applies so a force command can't bypass safety or thrash the bus.
    //
    // 3b) Otherwise: P + implicit-I (via battery_w_raw) + D-on-measurement.
    float raw_setpoint;
    if (modbus_force_w != 0) {
        raw_setpoint = static_cast<float>(modbus_force_w);
    } else {
        const float delta_w = ema_grid_w - static_cast<float>(target_grid_w);
        raw_setpoint = static_cast<float>(battery_w_raw)
                     + kp * delta_w
                     + kd * d_grid;
    }

    // 3) SoC limits (only when we have a usable reading).
    if (soc_pct != 255) {
        if (soc_pct >= 100 && raw_setpoint < 0.0f) { raw_setpoint = 0.0f; }
        if (soc_pct ==   0 && raw_setpoint > 0.0f) { raw_setpoint = 0.0f; }
    }

    // 4) Hard limits.
    raw_setpoint = std::clamp(raw_setpoint,
                              static_cast<float>(-max_charge_w),
                              static_cast<float>(max_discharge_w));

    // 5) Smooth the commanded setpoint.
    if (!ema_setpoint_seeded) {
        ema_setpoint_w        = raw_setpoint;
        ema_setpoint_seeded   = true;
    } else {
        ema_setpoint_w = alpha_setpoint * raw_setpoint
                       + (1.0f - alpha_setpoint) * ema_setpoint_w;
    }

    // 6) NaN / inf guard. If anything upstream produced a non-finite value,
    //    refuse to write -- the inverter would hold the last good setpoint,
    //    which is safer than commanding a garbage value.
    if (!isfinite(ema_setpoint_w)) {
        state.get("last_error")->updateString("non-finite setpoint computed");
        finish_cycle(Mode::Faulted);
        return;
    }

    const int32_t target_w = lroundf(ema_setpoint_w);

    state.get("grid_w_raw")->updateInt(grid_w_raw);
    state.get("grid_w_ema")->updateInt(lroundf(ema_grid_w));
    state.get("battery_w") ->updateInt(battery_w_raw);
    state.get("battery_soc")->updateUint(soc_pct);

    trace_history.add_sample(lroundf(ema_grid_w), battery_w_raw, last_written_w, target_grid_w);

    // 7) Deadband. If the new target is within deadband of the last write,
    //    skip the write -- the SBSE holds the last commanded setpoint
    //    indefinitely (no internal-control fallback), so there is no
    //    watchdog to satisfy. This cuts write traffic and cell churn.
    if (last_write_ok != -1_us && std::abs(target_w - last_written_w) < deadband_w) {
        finish_cycle(current_running_mode());
        return;
    }

    send_setpoint(target_w);
}

void SbseController::send_setpoint(int32_t watts)
{
    if (simulation_mode) {
        // Skip the Modbus write but mirror every state change a real
        // successful write would have produced, so the deadband logic, the
        // dashboard counters, and the live chart all behave as if the write
        // had gone out.
        last_written_w = watts;
        last_write_ok  = now_us();
        ++write_ok_count;
        state.get("write_ok_count")->updateUint(write_ok_count);
        publish_setpoint(watts);
        finish_cycle(current_running_mode());
        return;
    }

    write_int32be(buf_setpoint + 0, watts);
    write_int32be(buf_setpoint + 2, SBSE_COMPANION_VALUE);

    auto *client = static_cast<TFModbusTCPSharedClient *>(connected_client);

    client->transact(INVERTER_UNIT_ID,
                     TFModbusTCPFunctionCode::WriteMultipleRegisters,
                     POWER_SETPOINT_ADDR,
                     POWER_SETPOINT_REG_COUNT,
                     buf_setpoint,
                     MODBUS_TIMEOUT,
    [this, watts](TFModbusTCPClientTransactionResult result, const char *err) {
        if (result != TFModbusTCPClientTransactionResult::Success) {
            ++write_err_count;
            state.get("write_err_count")->updateUint(write_err_count);

            char msg[64];
            snprintf(msg, sizeof(msg), "write failed: %s (%d)",
                     get_tf_modbus_tcp_client_transaction_result_name(result),
                     static_cast<int>(result));
            state.get("last_error")->updateString(msg);

            logger.printfln("setpoint write failed: %s (%d)%s%s",
                            get_tf_modbus_tcp_client_transaction_result_name(result),
                            static_cast<int>(result),
                            err != nullptr ? " / " : "",
                            err != nullptr ? err   : "");
            finish_cycle(Mode::Stale);
            return;
        }

        last_written_w  = watts;
        last_write_ok   = now_us();
        ++write_ok_count;
        state.get("write_ok_count")->updateUint(write_ok_count);

        publish_setpoint(watts);
        finish_cycle(current_running_mode());
    });
}

void SbseController::send_release()
{
    // Zero-setpoint write. The SBSE has no auto-fallback to internal control,
    // so it will hold this 0 W command until either we issue a new setpoint
    // or the operator switches the inverter back to internal control. Used
    // by `force_release` (operator-driven pause) and `pre_reboot` (so we
    // don't leave a stale active setpoint commanding the battery).
    //
    // In simulation mode we skip the actual write but the operator-visible
    // pause behaviour (handled in the caller) is unaffected.
    if (connected_client == nullptr || simulation_mode) {
        return;
    }

    write_int32be(buf_setpoint + 0, 0);
    write_int32be(buf_setpoint + 2, SBSE_COMPANION_VALUE);

    auto *client = static_cast<TFModbusTCPSharedClient *>(connected_client);

    client->transact(INVERTER_UNIT_ID,
                     TFModbusTCPFunctionCode::WriteMultipleRegisters,
                     POWER_SETPOINT_ADDR,
                     POWER_SETPOINT_REG_COUNT,
                     buf_setpoint,
                     MODBUS_TIMEOUT,
    [](TFModbusTCPClientTransactionResult /*result*/, const char * /*err*/) {
        // Best effort, no state churn.
    });
}

void SbseController::cycle_failed(const char *where,
                                  TFModbusTCPClientTransactionResult result,
                                  const char *error_message)
{
    char msg[64];
    snprintf(msg, sizeof(msg), "%s read failed: %s (%d)",
             where,
             get_tf_modbus_tcp_client_transaction_result_name(result),
             static_cast<int>(result));
    state.get("last_error")->updateString(msg);

    logger.printfln("%s read failed: %s (%d)%s%s",
                    where,
                    get_tf_modbus_tcp_client_transaction_result_name(result),
                    static_cast<int>(result),
                    error_message != nullptr ? " / " : "",
                    error_message != nullptr ? error_message : "");

    ++consecutive_failures;
    state.get("read_fail_streak")->updateUint(consecutive_failures);

    // Trip the safety net: command 0 W exactly once when the streak first
    // crosses the threshold. The SBSE will hold 0 W until reads recover
    // (which clears the armed flag in compute_and_write) or until the
    // operator switches the inverter back to internal control.
    if (safety_zero_after_failures != 0
        && !safety_zero_armed
        && consecutive_failures >= safety_zero_after_failures
        && connected_client != nullptr) {
        safety_zero_armed = true;
        logger.printfln("read failure streak hit %lu, commanding 0 W safety setpoint",
                        static_cast<unsigned long>(consecutive_failures));
        send_safety_zero();
        return;
    }

    finish_cycle(safety_zero_armed ? Mode::Safety : Mode::Stale);
}

void SbseController::send_safety_zero()
{
    if (simulation_mode) {
        last_written_w = 0;
        last_write_ok  = now_us();
        ++write_ok_count;
        state.get("write_ok_count")->updateUint(write_ok_count);
        publish_setpoint(0);
        finish_cycle(Mode::Safety);
        return;
    }

    write_int32be(buf_setpoint + 0, 0);
    write_int32be(buf_setpoint + 2, SBSE_COMPANION_VALUE);

    auto *client = static_cast<TFModbusTCPSharedClient *>(connected_client);

    client->transact(INVERTER_UNIT_ID,
                     TFModbusTCPFunctionCode::WriteMultipleRegisters,
                     POWER_SETPOINT_ADDR,
                     POWER_SETPOINT_REG_COUNT,
                     buf_setpoint,
                     MODBUS_TIMEOUT,
    [this](TFModbusTCPClientTransactionResult result, const char *err) {
        if (result == TFModbusTCPClientTransactionResult::Success) {
            last_written_w = 0;
            last_write_ok  = now_us();
            ++write_ok_count;
            state.get("write_ok_count")->updateUint(write_ok_count);
            publish_setpoint(0);
        } else {
            ++write_err_count;
            state.get("write_err_count")->updateUint(write_err_count);
            // Leave safety_zero_armed set: we don't want to retry every cycle.
            // Reads recovering is the only thing that clears the armed flag.
            logger.printfln("safety-zero write failed: %s (%d)%s%s",
                            get_tf_modbus_tcp_client_transaction_result_name(result),
                            static_cast<int>(result),
                            err != nullptr ? " / " : "",
                            err != nullptr ? err   : "");
        }
        finish_cycle(Mode::Safety);
    });
}

void SbseController::finish_cycle(Mode mode)
{
    cycle_in_flight = false;
    publish_mode(mode);
}

void SbseController::publish_setpoint(int32_t watts)
{
    state.get("last_setpoint_w")->updateInt(watts);
}

void SbseController::publish_mode(Mode mode)
{
    state.get("mode")->updateString(mode_name(mode));
    if (last_write_ok != -1_us) {
        const millis_t age = (now_us() - last_write_ok).to<millis_t>();
        state.get("last_write_age_ms")->updateUint(static_cast<uint32_t>(age.t));
    }
}

SbseController::Mode SbseController::current_running_mode() const
{
    // Selects which "is running" Mode the controller should report.
    // ForceCharge/Discharge are visually distinct in the dashboard pill so the
    // operator can see at a glance that an external Modbus client is steering.
    if (modbus_force_w < 0) {
        return Mode::ForceCharge;
    }
    if (modbus_force_w > 0) {
        return Mode::ForceDischarge;
    }
    // The block-* modes are derived from the saturation limits regardless of
    // source: Modbus, dashboard, MQTT and HTTP all surface the same way.
    if (max_charge_w == 0 && max_discharge_w == 0) {
        return Mode::Blocked;
    }
    if (max_charge_w == 0) {
        return Mode::BlockCharge;
    }
    if (max_discharge_w == 0) {
        return Mode::BlockDischarge;
    }
    return Mode::Running;
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
    // operator's configured target_grid_w across Modbus traffic.
    if (modbus_server_use_grid_spt) {
        active_config.get("target_grid_w")->updateInt(target_clamped);
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

