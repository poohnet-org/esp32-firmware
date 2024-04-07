/* ***********************************************************
 * This file was automatically generated on 2024-03-06.      *
 *                                                           *
 * C/C++ for Microcontrollers Bindings Version 2.0.3         *
 *                                                           *
 * If you have a bugfix for this file and want to commit it, *
 * please fix the bug in the generator. You can find a link  *
 * to the generators git repository on tinkerforge.com       *
 *************************************************************/

#ifndef TF_PHASE_SWITCHER_H
#define TF_PHASE_SWITCHER_H

#include "config.h"
#include "tfp.h"
#include "hal_common.h"
#include "macros.h"
#include "streaming.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \defgroup TF_PhaseSwitcher Phase Switcher Bricklet
 */

struct TF_PhaseSwitcher;
#if TF_IMPLEMENT_CALLBACKS != 0


#endif
/**
 * \ingroup TF_PhaseSwitcher
 *
 * Phase Switcher
 */
typedef struct TF_PhaseSwitcher {
    TF_TFP *tfp;
#if TF_IMPLEMENT_CALLBACKS != 0

#endif
    uint16_t magic;
    uint8_t response_expected[1];
} TF_PhaseSwitcher;

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_FUNCTION_SET_CONTROL_PILOT_DISCONNECT 1

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_FUNCTION_SET_PHASES_WANTED 2

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_FUNCTION_GET_ALL_DATA 3

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_FUNCTION_GET_SPITFP_ERROR_COUNT 234

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_FUNCTION_SET_BOOTLOADER_MODE 235

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_FUNCTION_GET_BOOTLOADER_MODE 236

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_FUNCTION_SET_WRITE_FIRMWARE_POINTER 237

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_FUNCTION_WRITE_FIRMWARE 238

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_FUNCTION_SET_STATUS_LED_CONFIG 239

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_FUNCTION_GET_STATUS_LED_CONFIG 240

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_FUNCTION_GET_CHIP_TEMPERATURE 242

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_FUNCTION_RESET 243

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_FUNCTION_WRITE_UID 248

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_FUNCTION_READ_UID 249

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_FUNCTION_GET_IDENTITY 255

#if TF_IMPLEMENT_CALLBACKS != 0

#endif

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_CHANNEL_LED_CONFIG_OFF 0

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_CHANNEL_LED_CONFIG_ON 1

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_CHANNEL_LED_CONFIG_SHOW_HEARTBEAT 2

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_CHANNEL_LED_CONFIG_SHOW_CHANNEL_STATUS 3

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_BOOTLOADER_MODE_BOOTLOADER 0

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_BOOTLOADER_MODE_FIRMWARE 1

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_BOOTLOADER_MODE_BOOTLOADER_WAIT_FOR_REBOOT 2

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_BOOTLOADER_MODE_FIRMWARE_WAIT_FOR_REBOOT 3

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_BOOTLOADER_MODE_FIRMWARE_WAIT_FOR_ERASE_AND_REBOOT 4

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_BOOTLOADER_STATUS_OK 0

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_BOOTLOADER_STATUS_INVALID_MODE 1

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_BOOTLOADER_STATUS_NO_CHANGE 2

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_BOOTLOADER_STATUS_ENTRY_FUNCTION_NOT_PRESENT 3

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_BOOTLOADER_STATUS_DEVICE_IDENTIFIER_INCORRECT 4

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_BOOTLOADER_STATUS_CRC_MISMATCH 5

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_STATUS_LED_CONFIG_OFF 0

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_STATUS_LED_CONFIG_ON 1

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_STATUS_LED_CONFIG_SHOW_HEARTBEAT 2

/**
 * \ingroup TF_PhaseSwitcher
 */
#define TF_PHASE_SWITCHER_STATUS_LED_CONFIG_SHOW_STATUS 3

/**
 * \ingroup TF_PhaseSwitcher
 *
 * This constant is used to identify a Phase Switcher Bricklet.
 *
 * The {@link phase_switcher_get_identity} function and the
 * {@link IPCON_CALLBACK_ENUMERATE} callback of the IP Connection have a
 * \c device_identifier parameter to specify the Brick's or Bricklet's type.
 */
#define TF_PHASE_SWITCHER_DEVICE_IDENTIFIER 9002

/**
 * \ingroup TF_PhaseSwitcher
 *
 * This constant represents the display name of a Phase Switcher Bricklet.
 */
#define TF_PHASE_SWITCHER_DEVICE_DISPLAY_NAME "Phase Switcher Bricklet"

/**
 * \ingroup TF_PhaseSwitcher
 *
 * Creates the device object \c phase_switcher with the unique device ID \c uid and adds
 * it to the HAL \c hal.
 */
int tf_phase_switcher_create(TF_PhaseSwitcher *phase_switcher, const char *uid_or_port_name, TF_HAL *hal);

/**
 * \ingroup TF_PhaseSwitcher
 *
 * Removes the device object \c phase_switcher from its HAL and destroys it.
 * The device object cannot be used anymore afterwards.
 */
int tf_phase_switcher_destroy(TF_PhaseSwitcher *phase_switcher);

/**
 * \ingroup TF_PhaseSwitcher
 *
 * Returns the response expected flag for the function specified by the
 * \c function_id parameter. It is *true* if the function is expected to
 * send a response, *false* otherwise.
 *
 * For getter functions this is enabled by default and cannot be disabled,
 * because those functions will always send a response. For callback
 * configuration functions it is enabled by default too, but can be disabled
 * via the tf_phase_switcher_set_response_expected function. For setter
 * functions it is disabled by default and can be enabled.
 *
 * Enabling the response expected flag for a setter function allows to
 * detect timeouts and other error conditions calls of this setter as well.
 * The device will then send a response for this purpose. If this flag is
 * disabled for a setter function then no response is sent and errors are
 * silently ignored, because they cannot be detected.
 */
int tf_phase_switcher_get_response_expected(TF_PhaseSwitcher *phase_switcher, uint8_t function_id, bool *ret_response_expected);

/**
 * \ingroup TF_PhaseSwitcher
 *
 * Changes the response expected flag of the function specified by the
 * \c function_id parameter. This flag can only be changed for setter
 * (default value: *false*) and callback configuration functions
 * (default value: *true*). For getter functions it is always enabled.
 *
 * Enabling the response expected flag for a setter function allows to detect
 * timeouts and other error conditions calls of this setter as well. The device
 * will then send a response for this purpose. If this flag is disabled for a
 * setter function then no response is sent and errors are silently ignored,
 * because they cannot be detected.
 */
int tf_phase_switcher_set_response_expected(TF_PhaseSwitcher *phase_switcher, uint8_t function_id, bool response_expected);

/**
 * \ingroup TF_PhaseSwitcher
 *
 * Changes the response expected flag for all setter and callback configuration
 * functions of this device at once.
 */
int tf_phase_switcher_set_response_expected_all(TF_PhaseSwitcher *phase_switcher, bool response_expected);


#if TF_IMPLEMENT_CALLBACKS != 0
/**
 * \ingroup TF_PhaseSwitcher
 *
 * Polls for callbacks. Will block for the given timeout in microseconds.
 *
 * This function can be used in a non-blocking fashion by calling it with a timeout of 0.
 */
int tf_phase_switcher_callback_tick(TF_PhaseSwitcher *phase_switcher, uint32_t timeout_us);
#endif

/**
 * \ingroup TF_PhaseSwitcher
 *
 * TODO
 */
int tf_phase_switcher_set_control_pilot_disconnect(TF_PhaseSwitcher *phase_switcher, bool control_pilot_disconnect);

/**
 * \ingroup TF_PhaseSwitcher
 *
 * TODO
 */
int tf_phase_switcher_set_phases_wanted(TF_PhaseSwitcher *phase_switcher, uint8_t phases_wanted);

/**
 * \ingroup TF_PhaseSwitcher
 *
 * TODO
 */
int tf_phase_switcher_get_all_data(TF_PhaseSwitcher *phase_switcher, bool *ret_is_control_pilot_disconnected, uint8_t *ret_phases_wanted, uint8_t *ret_phases_active);

/**
 * \ingroup TF_PhaseSwitcher
 *
 * Returns the error count for the communication between Brick and Bricklet.
 *
 * The errors are divided into
 *
 * * ACK checksum errors,
 * * message checksum errors,
 * * framing errors and
 * * overflow errors.
 *
 * The errors counts are for errors that occur on the Bricklet side. All
 * Bricks have a similar function that returns the errors on the Brick side.
 */
int tf_phase_switcher_get_spitfp_error_count(TF_PhaseSwitcher *phase_switcher, uint32_t *ret_error_count_ack_checksum, uint32_t *ret_error_count_message_checksum, uint32_t *ret_error_count_frame, uint32_t *ret_error_count_overflow);

/**
 * \ingroup TF_PhaseSwitcher
 *
 * Sets the bootloader mode and returns the status after the requested
 * mode change was instigated.
 *
 * You can change from bootloader mode to firmware mode and vice versa. A change
 * from bootloader mode to firmware mode will only take place if the entry function,
 * device identifier and CRC are present and correct.
 *
 * This function is used by Brick Viewer during flashing. It should not be
 * necessary to call it in a normal user program.
 */
int tf_phase_switcher_set_bootloader_mode(TF_PhaseSwitcher *phase_switcher, uint8_t mode, uint8_t *ret_status);

/**
 * \ingroup TF_PhaseSwitcher
 *
 * Returns the current bootloader mode, see {@link tf_phase_switcher_set_bootloader_mode}.
 */
int tf_phase_switcher_get_bootloader_mode(TF_PhaseSwitcher *phase_switcher, uint8_t *ret_mode);

/**
 * \ingroup TF_PhaseSwitcher
 *
 * Sets the firmware pointer for {@link tf_phase_switcher_write_firmware}. The pointer has
 * to be increased by chunks of size 64. The data is written to flash
 * every 4 chunks (which equals to one page of size 256).
 *
 * This function is used by Brick Viewer during flashing. It should not be
 * necessary to call it in a normal user program.
 */
int tf_phase_switcher_set_write_firmware_pointer(TF_PhaseSwitcher *phase_switcher, uint32_t pointer);

/**
 * \ingroup TF_PhaseSwitcher
 *
 * Writes 64 Bytes of firmware at the position as written by
 * {@link tf_phase_switcher_set_write_firmware_pointer} before. The firmware is written
 * to flash every 4 chunks.
 *
 * You can only write firmware in bootloader mode.
 *
 * This function is used by Brick Viewer during flashing. It should not be
 * necessary to call it in a normal user program.
 */
int tf_phase_switcher_write_firmware(TF_PhaseSwitcher *phase_switcher, const uint8_t data[64], uint8_t *ret_status);

/**
 * \ingroup TF_PhaseSwitcher
 *
 * Sets the status LED configuration. By default the LED shows
 * communication traffic between Brick and Bricklet, it flickers once
 * for every 10 received data packets.
 *
 * You can also turn the LED permanently on/off or show a heartbeat.
 *
 * If the Bricklet is in bootloader mode, the LED is will show heartbeat by default.
 */
int tf_phase_switcher_set_status_led_config(TF_PhaseSwitcher *phase_switcher, uint8_t config);

/**
 * \ingroup TF_PhaseSwitcher
 *
 * Returns the configuration as set by {@link tf_phase_switcher_set_status_led_config}
 */
int tf_phase_switcher_get_status_led_config(TF_PhaseSwitcher *phase_switcher, uint8_t *ret_config);

/**
 * \ingroup TF_PhaseSwitcher
 *
 * Returns the temperature as measured inside the microcontroller. The
 * value returned is not the ambient temperature!
 *
 * The temperature is only proportional to the real temperature and it has bad
 * accuracy. Practically it is only useful as an indicator for
 * temperature changes.
 */
int tf_phase_switcher_get_chip_temperature(TF_PhaseSwitcher *phase_switcher, int16_t *ret_temperature);

/**
 * \ingroup TF_PhaseSwitcher
 *
 * Calling this function will reset the Bricklet. All configurations
 * will be lost.
 *
 * After a reset you have to create new device objects,
 * calling functions on the existing ones will result in
 * undefined behavior!
 */
int tf_phase_switcher_reset(TF_PhaseSwitcher *phase_switcher);

/**
 * \ingroup TF_PhaseSwitcher
 *
 * Writes a new UID into flash. If you want to set a new UID
 * you have to decode the Base58 encoded UID string into an
 * integer first.
 *
 * We recommend that you use Brick Viewer to change the UID.
 */
int tf_phase_switcher_write_uid(TF_PhaseSwitcher *phase_switcher, uint32_t uid);

/**
 * \ingroup TF_PhaseSwitcher
 *
 * Returns the current UID as an integer. Encode as
 * Base58 to get the usual string version.
 */
int tf_phase_switcher_read_uid(TF_PhaseSwitcher *phase_switcher, uint32_t *ret_uid);

/**
 * \ingroup TF_PhaseSwitcher
 *
 * Returns the UID, the UID where the Bricklet is connected to,
 * the position, the hardware and firmware version as well as the
 * device identifier.
 *
 * The position can be 'a', 'b', 'c', 'd', 'e', 'f', 'g' or 'h' (Bricklet Port).
 * A Bricklet connected to an :ref:`Isolator Bricklet <isolator_bricklet>` is always at
 * position 'z'.
 *
 * The device identifier numbers can be found :ref:`here <device_identifier>`.
 * |device_identifier_constant|
 */
int tf_phase_switcher_get_identity(TF_PhaseSwitcher *phase_switcher, char ret_uid[8], char ret_connected_uid[8], char *ret_position, uint8_t ret_hardware_version[3], uint8_t ret_firmware_version[3], uint16_t *ret_device_identifier);

#ifdef __cplusplus
}
#endif

#endif
