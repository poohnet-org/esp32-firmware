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

// SMA-compatible Modbus TCP server. Thin protocol adapter: accepts
// WriteMultipleRegisters at exactly two addresses and forwards the parsed
// payload to handlers provided by the controller. All semantics
// (sticky OpMod, force-mode, watchdog, active_config wiring) live in the
// controller; this class only owns the network listener and the dispatch.
//
// Accepted addresses (function code 16, WriteMultipleRegisters):
//   40236  2 reg   CmpBMS.OpMod                       -> on_op_mod(uint32_t)
//   40793  10 reg  CmpBMS.{BatChaMin/Max,BatDchg.,GridWSpt}  -> on_setpoint(regs)
//
// Anything else returns IllegalFunction or IllegalDataAddress.
class SbseModbusServer final
{
public:
    // Handlers return the Modbus exception code that should be sent on the
    // wire. Returning Success acknowledges the write.
    using OpModHandler    = std::function<TFModbusTCPExceptionCode(uint32_t op_mod)>;
    using SetpointHandler = std::function<TFModbusTCPExceptionCode(const uint16_t *regs10)>;

    SbseModbusServer();
    SbseModbusServer(const SbseModbusServer &) = delete;
    SbseModbusServer &operator=(const SbseModbusServer &) = delete;

    // Wire up the dispatch callbacks. Must be called before start().
    void set_handlers(OpModHandler on_op_mod, SetpointHandler on_setpoint);

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
                                      const uint16_t *regs);

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
};
