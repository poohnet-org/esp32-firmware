/* esp32-firmware
 * Copyright (C) 2024 Thomas Hein <github@poohnet.de>
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

#include "config.h"

class ControlPilotBackend {
public:
    virtual ~ControlPilotBackend() = default;

    virtual bool get_control_pilot_disconnect() const = 0;
    virtual void set_control_pilot_disconnect(bool cp_disconnect, bool* cp_disconnected) = 0;

    virtual void pre_setup();
    virtual void register_urls();

protected:
    ConfigRoot control_pilot_disconnect;
    ConfigRoot control_pilot_disconnect_update;
};
