/*
 * Per-port operating mode, stored in NVS (default MODE0).
 *
 *   MODE0  off: the port is never opened
 *   MODE1  USB adapter only (cangaroo opens and closes the port)
 *   MODE2  USB adapter + OBD-II ECU simulator on the same port. The ECU always runs:
 *          without a host at 500 kbit/s, while the host has the channel started with the host's settings.
 */
#pragma once

#include "esp_err.h"
#include "canfd_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CH_MODE_OFF = 0,
    CH_MODE_USB = 1,
    CH_MODE_OBD = 2,
    CH_MODE_COUNT,
} ch_mode_t;

/**
 * @brief Load the modes and OBD settings from NVS and start the MODE2 ports.
 */
void ch_mode_init(void);

ch_mode_t ch_mode_get(int port);
const char *ch_mode_str(ch_mode_t mode);

/**
 * @brief Store the mode in NVS and apply it. Fails while the USB host has the channel started.
 */
esp_err_t ch_mode_set(int port, ch_mode_t mode);

/**
 * @brief (Re)open a MODE2 port with the ECU attached.
 * @param cfg Host settings, or NULL for the ECU default (500 kbit/s classic).
 *            If the host settings fail, the port falls back to the default so the ECU keeps running.
 */
esp_err_t ch_mode_port_open(int port, const canfd_port_config_t *cfg);

void register_mode_commands(void);

#ifdef __cplusplus
}
#endif
