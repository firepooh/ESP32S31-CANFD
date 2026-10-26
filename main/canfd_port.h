/*
 * CAN-FD port abstraction on top of the ESP-IDF TWAI (on-chip) driver.
 *
 * Port numbers are 1-based (CAN1, CAN2) to match the board wiring.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_twai.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CANFD_PORT_NUM      2
#define CANFD_PORT_FIRST    1
#define CANFD_PORT_LAST     CANFD_PORT_NUM

typedef enum {
    CANFD_MODE_NORMAL = 0,      /**< Normal operation, needs another node to ACK */
    CANFD_MODE_LISTEN_ONLY,     /**< Monitor the bus only, no TX and no ACK */
    CANFD_MODE_SELF_TEST,       /**< Internal loopback, no ACK required */
} canfd_mode_t;

typedef enum {
    CANFD_FAULT_NONE = 0,       /**< TX pin driven by the TWAI controller */
    CANFD_FAULT_DOMINANT,       /**< TX pin forced low: the transceiver holds the bus dominant */
    CANFD_FAULT_RECESSIVE,      /**< TX pin forced high: the node can no longer drive the bus */
} canfd_fault_t;

typedef struct {
    uint32_t nominal_bitrate;       /**< Arbitration phase bitrate [bps] */
    uint32_t data_bitrate;          /**< Data phase bitrate [bps], 0 = classic CAN only */
    uint16_t nominal_sp_permill;    /**< 0 = driver default */
    uint16_t data_sp_permill;       /**< 0 = driver default */
    uint16_t data_ssp_permill;      /**< Secondary sample point, 0 = disabled */
    int8_t retry_limit;             /**< Hardware retransmission limit [-1:15], -1 = retry forever */
    uint32_t auto_recover_ms;       /**< Start bus-off recovery this long after bus-off, 0 = manual only */
    canfd_mode_t mode;
} canfd_port_config_t;

typedef struct {
    uint32_t tx_ok;             /**< Frames transmitted successfully */
    uint32_t tx_fail;           /**< Frames reported as failed by the driver */
    uint32_t rx;                /**< Frames received by the ISR */
    uint32_t rx_drop;           /**< Frames dropped because the application queue was full */
    uint32_t bus_err;           /**< Bus error events */
    uint32_t arb_lost;
    uint32_t bit_err;
    uint32_t form_err;
    uint32_t stuff_err;
    uint32_t ack_err;
    uint32_t bus_off;           /**< Number of transitions into bus-off */
    uint32_t err_passive;       /**< Number of transitions into error passive */
    uint32_t recover;           /**< Bus-off recoveries started (manual and automatic) */
    uint32_t rxq_peak;          /**< Highest RX queue fill level seen (frames) */
    uint32_t txpool_peak;       /**< Highest number of TX frame slots in use */
} canfd_port_stats_t;

typedef struct {
    uint32_t rxq_depth;         /**< RX queue length (events) */
    uint32_t rxq_item_size;     /**< Bytes per RX queue entry */
    const void *rxq_buf;        /**< RX queue storage */
    bool rxq_in_psram;
    uint32_t txpool_size;       /**< TX frame slots */
    uint32_t txpool_item_size;
    const void *txpool_buf;
    bool txpool_in_psram;
} canfd_port_mem_info_t;

typedef struct {
    twai_frame_header_t header;
    uint8_t len;                            /**< Payload length in bytes */
    uint8_t data[TWAIFD_FRAME_MAX_LEN];
} canfd_msg_t;

/**
 * @brief RX hook, called from the port's RX task for every received frame.
 *        While a hook is set, frames are not printed by the dump function.
 */
typedef void (*canfd_rx_hook_t)(int port, const canfd_msg_t *msg, void *arg);

void canfd_port_get_default_config(canfd_port_config_t *cfg);
void canfd_port_get_pins(int port, int *tx_gpio, int *rx_gpio);
bool canfd_port_is_valid(int port);
bool canfd_port_is_open(int port);
const canfd_port_config_t *canfd_port_get_config(int port);

esp_err_t canfd_port_open(int port, const canfd_port_config_t *cfg);
esp_err_t canfd_port_close(int port);
esp_err_t canfd_port_recover(int port, int timeout_ms);

/**
 * @brief Queue one frame for transmission.
 *        The payload is copied, so @p msg may be reused right after the call.
 *        For FD frames the length is rounded up to the next valid FD length and padded with 0x00.
 */
esp_err_t canfd_port_send(int port, const canfd_msg_t *msg, int timeout_ms);

/**
 * @brief Wait until all queued frames of the port have been sent.
 */
esp_err_t canfd_port_wait_tx_done(int port, int timeout_ms);

esp_err_t canfd_port_get_status(int port, twai_node_status_t *status, canfd_port_stats_t *stats);
void canfd_port_reset_stats(int port);
void canfd_port_reset_peaks(int port);
esp_err_t canfd_port_get_mem_info(int port, canfd_port_mem_info_t *info);

void canfd_port_set_dump(int port, bool enable);
bool canfd_port_get_dump(int port);

/**
 * @brief Print a bus error summary (at most every 500 ms) while errors keep occurring. Default: on.
 */
void canfd_port_set_errlog(int port, bool enable);
bool canfd_port_get_errlog(int port);

/**
 * @brief Fault injection: take the TX pin away from the controller and hold it dominant or recessive.
 *        Works on a closed port too (the pin then acts like a faulty node on the bus).
 *        CANFD_FAULT_NONE gives the pin back to the controller.
 */
esp_err_t canfd_port_inject_fault(int port, canfd_fault_t fault);
canfd_fault_t canfd_port_get_fault(int port);
const char *canfd_fault_str(canfd_fault_t fault);
void canfd_port_set_rx_hook(int port, canfd_rx_hook_t hook, void *arg);

/**
 * @brief Round a payload length up to the next valid CAN-FD length (0..8, 12, 16, 20, 24, 32, 48, 64).
 */
uint8_t canfd_round_fd_len(uint8_t len);

const char *canfd_state_str(twai_error_state_t state);
const char *canfd_mode_str(canfd_mode_t mode);

#ifdef __cplusplus
}
#endif
