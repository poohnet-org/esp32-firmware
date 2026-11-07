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
#include <string.h>

// Writes register values into the slice of a Modbus response buffer that
// overlaps [start, start+count). Fields outside the requested range are
// dropped, so an emulated register map can simply emit all fields of a
// window on every read. Multi-register values are big-endian, high word
// first (GoodWe and Solis alike).
class SbseRegisterWriter final
{
public:
    SbseRegisterWriter(uint16_t start_, uint16_t count_, uint16_t *out_) :
        start(start_), count(count_), out(out_) {}

    void u16(uint16_t addr, uint16_t value)
    {
        const uint32_t off = static_cast<uint32_t>(addr) - start;
        if (addr >= start && off < count) {
            out[off] = value;
        }
    }

    void s16(uint16_t addr, int32_t value)
    {
        if (value > INT16_MAX) value = INT16_MAX;
        if (value < INT16_MIN) value = INT16_MIN;
        u16(addr, static_cast<uint16_t>(static_cast<int16_t>(value)));
    }

    void u32(uint16_t addr, uint32_t value)
    {
        u16(addr,                              static_cast<uint16_t>(value >> 16));
        u16(static_cast<uint16_t>(addr + 1),   static_cast<uint16_t>(value & 0xFFFFu));
    }

    void s32(uint16_t addr, int32_t value) { u32(addr, static_cast<uint32_t>(value)); }

    void u64(uint16_t addr, uint64_t value)
    {
        u32(addr,                              static_cast<uint32_t>(value >> 32));
        u32(static_cast<uint16_t>(addr + 2),   static_cast<uint32_t>(value & 0xFFFFFFFFu));
    }

    void f32(uint16_t addr, float value)
    {
        uint32_t bits;
        memcpy(&bits, &value, sizeof(bits));
        u32(addr, bits);
    }

    // ASCII string, two characters per register (high byte first), padded
    // with `pad` up to `regs` registers.
    void str(uint16_t addr, uint16_t regs, const char *s, char pad)
    {
        const size_t len = s != nullptr ? strlen(s) : 0;
        for (uint16_t i = 0; i < regs; ++i) {
            const size_t a = 2u * i, b = a + 1;
            const uint8_t hi = static_cast<uint8_t>(a < len ? s[a] : pad);
            const uint8_t lo = static_cast<uint8_t>(b < len ? s[b] : pad);
            u16(static_cast<uint16_t>(addr + i), static_cast<uint16_t>((hi << 8) | lo));
        }
    }

private:
    uint16_t start;
    uint16_t count;
    uint16_t *out;
};
