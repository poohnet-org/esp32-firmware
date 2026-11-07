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

#define EVENT_LOG_PREFIX "sbse_mb"

#include "sbse_modbus_server.h"

#include "event_log_prefix.h"
#include "generated/module_dependencies.h"
#include "tools/net.h"

#include "gcc_warnings.h"

// SMA register block addresses + lengths we accept.
static constexpr uint16_t SRV_OPMOD_ADDR        = 40236;
static constexpr uint16_t SRV_OPMOD_REG_COUNT   = 2;
// Setpoint block: 5 x uint32 = 10 registers, starting at 40793. We accept
// any 2/4/6/8/10-register sub-block within this range as long as it's
// even-aligned to a uint32 boundary -- the controller's setpoint handler
// picks only the fields that were actually written.
static constexpr uint16_t SRV_SETP_ADDR_LO      = 40793;
static constexpr uint16_t SRV_SETP_REG_COUNT_MAX = 10;

// Server tick cadence. Lower = snappier I/O, higher = less scheduler churn.
// 20 ms is plenty for a device that sees <1 write/s in normal use.
static constexpr millis_t SRV_TICK            = 20_ms;

const char *sbse_modbus_protocol_name(SbseModbusProtocol protocol)
{
    switch (protocol) {
        case SbseModbusProtocol::Sma:    return "sma";
        case SbseModbusProtocol::Goodwe: return "goodwe";
        case SbseModbusProtocol::Solis:  return "solis";
        default:                         return "unknown";
    }
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Weffc++"
#endif

SbseModbusServer::SbseModbusServer() = default;

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

void SbseModbusServer::set_handlers(OpModHandler on_op_mod_,
                                    SetpointHandler on_setpoint_,
                                    ReadHandler on_read_)
{
    on_op_mod   = std::move(on_op_mod_);
    on_setpoint = std::move(on_setpoint_);
    on_read     = std::move(on_read_);
}

void SbseModbusServer::set_map_handlers(ReadHandler on_read_, WriteHandler on_write_)
{
    on_map_read  = std::move(on_read_);
    on_map_write = std::move(on_write_);
}

void SbseModbusServer::configure(bool enabled_, uint16_t port_, uint8_t unit_id_,
                                 SbseModbusProtocol protocol_)
{
    enabled  = enabled_;
    port     = port_;
    unit_id  = unit_id_;
    protocol = protocol_;
}

bool SbseModbusServer::needs_restart_for(bool new_enabled, uint16_t new_port,
                                         SbseModbusProtocol new_protocol) const
{
    return new_enabled  != last_started_enabled
        || new_port     != last_started_port
        || new_protocol != last_started_protocol;
}

void SbseModbusServer::start()
{
    if (running || !enabled) {
        return;
    }

    // Bind to the IPv6 any-address (::); the library clears IPV6_V6ONLY, so
    // the single dual-stack socket accepts IPv4 and IPv6 clients (same as
    // the upstream modbus_tcp module).
    ip_addr_t bind_addr = IPADDR6_INIT(0, 0, 0, 0);
    const bool ok = server.start(
        &bind_addr, port,
        [](uint64_t connection_id, const ip_addr_t *peer_address, uint16_t peer_port) {
            char peer_str[INET6_ADDRSTRLEN];
            tf_ipaddr_ntoa(peer_address, peer_str, sizeof(peer_str));
            logger.printfln("client %llu %s:%u connected",
                            static_cast<unsigned long long>(connection_id), peer_str, peer_port);
        },
        [](uint64_t connection_id, const ip_addr_t *peer_address, uint16_t peer_port,
           TFModbusTCPServerDisconnectReason reason, int error_number) {
            char peer_str[INET6_ADDRSTRLEN];
            tf_ipaddr_ntoa(peer_address, peer_str, sizeof(peer_str));
            logger.printfln("client %llu %s:%u disconnected: %s (error %d)",
                            static_cast<unsigned long long>(connection_id), peer_str, peer_port,
                            get_tf_modbus_tcp_server_client_disconnect_reason_name(reason),
                            error_number);
        },
        [this](uint64_t /*connection_id*/, uint8_t req_unit_id, TFModbusTCPFunctionCode fc,
               uint16_t start_address, uint16_t data_count, void *data) {
            return this->dispatch(req_unit_id, fc, start_address, data_count, data);
        });

    if (!ok) {
        logger.printfln("failed to start on port %u", port);
        return;
    }

    running               = true;
    seen_request_count    = 0;
    seen_requests_overflow_logged = false;
    last_started_enabled  = enabled;
    last_started_port     = port;
    last_started_protocol = protocol;

    tick_task_id = task_scheduler.scheduleWithFixedDelay([this]() {
        server.tick();
    }, SRV_TICK, SRV_TICK);

    logger.printfln("listening on port %u, unit_id=%u, protocol=%s",
                    port, static_cast<unsigned>(unit_id),
                    sbse_modbus_protocol_name(protocol));
}

void SbseModbusServer::stop()
{
    if (!running) {
        return;
    }
    if (tick_task_id != 0) {
        task_scheduler.cancel(tick_task_id);
        tick_task_id = 0;
    }
    server.stop();
    running = false;
    logger.printfln("stopped");
}

void SbseModbusServer::restart()
{
    stop();
    if (enabled) {
        start();
    }
}

TFModbusTCPExceptionCode SbseModbusServer::dispatch(
    uint8_t req_unit_id,
    TFModbusTCPFunctionCode function_code,
    uint16_t start_address,
    uint16_t data_count,
    void *data)
{
    if (protocol != SbseModbusProtocol::Sma) {
        // Log rejected unit ids too: a client configured for another unit
        // id would otherwise be invisible.
        const TFModbusTCPExceptionCode rc = (unit_id != 0 && req_unit_id != unit_id)
            ? TFModbusTCPExceptionCode::IllegalDataAddress
            : dispatch_map(function_code, start_address, data_count, data);
        log_request_once(req_unit_id, function_code, start_address, data_count, rc);
        return rc;
    }

    // Unit ID filter: 0 in our config means "accept any unit id".
    if (unit_id != 0 && req_unit_id != unit_id) {
        return TFModbusTCPExceptionCode::IllegalDataAddress;
    }

    // Reads -- delegate to the controller's proxy cache for any address.
    // The library has already validated the count against its protocol
    // limits and zero-filled the response buffer.
    if (function_code == TFModbusTCPFunctionCode::ReadHoldingRegisters
        || function_code == TFModbusTCPFunctionCode::ReadInputRegisters) {
        if (!on_read) {
            return TFModbusTCPExceptionCode::IllegalFunction;
        }
        return on_read(function_code, start_address, data_count,
                       static_cast<uint16_t *>(data));
    }

    if (function_code != TFModbusTCPFunctionCode::WriteMultipleRegisters) {
        return TFModbusTCPExceptionCode::IllegalFunction;
    }

    const uint16_t *regs = static_cast<const uint16_t *>(data);

    if (start_address == SRV_OPMOD_ADDR && data_count == SRV_OPMOD_REG_COUNT) {
        const uint32_t op_mod = (static_cast<uint32_t>(regs[0]) << 16) | regs[1];
        return on_op_mod ? on_op_mod(op_mod) : TFModbusTCPExceptionCode::Success;
    }

    // Setpoint block. Accept any even-aligned 2/4/6/8/10-register sub-block
    // that lies entirely within [40793, 40802]; the controller handler
    // decodes which uint32 fields are present.
    {
        const uint16_t setp_offset = start_address - SRV_SETP_ADDR_LO;
        const bool aligned    = (start_address >= SRV_SETP_ADDR_LO)
                              && (setp_offset % 2 == 0);
        const bool in_range   = aligned
                              && (data_count > 0)
                              && (data_count % 2 == 0)
                              && (setp_offset + data_count <= SRV_SETP_REG_COUNT_MAX);
        if (in_range) {
            return on_setpoint ? on_setpoint(start_address, data_count, regs)
                               : TFModbusTCPExceptionCode::Success;
        }
    }

    return TFModbusTCPExceptionCode::IllegalDataAddress;
}

TFModbusTCPExceptionCode SbseModbusServer::dispatch_map(
    TFModbusTCPFunctionCode function_code,
    uint16_t start_address,
    uint16_t data_count,
    void *data)
{
    if (function_code == TFModbusTCPFunctionCode::ReadHoldingRegisters
        || function_code == TFModbusTCPFunctionCode::ReadInputRegisters) {
        if (!on_map_read) {
            return TFModbusTCPExceptionCode::IllegalFunction;
        }
        return on_map_read(function_code, start_address, data_count,
                              static_cast<uint16_t *>(data));
    }
    if (function_code == TFModbusTCPFunctionCode::WriteMultipleRegisters) {
        if (!on_map_write) {
            return TFModbusTCPExceptionCode::IllegalFunction;
        }
        return on_map_write(start_address, data_count,
                               static_cast<const uint16_t *>(data));
    }
    return TFModbusTCPExceptionCode::IllegalFunction;
}

void SbseModbusServer::log_request_once(uint8_t req_unit_id,
                                        TFModbusTCPFunctionCode function_code,
                                        uint16_t start_address, uint16_t data_count,
                                        TFModbusTCPExceptionCode result)
{
    const uint8_t fc = static_cast<uint8_t>(function_code);
    const uint8_t rc = static_cast<uint8_t>(result);
    for (size_t i = 0; i < seen_request_count; ++i) {
        const SeenRequest &r = seen_requests[i];
        if (r.start_address == start_address && r.data_count == data_count
            && r.unit_id == req_unit_id && r.function_code == fc && r.result == rc) {
            return;
        }
    }
    if (seen_request_count >= SEEN_REQUESTS_MAX) {
        if (!seen_requests_overflow_logged) {
            seen_requests_overflow_logged = true;
            logger.printfln("%s: more than %u distinct requests, not logging new ones",
                            sbse_modbus_protocol_name(protocol),
                            static_cast<unsigned>(SEEN_REQUESTS_MAX));
        }
        return;
    }
    seen_requests[seen_request_count++] = {start_address, data_count, req_unit_id, fc, rc};
    logger.printfln("%s: unit %u %s %u+%u -> %s",
                    sbse_modbus_protocol_name(protocol),
                    static_cast<unsigned>(req_unit_id),
                    get_tf_modbus_tcp_function_code_name(function_code),
                    start_address, data_count,
                    get_tf_modbus_tcp_exception_code_name(result));
}
