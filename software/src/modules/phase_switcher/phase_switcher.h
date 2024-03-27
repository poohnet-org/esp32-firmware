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

#pragma once

#include "config.h"
#include "module.h"

#include "bindings/bricklet_phase_switcher.h"

#include "device_module.h"
#include "phase_switcher_bricklet_firmware_bin.embedded.h"

#include "../evse/control_pilot_backend.h"

#if defined(__GNUC__)
  #pragma GCC diagnostic push
  //#include "gcc_warnings.h"
  #pragma GCC diagnostic ignored "-Weffc++"
#endif

enum class PhaseSwitchingState
{
  Monitoring = 0,
  Stopping,
  DisconnectingCP,
  TogglingContactor,
  ConnectingCP
};

class PhaseSwitcher final : public DeviceModule<TF_PhaseSwitcher,
                                phase_switcher_bricklet_firmware_bin_data,
                                phase_switcher_bricklet_firmware_bin_length,
                                tf_phase_switcher_create,
                                tf_phase_switcher_get_bootloader_mode,
                                tf_phase_switcher_reset,
                                tf_phase_switcher_destroy,
                                false>, ControlPilotBackend
{
public:
  PhaseSwitcher();

  void pre_setup() override;
  void setup() override;
  void register_urls() override;
  void loop() override;

  void setup_phase_switcher();

  // ControlPilotBackend implementation
  bool get_control_pilot_disconnect() const override;
  void set_control_pilot_disconnect(bool cp_disconnect, bool* cp_disconnected) override;

  uint8_t get_phases_requested() const;
  void set_phases_requested(uint8_t phases_requested);

  uint8_t get_phases_current() const;
  void set_phases_current(uint8_t phases_current);

  uint8_t get_phases_active() const;
  uint8_t get_phases_state() const;

private:
  void update_all_data();
  void do_the_stuff();

  ConfigRoot state;
  PhaseSwitchingState switching_state = PhaseSwitchingState::Monitoring;
};

#if defined(__GNUC__)
  #pragma GCC diagnostic pop
#endif
