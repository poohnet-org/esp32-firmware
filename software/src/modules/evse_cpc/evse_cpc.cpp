/* esp32-firmware
 * Copyright (C) 2023-2024 Thomas Hein
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

#include "evse_cpc.h"
#include "evse_cpc_bricklet_firmware_bin.embedded.h"

#include "bindings/errors.h"
#include "event_log_prefix.h"
#include "module_dependencies.h"

static const char* toString(bool cp_disconnect)
{
  return cp_disconnect ? "disconnected" : "connected";
}

EvseCPC::EvseCPC()
  : DeviceModule(evse_cpc_bricklet_firmware_bin_data, evse_cpc_bricklet_firmware_bin_length, "evse_cpc", "EVSE CPC", "EVSE CPC", [this](){this->setup_evse_cpc();})
{
}

void EvseCPC::pre_setup()
{
  this->DeviceModule::pre_setup();
  this->ControlPilotBackend::pre_setup();

  state = Config::Object({
    {"channel0", Config::Bool(false)},
    {"channel1", Config::Bool(false)}
  });
}

void EvseCPC::setup()
{
  setup_evse_cpc();

  if (!device_found) {
    return;
  }

  initialized = true;
  evse.register_cp_backend(this);
  api.addFeature("cp_disconnect");

  task_scheduler.scheduleWithFixedDelay([this]() {
    update_all_data();
  }, 0_ms, 250_ms);
}

void EvseCPC::register_urls()
{
  api.addState("evse_cpc/state", &state);

  this->DeviceModule::register_urls();
  this->ControlPilotBackend::register_urls();
}

void EvseCPC::loop()
{
  this->DeviceModule::loop();
}

void EvseCPC::setup_evse_cpc()
{
  if (!this->DeviceModule::setup_device()) {
    return;
  }

  tf_evse_cpc_set_status_led_config(&device, TF_EVSE_CPC_STATUS_LED_CONFIG_SHOW_HEARTBEAT);
  tf_evse_cpc_set_channel_led_config(&device, 0, TF_EVSE_CPC_CHANNEL_LED_CONFIG_SHOW_CHANNEL_STATUS);
  tf_evse_cpc_set_channel_led_config(&device, 1, TF_EVSE_CPC_CHANNEL_LED_CONFIG_SHOW_CHANNEL_STATUS);

  int rc = tf_evse_cpc_set_value(&device, true, false);

  if (rc != TF_E_OK) {
    if (!is_in_bootloader(rc)) {
      logger.printfln("EVSE CPC (Setup): tf_evse_cpc_set_value() failed with rc %d.", rc);
    }

    return;
  }
}

bool EvseCPC::get_control_pilot_disconnect() const
{
  return !state.get("channel0")->asBool();
}

void EvseCPC::set_control_pilot_disconnect(bool cp_disconnect, bool* cp_disconnected)
{
  if (!initialized) {
    return;
  }

  bool old_cp_disconnect = get_control_pilot_disconnect();

  if (cp_disconnect != old_cp_disconnect) {
    int rc = tf_evse_cpc_set_value(&device, !cp_disconnect, cp_disconnect);

    if (rc != TF_E_OK) {
      if (!is_in_bootloader(rc)) {
        logger.printfln("EVSE CPC: tf_evse_cpc_set_value() failed with rc %d.", rc);
      }

      return;
    }

    logger.printfln("EVSE CPC: Control Pilot changed from %s to %s.", toString(old_cp_disconnect), toString(cp_disconnect));
  }

  if (cp_disconnected) {
    *cp_disconnected = cp_disconnect;
  }
}

void EvseCPC::update_all_data()
{
  if (!initialized) {
    return;
  }

  bool channel0, channel1;
  int rc = tf_evse_cpc_get_value(&device, &channel0, &channel1);

  if (rc != TF_E_OK) {
    if (!is_in_bootloader(rc)) {
      logger.printfln("EVSE CPC: tf_evse_cpc_get_value() failed with rc %d.", rc);
    }

    return;
  }

  state.get("channel0")->updateBool(channel0);
  state.get("channel1")->updateBool(channel1);

  control_pilot_disconnect.get("disconnect")->updateBool(!channel0);
}
