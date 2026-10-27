/*
 * Candlelight / ElmueSoft CANable 2.5 protocol on top of canfd_port.
 *
 * CAN1 = channel 0 = USB interface 0, CAN2 = channel 1 = USB interface 2 (interface 1 is the
 * firmware update interface, as in the CANable 2.5 firmware). The 'itf' arguments below are the
 * TinyUSB vendor instance numbers: 0 = CAN1, 1 = CAN2.
 * Legacy protocol: all channels use interface 0 (kHostFrameLegacy.channel selects the channel).
 * ElmueSoft protocol: each channel uses its own interface.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CANDLE_CHANNEL_COUNT    2
#define CANDLE_FW_VERSION_BCD   0x261027    // YYMMDD
#define CANDLE_HW_VERSION_BCD   0x0310

/**
 * @brief Prepare the protocol state and start the background task (errors, bus load, LED).
 */
esp_err_t candle_init(void);

/* Called from the TinyUSB task (candle_usb.c) */
bool candle_control_request(uint8_t rhport, uint8_t stage, const tusb_control_request_t *req);
void candle_usb_rx(uint8_t itf, const uint8_t *buf, uint32_t len);
void candle_usb_tx_done(uint8_t itf);
void candle_usb_mount_changed(bool mounted);

/**
 * @brief True while the USB host has the channel started (the port is controlled by the host).
 */
bool candle_channel_is_open(int channel);

/**
 * @brief Print USB / protocol state to the console.
 */
void candle_print_status(void);

#ifdef __cplusplus
}
#endif
