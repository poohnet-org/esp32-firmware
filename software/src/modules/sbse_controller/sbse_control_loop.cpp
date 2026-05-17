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

// The SBSE control loop: read grid power, read battery power, read SoC if
// due, compute the new battery active-power setpoint via P + implicit-I +
// D-on-measurement, clamp by SoC and the operator's max-charge/discharge
// limits, smooth via the output EMA, and write back to the inverter --
// unless a Modbus client is force-mode-steering or the deadband suppresses
// the write.

#define EVENT_LOG_PREFIX "sbse_ctrl"

#include "sbse_controller.h"

#include <math.h>
#include <stdlib.h>
#include <algorithm>

#include "event_log_prefix.h"
#include "generated/module_dependencies.h"

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

// Compile-time agreement between the SbseController::buf_* member sizes
// declared in sbse_controller.h and the register counts above. If a register
// block is resized, both must update.
static_assert(SbseController::BUF_GRID_LEN     == GRID_POWER_REG_COUNT,     "buf_grid size");
static_assert(SbseController::BUF_BATTERY_LEN  == BATTERY_POWER_REG_COUNT,  "buf_battery size");
static_assert(SbseController::BUF_SOC_LEN      == BATTERY_SOC_REG_COUNT,    "buf_soc size");
static_assert(SbseController::BUF_SETPOINT_LEN == POWER_SETPOINT_REG_COUNT, "buf_setpoint size");

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
// Modbus client connection lifecycle
// ---------------------------------------------------------------------------

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
    // If a pause was requested mid-cycle (e.g. the pause command fired
    // between reads and write), drop the write -- the 0 W write has
    // already gone out and we don't want to immediately overwrite it.
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
    // 3b) Hard target (lo == hi): symmetric chase of the single grid value
    //     in both directions, using the standard P + implicit-I + D law.
    //     Identical to the legacy single-target behaviour.
    //
    // 3c) Soft target (lo < hi): asymmetric.
    //       - Always chase lo by *charging* the battery. The clamp
    //         setpoint <= 0 prevents discharge for chase-lo: if there's
    //         not enough excess PV to reach lo, the controller just
    //         stops charging (battery -> 0) and the grid drifts up
    //         rather than burning battery to pad the export.
    //       - When ema_grid drifts above hi, switch to chasing hi by
    //         *discharging* the battery. This implements the
    //         "don't import past hi" rescue path. At the hi boundary the
    //         two branches agree in steady state (battery near 0,
    //         setpoint near 0) so the transition is smooth.
    float raw_setpoint;
    if (modbus_force_w != 0) {
        raw_setpoint = static_cast<float>(modbus_force_w);
    } else if (grid_charge_target_w == grid_discharge_target_w) {
        // hard mode
        const float t       = static_cast<float>(grid_charge_target_w);
        const float delta_w = ema_grid_w - t;
        raw_setpoint = static_cast<float>(battery_w_raw)
                     + kp * delta_w
                     + kd * d_grid;
    } else {
        // soft mode -- asymmetric
        const float lo = static_cast<float>(grid_charge_target_w);
        const float hi = static_cast<float>(grid_discharge_target_w);
        if (ema_grid_w > hi) {
            // chase hi (discharge to bring grid down to hi)
            const float delta_w = ema_grid_w - hi;
            raw_setpoint = static_cast<float>(battery_w_raw)
                         + kp * delta_w
                         + kd * d_grid;
        } else {
            // chase lo (charge to bring grid down to lo); never discharge
            const float delta_w = ema_grid_w - lo;
            raw_setpoint = static_cast<float>(battery_w_raw)
                         + kp * delta_w
                         + kd * d_grid;
            if (raw_setpoint > 0.0f) {
                raw_setpoint = 0.0f;
            }
        }
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

    trace_history.add_sample(lroundf(ema_grid_w), battery_w_raw, last_written_w,
                             grid_charge_target_w, grid_discharge_target_w);

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

void SbseController::send_zero_w()
{
    // Zero-setpoint write. The SBSE has no auto-fallback to internal control,
    // so it will hold this 0 W command until either we issue a new setpoint
    // or the operator switches the inverter back to internal control. Used
    // by the `pause` command (operator-driven pause) and `pre_reboot` (so we
    // don't leave a stale active setpoint commanding the battery).
    if (connected_client == nullptr) {
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
