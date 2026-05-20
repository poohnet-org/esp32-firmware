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
    // Keep-alive timer: don't carry an idle measurement across a disconnect,
    // and drop any in-flight two-tick keep-alive event.
    battery_idle_since_us = -1_us;
    keepalive_pending_zero = false;
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
            // Pause held the loop idle long enough that the EMAs and the
            // derivative-on-measurement reference point are stale. Reseed
            // on the first post-pause sample so we don't produce a one-cycle
            // derivative kick from a ~30 s gap in measurements.
            ema_grid_seeded       = false;
            prev_ema_grid_seeded  = false;
            ema_setpoint_seeded   = false;
            // The 30 s pause-write-0 doesn't count toward keep-alive: the
            // operator just asked us to be silent, not to bypass standby
            // prevention right away. Also drop any in-flight keep-alive
            // return-write -- pause already wrote 0.
            battery_idle_since_us  = -1_us;
            keepalive_pending_zero = false;
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
    // Clear last_error now that the read phase succeeded. The NaN guard or
    // a failed write later in this cycle may re-set it. updateString dedups
    // identical values, so this is free when last_error is already empty.
    state.get("last_error")->updateString("");

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

    // 3) Compute the raw battery setpoint.
    //
    //   3a) Force-mode bypass. A Modbus client commanded a fixed battery power
    //       via SMA OpMod 2289 (charge) or 2290 (discharge). Skip the P loop,
    //       skip the EMA derivative -- command the exact requested watts.
    //       Output filtering (EMA on setpoint, deadband, SoC clamp) still
    //       applies so a force command can't bypass safety or thrash the bus.
    //
    //   3b) Otherwise the P + implicit-I + D law runs against an effective
    //       target chosen by where the *natural grid* sits relative to the
    //       [lo, hi] deadzone:
    //
    //         natural_grid = ema_grid_w + battery_w_raw   (= home_load - PV;
    //                                                      grid if battery=0)
    //         target       = clamp(natural_grid, lo, hi)
    //
    //       - natural_grid > hi  -> PV deficit beyond the rescue threshold.
    //                               target = hi: discharge to keep grid <= hi.
    //       - natural_grid < lo  -> PV surplus beyond the export floor.
    //                               target = lo: charge to keep grid >= lo.
    //       - lo <= natural_grid <= hi  -> the deadzone. target = natural_grid
    //                               so the formula yields raw = 0 (with Kp=1)
    //                               or a gentle decay (with Kp<1), winding any
    //                               leftover battery commitment toward idle.
    //
    //       Hard mode (lo == hi) is the natural collapse of this rule:
    //       clamp(., lo, lo) == lo for any natural_grid, so the formula is
    //       a symmetric P+I+D chase of the single target. The direction-lock
    //       clamps below become a "no overshoot through zero" guard so noise
    //       around the target doesn't sign-flip the battery.
    //
    //       Branching on natural_grid (PV vs load vs targets) rather than on
    //       ema_grid alone keeps the law continuous at the deadzone boundary
    //       even when the battery is mid-ramp: a brief ema_grid jiggle across
    //       hi while the battery is actively discharging at steady-state
    //       (load > PV) leaves natural_grid well above hi, so the regime
    //       stays "discharging" and the setpoint doesn't slam to 0 (the
    //       prior implementation's bug).
    float raw_setpoint;
    if (modbus_force_w != 0) {
        raw_setpoint = static_cast<float>(modbus_force_w);
    } else {
        const float lo_f          = static_cast<float>(grid_charge_target_w);
        const float hi_f          = static_cast<float>(grid_discharge_target_w);
        const float natural_grid  = ema_grid_w + static_cast<float>(battery_w_raw);
        const float target_w_f    = std::clamp(natural_grid, lo_f, hi_f);
        const float delta_w       = ema_grid_w - target_w_f;

        raw_setpoint = static_cast<float>(battery_w_raw)
                     + kp * delta_w
                     + kd * d_grid;

        // Direction lock. In each active regime the battery acts in its
        // natural direction only: when natural_grid is above hi we never
        // charge (which would push grid further above hi), and below lo
        // we never discharge (which would burn battery to pad export).
        // Inside the deadzone the formula naturally produces raw == 0 at
        // Kp=1, so no clamp is needed there.
        if (natural_grid > hi_f && raw_setpoint < 0.0f) raw_setpoint = 0.0f;
        if (natural_grid < lo_f && raw_setpoint > 0.0f) raw_setpoint = 0.0f;
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

    int32_t target_w = lroundf(ema_setpoint_w);

    state.get("grid_w_raw")->updateInt(grid_w_raw);
    state.get("grid_w_ema")->updateInt(lroundf(ema_grid_w));
    state.get("battery_w") ->updateInt(battery_w_raw);
    state.get("battery_soc")->updateUint(soc_pct);

    trace_history.add_sample(lroundf(ema_grid_w), battery_w_raw, last_written_w,
                             grid_charge_target_w, grid_discharge_target_w);

    // 7) Keep-alive heartbeat. Track how long the battery has been idle, and
    //    when the controller is about to fall into the "write 0 W again"
    //    deadband path with the inverter approaching its 10-15 min standby
    //    threshold, override the write with a small alternating-sign pulse.
    //    This prevents the 20-30 s wake-up lag when a subsequent regime
    //    change demands prompt battery action.
    //
    //    Only fires in the normal P-controller branch -- force-mode and
    //    safety-zero are explicit operator/system overrides we won't second-
    //    guess; pause is handled by the early return above.
    if (battery_w_raw != 0) {
        battery_idle_since_us = -1_us;
    } else if (battery_idle_since_us == -1_us) {
        battery_idle_since_us = now_us();
    }

    // A keep-alive event is two ticks long:
    //   tick N   -- the pulse: target_w = +/-keepalive_pulse_w, deadband bypassed.
    //   tick N+1 -- the return: target_w forced to 0, deadband bypassed,
    //               ema_setpoint snapped to 0 so the smoother doesn't drag
    //               the next few writes off from idle. The return reliably
    //               winds the inverter back to 0 W regardless of how
    //               keepalive_pulse_w compares to deadband_w.
    bool keepalive_fired = false;
    bool keepalive_return = false;
    if (keepalive_pending_zero) {
        keepalive_pending_zero = false;
        // Only force the 0 W return if no other source of truth has stepped
        // in between the two ticks (force-mode write, etc.). If force-mode
        // is active, its setpoint is already in target_w from the chase
        // block above and the keep-alive event is silently abandoned.
        if (modbus_force_w == 0) {
            keepalive_return = true;
            target_w         = 0;
            ema_setpoint_w   = 0.0f;
        }
    } else if (keepalive_interval_s > 0
               && target_w == 0
               && modbus_force_w == 0
               && battery_idle_since_us != -1_us
               && deadline_elapsed(battery_idle_since_us
                                   + micros_t{static_cast<int64_t>(keepalive_interval_s) * 1000000LL})) {
        const int32_t pulse = pick_keepalive_pulse();
        if (pulse != 0) {
            target_w = pulse;
            keepalive_fired        = true;
            keepalive_pending_zero = true;
            // Defer the next pulse by one full interval. The inverter may
            // take a couple of cycles to read back as non-zero (especially
            // the first pulse breaking it out of standby); without this,
            // battery_w_raw == 0 on the next cycle would re-fire keep-alive
            // every tick.
            battery_idle_since_us  = now_us();
        }
    }

    // 8) Deadband. If the new target is within deadband of the last write,
    //    skip the write -- the SBSE holds the last commanded setpoint
    //    indefinitely (no internal-control fallback), so there is no
    //    watchdog to satisfy. This cuts write traffic and cell churn.
    //
    //    Three exceptions bypass the deadband:
    //      - the keep-alive pulse (target_w replaced with +/- pulse W),
    //      - the keep-alive return (target_w forced back to 0 next tick),
    //      - the active refresh: if the most recent write is older than
    //        keepalive_interval_s, re-assert target_w even when its value
    //        is within deadband_w of the last commanded one. Together with
    //        the idle-pulse path this caps the silent-write gap at the
    //        keep-alive interval whether the battery is active or idle.
    const bool keepalive_refresh_due =
        keepalive_interval_s > 0
        && last_write_ok != -1_us
        && deadline_elapsed(last_write_ok
                            + micros_t{static_cast<int64_t>(keepalive_interval_s) * 1000000LL});
    const bool bypass_deadband = keepalive_fired || keepalive_return || keepalive_refresh_due;
    if (!bypass_deadband
        && last_write_ok != -1_us
        && std::abs(target_w - last_written_w) < deadband_w) {
        finish_cycle(current_running_mode());
        return;
    }

    send_setpoint(target_w);
}

int32_t SbseController::pick_keepalive_pulse()
{
    // Choose the next keep-alive pulse: alternating sign so the long-run
    // energy contribution averages to zero, respecting the saturation caps
    // and the SoC edges. If the preferred direction is unavailable, try the
    // other; if neither is available (both caps zero, or SoC pinned with
    // only the other direction blocked too), return 0 and the keep-alive
    // simply doesn't fire this cycle -- we'll retry on the next one.
    const int32_t mag = keepalive_pulse_w;
    if (mag <= 0) {
        return 0;
    }

    const int32_t pulse_d = std::min(mag, max_discharge_w);
    const int32_t pulse_c = std::min(mag, max_charge_w);
    const bool can_discharge = pulse_d > 0 && (soc_pct == 255 || soc_pct > 0);
    const bool can_charge    = pulse_c > 0 && (soc_pct == 255 || soc_pct < 100);

    if (!can_discharge && !can_charge) {
        return 0;
    }

    bool charge = keepalive_next_charge;
    if ( charge && !can_charge)    charge = false;
    if (!charge && !can_discharge) charge = true;

    keepalive_next_charge = !charge;
    return charge ? -pulse_c : pulse_d;
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
    //
    // Uses buf_zero, not buf_setpoint, so a fire-and-forget call from outside
    // the cycle pipeline can't trample on an in-flight cycle's payload.
    if (connected_client == nullptr) {
        return;
    }

    write_int32be(buf_zero + 0, 0);
    write_int32be(buf_zero + 2, SBSE_COMPANION_VALUE);

    auto *client = static_cast<TFModbusTCPSharedClient *>(connected_client);

    client->transact(INVERTER_UNIT_ID,
                     TFModbusTCPFunctionCode::WriteMultipleRegisters,
                     POWER_SETPOINT_ADDR,
                     POWER_SETPOINT_REG_COUNT,
                     buf_zero,
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
    // Same rationale as send_zero_w() for using buf_zero: this fires from
    // inside a cycle's failure callback, the regular send_setpoint flow won't
    // overlap, but isolating the buffer keeps the invariant uniform.
    write_int32be(buf_zero + 0, 0);
    write_int32be(buf_zero + 2, SBSE_COMPANION_VALUE);

    auto *client = static_cast<TFModbusTCPSharedClient *>(connected_client);

    client->transact(INVERTER_UNIT_ID,
                     TFModbusTCPFunctionCode::WriteMultipleRegisters,
                     POWER_SETPOINT_ADDR,
                     POWER_SETPOINT_REG_COUNT,
                     buf_zero,
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
