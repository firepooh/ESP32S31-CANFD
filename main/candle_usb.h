/*
 * USB device (ESP32-S31 HS OTG) for the Candlelight / ElmueSoft protocol.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_BLOB_SIZE_USB   2048    // largest IN/OUT transfer (MAX_BLOB_SIZE of the protocol)

/**
 * @brief Start the protocol engine and the TinyUSB device on the high-speed OTG port.
 */
esp_err_t candle_usb_init(void);

/**
 * @brief Register the "usb" console command.
 */
void register_candle_commands(void);

#ifdef __cplusplus
}
#endif
