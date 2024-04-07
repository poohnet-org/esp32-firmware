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

#include "bindings/bricklet_evse_cpc.h"

#include "device_module.h"
#include "evse_cpc_bricklet_firmware_bin.embedded.h"

#if defined(__GNUC__)
  #pragma GCC diagnostic push
  //#include "gcc_warnings.h"
  #pragma GCC diagnostic ignored "-Weffc++"
#endif

class EvseCPC final : public DeviceModule<TF_EVSECPC,
                                evse_cpc_bricklet_firmware_bin_data,
                                evse_cpc_bricklet_firmware_bin_length,
                                tf_evse_cpc_create,
                                tf_evse_cpc_get_bootloader_mode,
                                tf_evse_cpc_reset,
                                tf_evse_cpc_destroy,
                                false>
{
public:
  EvseCPC();

  void pre_setup() override;
  void setup() override;
  void register_urls() override;
  void loop() override;

  void setup_evse_cpc();

  bool get_control_pilot_disconnect();
  void set_control_pilot_disconnect(bool cp_disconnect, bool* cp_disconnected);

private:
  void update_all_data();

  ConfigRoot state;
};

#if defined(__GNUC__)
  #pragma GCC diagnostic pop
#endif
