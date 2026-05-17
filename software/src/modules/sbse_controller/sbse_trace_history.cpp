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

#define EVENT_LOG_PREFIX "sbse_hist"

#include "sbse_trace_history.h"

#include "event_log_prefix.h"
#include "generated/module_dependencies.h"
#include "tools/string_builder.h"

#include "gcc_warnings.h"

static int16_t sat16(int32_t v)
{
    if (v > INT16_MAX) return INT16_MAX;
    if (v < INT16_MIN) return INT16_MIN;
    return static_cast<int16_t>(v);
}

void SbseTraceHistory::add_sample(int32_t grid_w, int32_t battery_w, int32_t setpoint_w,
                                  int32_t target_lo_w, int32_t target_hi_w)
{
    const micros_t now = now_us();
    if (last_us != -1_us && (now - last_us) < SAMPLE_INTERVAL) {
        return;
    }
    last_us = now;

    Sample &s = samples[head];
    s.captured_us = now;
    s.grid_w      = sat16(grid_w);
    s.battery_w   = sat16(battery_w);
    s.setpoint_w  = sat16(setpoint_w);
    s.target_lo_w = sat16(target_lo_w);
    s.target_hi_w = sat16(target_hi_w);

    head = (head + 1) % CAPACITY;
    if (count < CAPACITY) {
        ++count;
    }
}

void SbseTraceHistory::format(micros_t now, StringBuilder *sb) const
{
    sb->puts("{\"samples\":[");
    if (count > 0) {
        const size_t start = (head + CAPACITY - count) % CAPACITY;
        for (size_t i = 0; i < count; ++i) {
            const size_t idx = (start + i) % CAPACITY;
            const Sample &s = samples[idx];
            const uint32_t age_ms = (now - s.captured_us).to<millis_t>().as<uint32_t>();
            sb->printf("%s[%lu,%d,%d,%d,%d,%d]",
                       i == 0 ? "" : ",",
                       age_ms,
                       static_cast<int>(s.grid_w),
                       static_cast<int>(s.battery_w),
                       static_cast<int>(s.setpoint_w),
                       static_cast<int>(s.target_lo_w),
                       static_cast<int>(s.target_hi_w));
        }
    }
    sb->puts("]}");
}

void SbseTraceHistory::register_url(const char *path)
{
    server.on(path, HTTP_GET, [this](WebServerRequest request) {
        StringBuilder sb;
        // Worst-case per sample: "[9999999,-32768,-32768,-32768,-32768],"
        // = ~43 bytes. CAPACITY * 50 + 64 outer = ~15 kB.
        if (!sb.setCapacity(CAPACITY * 50 + 64)) {
            return request.send_plain(500, "history alloc failed");
        }
        this->format(now_us(), &sb);
        return request.send_json(200, sb);
    });
}
