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

#include "control_pilot_backend.h"

#include "event_log_prefix.h"
#include "generated/module_dependencies.h"

void ControlPilotBackend::pre_setup()
{
    control_pilot_disconnect = Config::Object({
        {"disconnect", Config::Bool(false)}
    });

    control_pilot_disconnect_update = control_pilot_disconnect;
}

void ControlPilotBackend::register_urls()
{
    api.addState("evse/control_pilot_disconnect", &control_pilot_disconnect);
    api.addCommand("evse/control_pilot_disconnect_update", &control_pilot_disconnect_update, {}, [this](Language /*language*/, String & /*errmsg*/) {
        if (evse_common.management_enabled.get("enabled")->asBool()) { // Disallow updating control pilot configuration if management is enabled because the charge manager will override the CP config every second.
            logger.printfln("Control pilot cannot be (dis)connected by API while charge management is enabled.");
            return;
        }
        this->set_control_pilot_disconnect(control_pilot_disconnect_update.get("disconnect")->asBool(), nullptr);
    }, true);
}
