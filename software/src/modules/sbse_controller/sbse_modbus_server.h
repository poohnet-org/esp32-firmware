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
#include <functional>

#include <TFModbusTCPServer.h>
#include "TFModbusTCPCommon.h"

// SMA-compatible Modbus TCP server. Thin protocol adapter: accepts a small
// set of SMA register block requests and forwards them to handlers provided
// by the controller. All semantics (sticky OpMod, force-mode, watchdog,
// active_config wiring, register cache) live in the controller; this class
// only owns the network listener and the dispatch.
//
// Accepted requests:
//   FC 16 (WriteMultipleRegisters):
//     40236, 2 reg        -> on_op_mod(uint32_t)
//     40793..40802, any
//       even sub-block    -> on_setpoint(start_addr, reg_count, regs)
//   FC 3  (ReadHoldingRegisters):
//     any                 -> on_read(fc, start_addr, reg_count, out)
//   FC 4  (ReadInputRegisters):
//     any                 -> on_read(fc, start_addr, reg_count, out)
//
// The setpoint handler accepts partial writes because evcc's sma-hybrid
// template (and SMA's own clients) may write individual sub-fields rather
// than the whole 5x uint32 block; we hand the relevant slice to the
// controller and let it decide which fields to apply. The read handler
// returns Success with the response buffer filled (or NaN sentinels for
// addresses the proxy cache doesn't cover).
//
// Anything else returns IllegalFunction or IllegalDataAddress.
class SbseModbusServer final
{
public:
    // Handlers return the Modbus exception code that should be sent on the
    // wire. Returning Success acknowledges the request.
    using OpModHandler    = std::function<TFModbusTCPExceptionCode(uint32_t op_mod)>;
    using SetpointHandler = std::function<TFModbusTCPExceptionCode(uint16_t start_address,
                                                                   uint16_t reg_count,
                                                                   const uint16_t *regs)>;
    using ReadHandler     = std::function<TFModbusTCPExceptionCode(TFModbusTCPFunctionCode fc,
                                                                   uint16_t start_address,
                                                                   uint16_t reg_count,
                                                                   uint16_t *out_regs)>;

    SbseModbusServer();
    SbseModbusServer(const SbseModbusServer &) = delete;
    SbseModbusServer &operator=(const SbseModbusServer &) = delete;

    // Wire up the dispatch callbacks. Must be called before start().
    void set_handlers(OpModHandler on_op_mod,
                      SetpointHandler on_setpoint,
                      ReadHandler on_read);

    // Snapshot the controller's persistent config into the server. unit_id 0
    // means "accept any unit id" (broadcast-style). Doesn't itself bind --
    // call start() / restart() afterwards.
    void configure(bool enabled, uint16_t port, uint8_t unit_id);

    // Has the (enabled, port) tuple changed since the most recent start()?
    // Used by the controller to decide whether to bounce the listener after
    // a /config PUT.
    bool needs_restart_for(bool new_enabled, uint16_t new_port) const;

    void start();
    void stop();
    void restart();           // stop + start if currently enabled
    bool is_running() const { return running; }

private:
    TFModbusTCPExceptionCode dispatch(uint8_t unit_id,
                                      TFModbusTCPFunctionCode function_code,
                                      uint16_t start_address,
                                      uint16_t data_count,
                                      void *data);

    TFModbusTCPServer server{TFModbusTCPByteOrder::Host};
    bool     running         = false;
    uint64_t tick_task_id    = 0;

    // Snapshot from the most recent configure(). The fields used at runtime
    // (e.g. unit_id matching) are read here on every request, so the
    // controller doesn't have to restart the server just to change them.
    bool     enabled         = false;
    uint16_t port            = 502;
    uint8_t  unit_id         = 3;

    // Snapshot of (enabled, port) at the moment of the most recent start();
    // needs_restart_for() compares against this to skip no-op restarts.
    bool     last_started_enabled = false;
    uint16_t last_started_port    = 0;

    OpModHandler    on_op_mod;
    SetpointHandler on_setpoint;
    ReadHandler     on_read;
};
