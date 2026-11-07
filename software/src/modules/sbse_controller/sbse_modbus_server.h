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

// Which inverter the Modbus TCP server emulates. Exactly one protocol is
// active at a time (config.modbus_server_protocol); the numeric values are
// the persisted config values.
enum class SbseModbusProtocol : uint8_t {
    Sma    = 0,   // SMA hybrid inverter (CmpBMS.OpMod + setpoint block)
    Goodwe = 1,   // GoodWe ET hybrid inverter (EMS mode + power, sbse_goodwe_map.h)
    Solis  = 2,   // Solis S5/S6 hybrid inverter (remote control, sbse_solis_map.h)
};

[[gnu::const]] const char *sbse_modbus_protocol_name(SbseModbusProtocol protocol);

// Inverter-emulating Modbus TCP server. Thin protocol adapter: in SMA mode
// it accepts a small set of SMA register block requests and forwards them
// to handlers provided by the controller. All semantics (sticky OpMod, force-mode, watchdog,
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
//
// GoodWe and Solis ("register map" protocols): every FC 3 / FC 4 read goes
// to the map read handler and every write (FC 16, and FC 6 -- the library
// delivers FC 6 as a one-register WriteMultipleRegisters) to the map write
// handler; the controller dispatches on the protocol. Address validation
// lives in the maps (sbse_goodwe_map.h, sbse_solis_map.h). Each distinct
// request shape
// (unit, function code, start, count, result) is logged once per listener
// start, so the event log shows what a new client (e.g. the Tibber Bridge)
// actually reads and writes without flooding on every poll.
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

    using WriteHandler    = std::function<TFModbusTCPExceptionCode(uint16_t start_address,
                                                                   uint16_t reg_count,
                                                                   const uint16_t *regs)>;

    SbseModbusServer();
    SbseModbusServer(const SbseModbusServer &) = delete;
    SbseModbusServer &operator=(const SbseModbusServer &) = delete;

    // Wire up the dispatch callbacks. Must be called before start().
    void set_handlers(OpModHandler on_op_mod,
                      SetpointHandler on_setpoint,
                      ReadHandler on_read);

    // Register-map protocol (GoodWe, Solis) callbacks. Must be called
    // before start().
    void set_map_handlers(ReadHandler on_read, WriteHandler on_write);

    // Snapshot the controller's persistent config into the server. unit_id 0
    // means "accept any unit id" (broadcast-style). Doesn't itself bind --
    // call start() / restart() afterwards.
    void configure(bool enabled, uint16_t port, uint8_t unit_id, SbseModbusProtocol protocol);

    // Has the (enabled, port, protocol) tuple changed since the most recent
    // start()? Used by the controller to decide whether to bounce the
    // listener after a /config PUT. A protocol switch restarts the listener
    // so connected clients of the old protocol are dropped.
    bool needs_restart_for(bool new_enabled, uint16_t new_port, SbseModbusProtocol new_protocol) const;

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
    TFModbusTCPExceptionCode dispatch_map(TFModbusTCPFunctionCode function_code,
                                             uint16_t start_address,
                                             uint16_t data_count,
                                             void *data);
    void log_request_once(uint8_t req_unit_id, TFModbusTCPFunctionCode function_code,
                          uint16_t start_address, uint16_t data_count,
                          TFModbusTCPExceptionCode result);

    TFModbusTCPServer server{TFModbusTCPByteOrder::Host};
    bool     running         = false;
    uint64_t tick_task_id    = 0;

    // Snapshot from the most recent configure(). The fields used at runtime
    // (e.g. unit_id matching) are read here on every request, so the
    // controller doesn't have to restart the server just to change them.
    bool     enabled         = false;
    uint16_t port            = 502;
    uint8_t  unit_id         = 3;
    SbseModbusProtocol protocol = SbseModbusProtocol::Sma;

    // Snapshot of (enabled, port, protocol) at the moment of the most recent
    // start(); needs_restart_for() compares against this to skip no-op
    // restarts.
    bool     last_started_enabled  = false;
    uint16_t last_started_port     = 0;
    SbseModbusProtocol last_started_protocol = SbseModbusProtocol::Sma;

    OpModHandler    on_op_mod;
    SetpointHandler on_setpoint;
    ReadHandler     on_read;

    ReadHandler     on_map_read;
    WriteHandler    on_map_write;

    // Request shapes already logged since the last start() (map protocols).
    struct SeenRequest {
        uint16_t start_address;
        uint16_t data_count;
        uint8_t  unit_id;
        uint8_t  function_code;
        uint8_t  result;
    };
    static constexpr size_t SEEN_REQUESTS_MAX = 48;
    SeenRequest seen_requests[SEEN_REQUESTS_MAX] = {};
    size_t      seen_request_count = 0;
    bool        seen_requests_overflow_logged = false;
};
