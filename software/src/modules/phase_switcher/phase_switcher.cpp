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

#include "api.h"
#include "event_log.h"
#include "task_scheduler.h"
#include "phase_switcher.h"

#include "bindings/errors.h"

#include "module_dependencies.h"

static const char* toString(bool cp_disconnect)
{
  return cp_disconnect ? "disconnected" : "connected";
}

PhaseSwitcher::PhaseSwitcher()
  : DeviceModule("phase_switcher", "Phase Switcher", "Phase Switcher", [this](){this->setup_phase_switcher();})
{
}

void PhaseSwitcher::pre_setup()
{
  this->DeviceModule::pre_setup();
  this->ControlPilotBackend::pre_setup();

  state = Config::Object({
    {"cp_disconnect", Config::Bool(false)},
    {"phases_requested", Config::Uint8(1)},
    {"phases_current", Config::Uint8(1)},
    {"phases_active", Config::Uint8(0)}
  });
}

void PhaseSwitcher::setup()
{
  setup_phase_switcher();

  if (!device_found) {
    return;
  }

  initialized = true;
  evse.register_cp_backend(this);
  api.addFeature("cp_disconnect");
  api.addFeature("phase_switcher");

  evse_common.set_phase_switcher_enabled(true);

  task_scheduler.scheduleUncancelable([this]() {
    update_all_data();
  }, 0_ms, 250_ms);

  task_scheduler.scheduleUncancelable([this]() {
    do_the_stuff();
  }, 1000_ms, 1000_ms);
}

void PhaseSwitcher::register_urls()
{
  api.addState("phase_switcher/state", &state);

  this->DeviceModule::register_urls();
  this->ControlPilotBackend::register_urls();
}

void PhaseSwitcher::loop()
{
  this->DeviceModule::loop();
}

void PhaseSwitcher::setup_phase_switcher()
{
  if (!this->DeviceModule::setup_device()) {
    return;
  }

  tf_phase_switcher_set_status_led_config(&device, TF_PHASE_SWITCHER_STATUS_LED_CONFIG_SHOW_HEARTBEAT);

  int rc = tf_phase_switcher_set_control_pilot_disconnect(&device, get_control_pilot_disconnect());

  if (rc != TF_E_OK) {
    if (!is_in_bootloader(rc)) {
      logger.printfln("Phase Switcher (Setup): tf_phase_switcher_set_control_pilot_disconnect() failed with rc %d.", rc);
    }

    return;
  }

  rc = tf_phase_switcher_set_phases_wanted(&device, get_phases_current());

  if (rc != TF_E_OK) {
    if (!is_in_bootloader(rc)) {
      logger.printfln("Phase Switcher (Setup): tf_phase_switcher_set_phases_wanted() failed with rc %d.", rc);
    }

    return;
  }
}

bool PhaseSwitcher::get_control_pilot_disconnect() const
{
  return state.get("cp_disconnect")->asBool();
}

void PhaseSwitcher::set_control_pilot_disconnect(bool cp_disconnect, bool* cp_disconnected)
{
  if (!initialized) {
    return;
  }

  bool old_cp_disconnect = get_control_pilot_disconnect();

  if (cp_disconnect != old_cp_disconnect) {
    int rc = tf_phase_switcher_set_control_pilot_disconnect(&device, cp_disconnect);

    if (rc != TF_E_OK) {
      if (!is_in_bootloader(rc)) {
        logger.printfln("Phase Switcher: tf_phase_switcher_set_control_pilot_disconnect() failed with rc %d.", rc);
      }

      return;
    }

    logger.printfln("Phase Switcher: Control Pilot changed from %s to %s.", toString(old_cp_disconnect), toString(cp_disconnect));
  }

  if (cp_disconnected) {
    *cp_disconnected = cp_disconnect;
  }
}

uint8_t PhaseSwitcher::get_phases_requested() const
{
  return static_cast<uint8_t>(state.get("phases_requested")->asUint());
}

void PhaseSwitcher::set_phases_requested(uint8_t phases_requested)
{
  state.get("phases_requested")->updateUint(phases_requested);
}

uint8_t PhaseSwitcher::get_phases_current() const
{
  return static_cast<uint8_t>(state.get("phases_current")->asUint());
}

void PhaseSwitcher::set_phases_current(uint8_t phases_current)
{
  if (!initialized) {
    return;
  }

  uint8_t old_phases_current = get_phases_current();

  if ((get_phases_active() == 0) && get_control_pilot_disconnect() && (phases_current != old_phases_current)) {
    int rc = tf_phase_switcher_set_phases_wanted(&device, phases_current);

    if (rc != TF_E_OK) {
      if (!is_in_bootloader(rc)) {
        logger.printfln("Phase Switcher: tf_phase_switcher_set_phases_wanted() failed with rc %d.", rc);
      }

      return;
    }

    logger.printfln("Phase Switcher: Phases changed from %u to %u.", old_phases_current, phases_current);
  }
}

uint8_t PhaseSwitcher::get_phases_active() const
{
  return static_cast<uint8_t>(state.get("phases_active")->asUint());
}

uint8_t PhaseSwitcher::get_phases_state() const
{
  return static_cast<uint8_t>(switching_state);
}

void PhaseSwitcher::update_all_data()
{
  if (!initialized) {
    return;
  }

  bool cp_disconnect;
  uint8_t phases_current, phases_active;
  int rc = tf_phase_switcher_get_all_data(&device, &cp_disconnect, &phases_current, &phases_active);

  if (rc != TF_E_OK) {
    if (!is_in_bootloader(rc)) {
      logger.printfln("Phase Switcher: tf_phase_switcher_get_all_data() failed with rc %d.", rc);
    }

    return;
  }

  state.get("cp_disconnect")->updateBool(cp_disconnect);
  state.get("phases_current")->updateUint(phases_current);
  state.get("phases_active")->updateUint(phases_active);

  control_pilot_disconnect.get("disconnect")->updateBool(cp_disconnect);
}

void PhaseSwitcher::do_the_stuff()
{
  static PhaseSwitchingState prev_state = switching_state;

  if (!initialized) {
    return;
  }

  if (switching_state != prev_state) {
    logger.printfln("Phase Switcher: Now in state %i.", static_cast<int>(switching_state));
    prev_state = switching_state;
  }

  uint8_t phases_requested = get_phases_requested();

  switch (switching_state) {
    case PhaseSwitchingState::Monitoring:
      evse_common.set_phase_switcher_blocking(false);

      if (phases_requested != get_phases_current()) {
        switching_state = PhaseSwitchingState::Stopping;
      }
      break;

    case PhaseSwitchingState::Stopping:
      evse_common.set_phase_switcher_blocking(true);

      if (get_phases_active() == 0) {
        switching_state = PhaseSwitchingState::DisconnectingCP;
      }
      break;

    case PhaseSwitchingState::DisconnectingCP:
      evse_common.set_control_pilot_disconnect(true, nullptr);

      if (evse_common.get_control_pilot_disconnect()) {
        switching_state = PhaseSwitchingState::TogglingContactor;
      }
      break;

    case PhaseSwitchingState::TogglingContactor:
      set_phases_current(phases_requested);

      if (get_phases_current() == phases_requested) {
        switching_state = PhaseSwitchingState::ConnectingCP;
      }
      break;

    case PhaseSwitchingState::ConnectingCP:
      evse_common.set_control_pilot_disconnect(false, nullptr);

      if (!evse_common.get_control_pilot_disconnect()) {
        switching_state = PhaseSwitchingState::Monitoring;
      }
      break;

    default:
      break;
  }
}
