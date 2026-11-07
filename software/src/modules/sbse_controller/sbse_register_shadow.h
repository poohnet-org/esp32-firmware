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

// Shadow of an emulated inverter's writable settings registers. Real
// inverters persist these; clients commonly write and read back, so writes
// are stored and served on reads. Unwritten registers read the protocol's
// default (DefaultFn). Small fixed table, no heap. Shared by the GoodWe and
// Solis adapters.
template <size_t N>
class SbseRegisterShadow final
{
public:
    using DefaultFn = uint16_t (*)(uint16_t addr);

    static constexpr size_t CAPACITY = N;

    explicit SbseRegisterShadow(DefaultFn default_fn_) : default_fn(default_fn_) {}

    void reset() { count = 0; }

    uint16_t get(uint16_t addr) const
    {
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].addr == addr) {
                return entries[i].value;
            }
        }
        return default_fn(addr);
    }

    // Has a client written this register?
    bool contains(uint16_t addr) const
    {
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].addr == addr) {
                return true;
            }
        }
        return false;
    }

    // Forget a client write; the register reads its default again.
    void erase(uint16_t addr)
    {
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].addr == addr) {
                entries[i] = entries[--count];
                return;
            }
        }
    }

    // false = table full (the write is dropped)
    bool set(uint16_t addr, uint16_t value)
    {
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].addr == addr) {
                entries[i].value = value;
                return true;
            }
        }
        if (count >= N) {
            return false;
        }
        entries[count++] = {addr, value};
        return true;
    }

private:
    struct Entry {
        uint16_t addr;
        uint16_t value;
    };

    DefaultFn default_fn;
    Entry     entries[N] = {};
    size_t    count      = 0;
};
