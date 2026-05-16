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

#include <stddef.h>
#include <stdint.h>
#include <TFTools/Micros.h>

class StringBuilder;

// 5-minute live trace ring buffer at 1 Hz, served by GET /<path> so a freshly
// loaded dashboard can seed its chart instead of starting from a blank canvas.
//
// Wire format of the JSON response:
//   {"samples": [[age_ms, grid, battery, setpoint, target], ...]}
//
// age_ms is computed at response time, so the browser can rebase against its
// own clock without depending on NTP sync between device and browser.
class SbseTraceHistory final
{
public:
    static constexpr size_t   CAPACITY        = 300;   // 5 min * 60 s
    static constexpr micros_t SAMPLE_INTERVAL = 1_s;

    SbseTraceHistory() = default;
    SbseTraceHistory(const SbseTraceHistory &) = delete;
    SbseTraceHistory &operator=(const SbseTraceHistory &) = delete;

    // Capture a sample. Called from the controller every tick; the class
    // throttles internally to one sample per SAMPLE_INTERVAL, so a faster
    // tick rate just drops the in-between samples.
    void add_sample(int32_t grid_w, int32_t battery_w, int32_t setpoint_w, int32_t target_w);

    // Register an HTTP GET handler at the given absolute path. Must be called
    // during register_urls(); the handler captures `this`, so the instance
    // must outlive the request handler (which it does -- module lifetime).
    void register_url(const char *path);

private:
    struct Sample {
        micros_t captured_us;
        int16_t  grid_w;
        int16_t  battery_w;
        int16_t  setpoint_w;
        int16_t  target_w;
    };

    void format(micros_t now, StringBuilder *sb) const;

    Sample   samples[CAPACITY] = {};
    size_t   count   = 0;          // valid entries, <= CAPACITY
    size_t   head    = 0;          // next write slot
    micros_t last_us = -1_us;      // last capture timestamp
};
