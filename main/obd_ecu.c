/*
 * OBD-II ECU simulator (see obd_ecu.h).
 *
 * - Service 01: PIDs 05, 0C, 0D, 1F, 2F, A6 and the supported-PID ranges 00..A0,
 *   computed from the enabled items. Only the first PID of a request is answered.
 * - Service 09: PID 00 and 02 (VIN).
 * - Service 03: the stored DTCs from the settings (none: 43 00). Service 04 clears them
 *   until the next boot or until the list is set again; the stored list is kept.
 * - Responses longer than 7 bytes go out as ISO-TP FF + CFs; the CFs are sent right after
 *   the tester's flow control (block size and STmin are not applied).
 * - Unsupported requests get no response.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "obd_ecu.h"

static const char *TAG = "obd_ecu";

#define NVS_NAMESPACE       "hybrid"
#define OBD_FUNC_ID         0x7DF
#define OBD_RESP_OFFSET     8
#define OBD_PAD             0xAA
#define OBD_SEND_TIMEOUT_MS 10
#define OBD_FC_TIMEOUT_US   1000000     // N_Bs

static const struct {
    uint8_t sid;
    uint8_t pid;
    const char *name;
    const char *desc;
} k_items[OBD_ITEM_COUNT] = {
    [OBD_ITEM_COOLANT] = { 0x01, 0x05, "05",  "coolant temperature [C]" },
    [OBD_ITEM_RPM]     = { 0x01, 0x0C, "0C",  "engine speed [rpm]" },
    [OBD_ITEM_SPEED]   = { 0x01, 0x0D, "0D",  "vehicle speed [km/h]" },
    [OBD_ITEM_RUNTIME] = { 0x01, 0x1F, "1F",  "run time since start [s]" },
    [OBD_ITEM_FUEL]    = { 0x01, 0x2F, "2F",  "fuel level [%]" },
    [OBD_ITEM_ODO]     = { 0x01, 0xA6, "A6",  "odometer [km]" },
    [OBD_ITEM_VIN]     = { 0x09, 0x02, "VIN", "vehicle identification number" },
    [OBD_ITEM_DTC]     = { 0x03, 0x00, "DTC", "stored DTCs (03), clear (04)" },
};

/* Multi-frame response waiting for the tester's flow control */
typedef struct {
    bool wait_fc;
    int64_t ff_us;
    uint8_t len;
    uint8_t data[3 + OBD_VIN_LEN];      // VIN: 20 bytes, 43 N + 8 DTCs: 18 bytes
} iso_tx_t;

static obd_ecu_cfg_t s_cfg[CANFD_PORT_NUM];
static iso_tx_t s_tx[CANFD_PORT_NUM];
static bool s_dtc_cleared[CANFD_PORT_NUM];

static inline int port_idx(int port)
{
    return port - CANFD_PORT_FIRST;
}

static inline bool item_on(const obd_ecu_cfg_t *cfg, obd_item_t item)
{
    return cfg->enabled & (1u << item);
}

/* ------------------------------------------------ Responses ------------------------------------------------ */

static void send_frame(int port, const obd_ecu_cfg_t *cfg, const uint8_t *data, uint8_t len)
{
    canfd_msg_t m = { 0 };
    m.header.id = cfg->req_id + OBD_RESP_OFFSET;
    m.len = 8;
    m.tag = OBD_ECU_TX_TAG;
    memcpy(m.data, data, len);
    memset(m.data + len, OBD_PAD, 8 - len);
    if (canfd_port_send(port, &m, OBD_SEND_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "CAN%d: response %03" PRIX32 " not sent", port, m.header.id);
    }
}

/* Single frame: PCI + payload (at most 7 bytes) */
static void send_single(int port, const obd_ecu_cfg_t *cfg, const uint8_t *payload, uint8_t len)
{
    uint8_t buf[8] = { len };
    memcpy(buf + 1, payload, len);
    send_frame(port, cfg, buf, (uint8_t)(len + 1));
}

/* Single frame up to 7 bytes, otherwise FF now and the CFs after the flow control */
static void send_payload(int port, const obd_ecu_cfg_t *cfg, const uint8_t *payload, uint8_t len)
{
    if (len <= 7) {
        send_single(port, cfg, payload, len);
        return;
    }
    iso_tx_t *tx = &s_tx[port_idx(port)];
    if (len > sizeof(tx->data)) {
        return;
    }
    uint8_t ff[8] = { 0x10, len };
    memcpy(ff + 2, payload, 6);
    memcpy(tx->data, payload, len);
    tx->len = len;
    tx->ff_us = esp_timer_get_time();
    tx->wait_fc = true;
    send_frame(port, cfg, ff, sizeof(ff));
}

static void send_cfs(int port, const obd_ecu_cfg_t *cfg)
{
    const iso_tx_t *tx = &s_tx[port_idx(port)];
    uint8_t sn = 1;
    for (int pos = 6; pos < tx->len; pos += 7, sn++) {
        uint8_t cf[8] = { (uint8_t)(0x20 | (sn & 0x0F)) };
        const int n = tx->len - pos < 7 ? tx->len - pos : 7;
        memcpy(cf + 1, tx->data + pos, n);
        send_frame(port, cfg, cf, (uint8_t)(n + 1));
    }
}

/* Bits of the PIDs base+1 .. base+0x20, MSB first; the last bit announces the next range */
static uint32_t supported_bits(const obd_ecu_cfg_t *cfg, uint8_t base)
{
    uint32_t bits = 0;
    for (int i = 0; i < OBD_ITEM_COUNT; i++) {
        if (k_items[i].sid != 0x01 || !item_on(cfg, (obd_item_t)i)) {
            continue;
        }
        const int pid = k_items[i].pid;
        if (pid > base && pid <= base + 0x20) {
            bits |= 1u << (32 - (pid - base));
        } else if (pid > base + 0x20) {
            bits |= 1u;
        }
    }
    return bits;
}

static void put_be(uint8_t *p, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static void service_01(int port, const obd_ecu_cfg_t *cfg, uint8_t pid)
{
    uint8_t p[7] = { 0x41, pid };
    uint8_t n;

    if (pid % 0x20 == 0 && pid <= 0xA0) {
        const uint32_t bits = supported_bits(cfg, pid);
        if (pid != 0 && bits == 0) {
            return;
        }
        put_be(p + 2, bits, 4);
        send_single(port, cfg, p, 6);
        return;
    }

    obd_item_t item = OBD_ITEM_COUNT;
    for (int i = 0; i < OBD_ITEM_COUNT; i++) {
        if (k_items[i].sid == 0x01 && k_items[i].pid == pid) {
            item = (obd_item_t)i;
        }
    }
    if (item == OBD_ITEM_COUNT || !item_on(cfg, item)) {
        return;
    }
    switch (item) {
    case OBD_ITEM_COOLANT: {
        const int a = cfg->coolant_c + 40;
        p[2] = (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a);
        n = 3;
        break;
    }
    case OBD_ITEM_RPM:
        put_be(p + 2, (uint32_t)cfg->rpm * 4 > 0xFFFF ? 0xFFFF : cfg->rpm * 4, 2);
        n = 4;
        break;
    case OBD_ITEM_SPEED:
        p[2] = cfg->speed_kmh;
        n = 3;
        break;
    case OBD_ITEM_RUNTIME: {
        const int64_t s = esp_timer_get_time() / 1000000;
        put_be(p + 2, s > 0xFFFF ? 0xFFFF : (uint32_t)s, 2);
        n = 4;
        break;
    }
    case OBD_ITEM_FUEL:
        p[2] = (uint8_t)((cfg->fuel_pct * 255 + 50) / 100);
        n = 3;
        break;
    case OBD_ITEM_ODO:
        put_be(p + 2, cfg->odo_km * 10, 4);    // 0.1 km per bit
        n = 6;
        break;
    default:
        return;
    }
    send_single(port, cfg, p, n);
}

static void service_09(int port, const obd_ecu_cfg_t *cfg, uint8_t pid)
{
    if (!item_on(cfg, OBD_ITEM_VIN)) {
        return;
    }
    if (pid == 0x00) {
        const uint8_t p[6] = { 0x49, 0x00, 0x40, 0x00, 0x00, 0x00 };   // only PID 02
        send_single(port, cfg, p, sizeof(p));
    } else if (pid == 0x02) {
        uint8_t p[3 + OBD_VIN_LEN] = { 0x49, 0x02, 0x01 };     // 1 data item
        memcpy(p + 3, cfg->vin, OBD_VIN_LEN);
        send_payload(port, cfg, p, sizeof(p));
    }
}

/* 03: 43, number of DTCs, 2 bytes per DTC */
static void service_03(int port, const obd_ecu_cfg_t *cfg)
{
    if (!item_on(cfg, OBD_ITEM_DTC)) {
        return;
    }
    const uint8_t n = s_dtc_cleared[port_idx(port)] ? 0 : cfg->dtc_count;
    uint8_t p[2 + 2 * OBD_DTC_MAX] = { 0x43, n };
    for (int i = 0; i < n; i++) {
        put_be(p + 2 + 2 * i, cfg->dtc[i], 2);
    }
    send_payload(port, cfg, p, (uint8_t)(2 + 2 * n));
}

static void service_04(int port, const obd_ecu_cfg_t *cfg)
{
    if (!item_on(cfg, OBD_ITEM_DTC)) {
        return;
    }
    s_dtc_cleared[port_idx(port)] = true;
    const uint8_t p[1] = { 0x44 };
    send_single(port, cfg, p, sizeof(p));
}

void obd_ecu_rx(int port, const canfd_msg_t *msg)
{
    const obd_ecu_cfg_t *cfg = &s_cfg[port_idx(port)];
    iso_tx_t *tx = &s_tx[port_idx(port)];
    const twai_frame_header_t *h = &msg->header;
    const uint8_t *d = msg->data;

    if (h->ide || h->rtr || h->fdf || msg->len < 2) {
        return;
    }
    if (h->id != OBD_FUNC_ID && h->id != cfg->req_id) {
        return;
    }

    switch (d[0] >> 4) {
    case 0x3:   // flow control for a multi-frame response, always sent to the physical address
        if (tx->wait_fc && h->id == cfg->req_id) {
            tx->wait_fc = false;
            if (d[0] == 0x30 && esp_timer_get_time() - tx->ff_us < OBD_FC_TIMEOUT_US) {
                send_cfs(port, cfg);
            }
        }
        return;
    case 0x0: { // single frame request
        const uint8_t n = d[0] & 0x0F;
        if (n < 1 || n > msg->len - 1) {
            return;
        }
        if (d[1] == 0x01 && n >= 2) {
            service_01(port, cfg, d[2]);
        } else if (d[1] == 0x09 && n >= 2) {
            service_09(port, cfg, d[2]);
        } else if (d[1] == 0x03) {
            service_03(port, cfg);
        } else if (d[1] == 0x04) {
            service_04(port, cfg);
        }
        return;
    }
    default:
        return;
    }
}

/* ------------------------------------------------ Settings ------------------------------------------------ */

static void set_defaults(obd_ecu_cfg_t *cfg)
{
    *cfg = (obd_ecu_cfg_t) {
        .req_id = 0x7E0,
        .enabled = (1u << OBD_ITEM_COUNT) - 1,
        .speed_kmh = 0,
        .fuel_pct = 75,
        .coolant_c = 90,
        .rpm = 800,
        .odo_km = 12345,
    };
    strlcpy(cfg->vin, "KMHESP32S31000001", sizeof(cfg->vin));
}

void obd_ecu_init(int port)
{
    obd_ecu_cfg_t *cfg = &s_cfg[port_idx(port)];
    set_defaults(cfg);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    char key[8];
    snprintf(key, sizeof(key), "obd%d", port);
    obd_ecu_cfg_t stored;
    size_t len = sizeof(stored);
    if (nvs_get_blob(h, key, &stored, &len) == ESP_OK && len == sizeof(stored)) {
        stored.vin[OBD_VIN_LEN] = '\0';
        *cfg = stored;
    }
    nvs_close(h);
}

obd_ecu_cfg_t *obd_ecu_cfg(int port)
{
    return canfd_port_is_valid(port) ? &s_cfg[port_idx(port)] : NULL;
}

esp_err_t obd_ecu_save(int port)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    char key[8];
    snprintf(key, sizeof(key), "obd%d", port);
    err = nvs_set_blob(h, key, &s_cfg[port_idx(port)], sizeof(obd_ecu_cfg_t));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

obd_item_t obd_ecu_item_find(const char *name)
{
    for (int i = 0; i < OBD_ITEM_COUNT; i++) {
        if (strcasecmp(name, k_items[i].name) == 0) {
            return (obd_item_t)i;
        }
    }
    if (strcasecmp(name, "0902") == 0) {
        return OBD_ITEM_VIN;
    }
    return OBD_ITEM_COUNT;
}

/* "P0301" -> 0x0301: letter P/C/B/U in bits 15-14, then 4 hex digits (the first one 0..3) */
static bool parse_dtc(const char *s, uint16_t *code)
{
    static const char letters[] = "PCBU";
    const char *l = s[0] ? strchr(letters, toupper((unsigned char)s[0])) : NULL;
    if (!l || strlen(s) != 5 || s[1] < '0' || s[1] > '3') {
        return false;
    }
    char *end;
    const unsigned long v = strtoul(s + 1, &end, 16);
    if (*end != '\0') {
        return false;
    }
    *code = (uint16_t)(((l - letters) << 14) | v);
    return true;
}

static void format_dtc(uint16_t code, char *buf)
{
    snprintf(buf, 6, "%c%04X", "PCBU"[code >> 14], code & 0x3FFF);
}

bool obd_ecu_set_dtcs(int port, int count, char **codes)
{
    uint16_t dtc[OBD_DTC_MAX];
    if (count > OBD_DTC_MAX) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (!parse_dtc(codes[i], &dtc[i])) {
            return false;
        }
    }
    obd_ecu_cfg_t *cfg = &s_cfg[port_idx(port)];
    memcpy(cfg->dtc, dtc, count * sizeof(dtc[0]));
    cfg->dtc_count = (uint8_t)count;
    s_dtc_cleared[port_idx(port)] = false;
    return true;
}

void obd_ecu_print(int port)
{
    const obd_ecu_cfg_t *cfg = &s_cfg[port_idx(port)];
    printf("CAN%d OBD ECU: request %03X / 7DF, response %03X\n", port, cfg->req_id, cfg->req_id + OBD_RESP_OFFSET);
    for (int i = 0; i < OBD_ITEM_COUNT; i++) {
        printf("  %-3s %-3s  %-32s ", k_items[i].name, item_on(cfg, (obd_item_t)i) ? "on" : "off", k_items[i].desc);
        switch ((obd_item_t)i) {
        case OBD_ITEM_COOLANT: printf("%d\n", cfg->coolant_c); break;
        case OBD_ITEM_RPM:     printf("%u\n", cfg->rpm); break;
        case OBD_ITEM_SPEED:   printf("%u\n", cfg->speed_kmh); break;
        case OBD_ITEM_RUNTIME: printf("%" PRId64 "\n", esp_timer_get_time() / 1000000); break;
        case OBD_ITEM_FUEL:    printf("%u\n", cfg->fuel_pct); break;
        case OBD_ITEM_ODO:     printf("%" PRIu32 "\n", cfg->odo_km); break;
        case OBD_ITEM_VIN:     printf("%s\n", cfg->vin); break;
        case OBD_ITEM_DTC:
            for (int d = 0; d < cfg->dtc_count; d++) {
                char code[6];
                format_dtc(cfg->dtc[d], code);
                printf("%s ", code);
            }
            printf("%s\n", cfg->dtc_count == 0 ? "none" :
                   s_dtc_cleared[port_idx(port)] ? "(cleared by 04 until reboot)" : "");
            break;
        default:               printf("\n"); break;
        }
    }
}
