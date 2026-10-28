/*
 * Per-port operating mode (see ch_mode.h).
 *
 * In MODE2 this file owns the port hooks: received frames go to the USB host and to the OBD ECU,
 * TX completions of the host's frames go back to the host, and the ECU's responses are shown
 * to the host as received frames.
 */
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ch_mode.h"
#include "obd_ecu.h"
#if CONFIG_CANDLE_USB_ENABLE
#include "candle.h"
#endif

static const char *TAG = "ch_mode";

#define NVS_NAMESPACE       "hybrid"
#define OBD_BITRATE         500000
#define OBD_AUTO_RECOVER_MS 100

static ch_mode_t s_mode[CANFD_PORT_NUM];
static int64_t s_ts_offset[CANFD_PORT_NUM];     // node RX timestamp - esp_timer, for the ECU's frames

static inline int port_idx(int port)
{
    return port - CANFD_PORT_FIRST;
}

const char *ch_mode_str(ch_mode_t mode)
{
    switch (mode) {
    case CH_MODE_OFF: return "off";
    case CH_MODE_USB: return "USB adapter";
    case CH_MODE_OBD: return "USB adapter + OBD ECU";
    default:          return "unknown";
    }
}

ch_mode_t ch_mode_get(int port)
{
    return canfd_port_is_valid(port) ? s_mode[port_idx(port)] : CH_MODE_OFF;
}

/* ------------------------------------------------ MODE2 port hooks (canN_rx task) ------------------------------------------------ */

static void obd_rx_hook(int port, const canfd_msg_t *msg, void *arg)
{
    s_ts_offset[port_idx(port)] = (int64_t)msg->header.timestamp - esp_timer_get_time();
#if CONFIG_CANDLE_USB_ENABLE
    candle_port_rx(port, msg);
#endif
    obd_ecu_rx(port, msg);
}

static void obd_tx_hook(int port, const canfd_msg_t *msg, bool success, void *arg)
{
#if CONFIG_CANDLE_USB_ENABLE
    if (msg->tag != OBD_ECU_TX_TAG) {
        candle_port_tx_done(port, msg, success);
        return;
    }
    if (success) {
        canfd_msg_t m = *msg;
        m.header.timestamp = (uint64_t)(esp_timer_get_time() + s_ts_offset[port_idx(port)]);
        candle_port_rx(port, &m);
    }
#endif
}

static esp_err_t open_with_hooks(int port, const canfd_port_config_t *cfg)
{
    if (canfd_port_is_open(port)) {
        canfd_port_close(port);
    }
    canfd_port_set_rx_hook(port, obd_rx_hook, NULL);
    canfd_port_set_tx_hook(port, obd_tx_hook, NULL);
    esp_err_t err = canfd_port_open(port, cfg);
    if (err != ESP_OK) {
        canfd_port_set_rx_hook(port, NULL, NULL);
        canfd_port_set_tx_hook(port, NULL, NULL);
    }
    return err;
}

esp_err_t ch_mode_port_open(int port, const canfd_port_config_t *cfg)
{
    canfd_port_config_t def;
    canfd_port_get_default_config(&def);
    def.nominal_bitrate = OBD_BITRATE;
    def.data_bitrate = 0;
    def.auto_recover_ms = OBD_AUTO_RECOVER_MS;

    esp_err_t err = open_with_hooks(port, cfg ? cfg : &def);
    if (err != ESP_OK && cfg) {
        ESP_LOGW(TAG, "CAN%d: host settings failed, ECU stays at %d bps", port, OBD_BITRATE);
        open_with_hooks(port, &def);
    }
    return err;
}

/* ------------------------------------------------ Mode setting ------------------------------------------------ */

static void apply(int port)
{
    if (s_mode[port_idx(port)] == CH_MODE_OBD) {
        if (ch_mode_port_open(port, NULL) == ESP_OK) {
            ESP_LOGI(TAG, "CAN%d: OBD ECU running at %d bps", port, OBD_BITRATE);
        }
    } else if (canfd_port_is_open(port)) {
        canfd_port_close(port);
    }
}

esp_err_t ch_mode_set(int port, ch_mode_t mode)
{
    if (!canfd_port_is_valid(port) || mode >= CH_MODE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_CANDLE_USB_ENABLE
    if (candle_channel_is_open(port_idx(port))) {
        return ESP_ERR_INVALID_STATE;
    }
#endif
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        char key[8];
        snprintf(key, sizeof(key), "mode%d", port);
        err = nvs_set_u8(h, key, (uint8_t)mode);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        return err;
    }
    s_mode[port_idx(port)] = mode;
    apply(port);
    return ESP_OK;
}

void ch_mode_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s, all ports stay off", esp_err_to_name(err));
        return;
    }

    nvs_handle_t h;
    const bool have_nvs = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK;
    for (int port = CANFD_PORT_FIRST; port <= CANFD_PORT_LAST; port++) {
        uint8_t mode = CH_MODE_OFF;
        if (have_nvs) {
            char key[8];
            snprintf(key, sizeof(key), "mode%d", port);
            nvs_get_u8(h, key, &mode);
        }
        s_mode[port_idx(port)] = mode < CH_MODE_COUNT ? (ch_mode_t)mode : CH_MODE_OFF;
        obd_ecu_init(port);
        ESP_LOGI(TAG, "CAN%d: mode %d (%s)", port, s_mode[port_idx(port)], ch_mode_str(s_mode[port_idx(port)]));
        apply(port);
    }
    if (have_nvs) {
        nvs_close(h);
    }
}
