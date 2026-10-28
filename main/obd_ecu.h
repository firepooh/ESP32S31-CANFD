/*
 * OBD-II ECU simulator (MODE2): answers service 01 / 09 requests on 7DF and on the configured
 * physical request ID. The response ID is the request ID + 8. Classic CAN, 11-bit IDs only.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "canfd_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OBD_ECU_TX_TAG      0xFFFFFFFEu     // canfd_msg_t.tag of the ECU's frames
#define OBD_VIN_LEN         17

typedef enum {
    OBD_ITEM_COOLANT = 0,   // 01 05
    OBD_ITEM_RPM,           // 01 0C
    OBD_ITEM_SPEED,         // 01 0D
    OBD_ITEM_RUNTIME,       // 01 1F, seconds since boot
    OBD_ITEM_FUEL,          // 01 2F
    OBD_ITEM_ODO,           // 01 A6
    OBD_ITEM_VIN,           // 09 02
    OBD_ITEM_COUNT,
} obd_item_t;

typedef struct {
    uint16_t req_id;        // physical request ID
    uint8_t enabled;        // bit per obd_item_t
    uint8_t speed_kmh;
    uint8_t fuel_pct;
    int16_t coolant_c;
    uint16_t rpm;
    uint32_t odo_km;
    char vin[OBD_VIN_LEN + 1];
} obd_ecu_cfg_t;

/**
 * @brief Load the settings of the port from NVS (defaults if none are stored).
 */
void obd_ecu_init(int port);

/**
 * @brief Handle a received frame (called from the port's RX task).
 */
void obd_ecu_rx(int port, const canfd_msg_t *msg);

obd_ecu_cfg_t *obd_ecu_cfg(int port);
esp_err_t obd_ecu_save(int port);
void obd_ecu_print(int port);

/**
 * @brief Item by name: "05", "0C", "0D", "1F", "2F", "A6" or "VIN". Returns OBD_ITEM_COUNT if unknown.
 */
obd_item_t obd_ecu_item_find(const char *name);

#ifdef __cplusplus
}
#endif
