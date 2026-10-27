/*
 * USB device for the Candlelight / ElmueSoft protocol on the ESP32-S31 high-speed OTG port.
 *
 * - Two vendor interfaces: interface 0 = CAN1, interface 1 = CAN2, each with one bulk IN/OUT pair
 * - MS OS 2.0 descriptors bind WinUSB to both interfaces and register the Candlelight device
 *   interface GUID, so Windows needs no driver installation and cangaroo finds the adapter
 * - The vendor class runs without FIFOs: one USB transfer is one protocol message (or blob)
 */
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_console.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "candle.h"
#include "candle_usb.h"

static const char *TAG = "candle_usb";

#define VENDOR_REQUEST_MS_OS_20     0x77
#define EP_SIZE_FS                  64
#define EP_SIZE_HS                  512
#define CONFIG_TOTAL_LEN            (TUD_CONFIG_DESC_LEN + CANDLE_CHANNEL_COUNT * TUD_VENDOR_DESC_LEN)

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CAN1,
    STRID_CAN2,
    STRID_COUNT,
};

_Static_assert(CFG_TUD_VENDOR == CANDLE_CHANNEL_COUNT, "set CONFIG_TINYUSB_VENDOR_COUNT=2");
_Static_assert(CFG_TUD_VENDOR_RX_BUFSIZE == 0 && CFG_TUD_VENDOR_TX_BUFSIZE == 0,
               "vendor FIFOs must be off: one USB transfer per protocol message");
_Static_assert(CFG_TUD_VENDOR_EPSIZE >= MAX_BLOB_SIZE_USB, "CONFIG_TINYUSB_VENDOR_EPSIZE must hold a 2 KB blob");

static const tusb_desc_device_t s_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210,                   // 2.1: Windows reads the BOS (MS OS 2.0) descriptor
    .bDeviceClass = 0x00,               // class per interface
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,                 // Espressif
    .idProduct = CONFIG_CANDLE_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

static const tusb_desc_device_qualifier_t s_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0210,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 1,
    .bReserved = 0,
};

static const uint8_t s_fs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, CANDLE_CHANNEL_COUNT, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_VENDOR_DESCRIPTOR(0, STRID_CAN1, 0x01, 0x81, EP_SIZE_FS),
    TUD_VENDOR_DESCRIPTOR(1, STRID_CAN2, 0x02, 0x82, EP_SIZE_FS),
};

static const uint8_t s_hs_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, CANDLE_CHANNEL_COUNT, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_VENDOR_DESCRIPTOR(0, STRID_CAN1, 0x01, 0x81, EP_SIZE_HS),
    TUD_VENDOR_DESCRIPTOR(1, STRID_CAN2, 0x02, 0x82, EP_SIZE_HS),
};

/* ------------------------------------------------ MS OS 2.0 ------------------------------------------------ */

// Device interface GUID of candleLight / gs_usb adapters, searched by cangaroo and the candle API
#define CANDLE_INTERFACE_GUID       "{c15b4308-04d3-11e6-b3ea-6057189e6443}"
#define MS_OS_PROP_NAME             "DeviceInterfaceGUIDs"
#define MS_OS_PROP_NAME_LEN         ((sizeof(MS_OS_PROP_NAME)) * 2)            // UTF-16 incl. NUL = 42
#define MS_OS_PROP_DATA_LEN         ((sizeof(CANDLE_INTERFACE_GUID) + 1) * 2)  // REG_MULTI_SZ: 2 NULs = 80
#define MS_OS_REG_PROP_LEN          (10 + MS_OS_PROP_NAME_LEN + MS_OS_PROP_DATA_LEN)
#define MS_OS_FUNCTION_LEN          (8 + 20 + MS_OS_REG_PROP_LEN)
#define MS_OS_CONFIG_LEN            (8 + CANDLE_CHANNEL_COUNT * MS_OS_FUNCTION_LEN)
#define MS_OS_DESC_LEN              (10 + MS_OS_CONFIG_LEN)

static uint8_t s_ms_os_desc[MS_OS_DESC_LEN];

static const uint8_t s_bos[] = {
    TUD_BOS_DESCRIPTOR(TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_DESC_LEN, VENDOR_REQUEST_MS_OS_20),
};

static uint8_t *put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return p + 2;
}

static uint8_t *put_utf16(uint8_t *p, const char *s, size_t chars_with_nul)
{
    for (size_t i = 0; i < chars_with_nul; i++) {
        p = put16(p, i < strlen(s) ? (uint8_t)s[i] : 0);
    }
    return p;
}

static void build_ms_os_desc(void)
{
    uint8_t *p = s_ms_os_desc;
    // Descriptor set header: Windows 8.1 and later
    p = put16(p, 10);
    p = put16(p, MS_OS_20_SET_HEADER_DESCRIPTOR);
    p = put16(p, 0x0000);
    p = put16(p, 0x0603);
    p = put16(p, MS_OS_DESC_LEN);
    // Configuration subset header
    p = put16(p, 8);
    p = put16(p, MS_OS_20_SUBSET_HEADER_CONFIGURATION);
    *p++ = 0;   // configuration index
    *p++ = 0;
    p = put16(p, MS_OS_CONFIG_LEN);

    for (int itf = 0; itf < CANDLE_CHANNEL_COUNT; itf++) {
        // Function subset header for one interface
        p = put16(p, 8);
        p = put16(p, MS_OS_20_SUBSET_HEADER_FUNCTION);
        *p++ = (uint8_t)itf;
        *p++ = 0;
        p = put16(p, MS_OS_FUNCTION_LEN);
        // Compatible ID: WINUSB
        p = put16(p, 20);
        p = put16(p, MS_OS_20_FEATURE_COMPATBLE_ID);
        memcpy(p, "WINUSB\0\0", 8);
        p += 8;
        memset(p, 0, 8);    // sub-compatible ID
        p += 8;
        // Registry property: DeviceInterfaceGUIDs (REG_MULTI_SZ = 7)
        p = put16(p, MS_OS_REG_PROP_LEN);
        p = put16(p, MS_OS_20_FEATURE_REG_PROPERTY);
        p = put16(p, 7);
        p = put16(p, MS_OS_PROP_NAME_LEN);
        p = put_utf16(p, MS_OS_PROP_NAME, MS_OS_PROP_NAME_LEN / 2);
        p = put16(p, MS_OS_PROP_DATA_LEN);
        p = put_utf16(p, CANDLE_INTERFACE_GUID, MS_OS_PROP_DATA_LEN / 2);
    }
    assert(p == s_ms_os_desc + sizeof(s_ms_os_desc));
}

/* ------------------------------------------------ TinyUSB callbacks ------------------------------------------------ */

uint8_t const *tud_descriptor_bos_cb(void)
{
    return s_bos;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    if (request->bRequest == VENDOR_REQUEST_MS_OS_20 && request->wIndex == 7 &&
            request->bmRequestType_bit.direction == TUSB_DIR_IN) {
        if (stage != CONTROL_STAGE_SETUP) {
            return true;
        }
        return tud_control_xfer(rhport, request, s_ms_os_desc, sizeof(s_ms_os_desc));
    }
    return candle_control_request(rhport, stage, request);
}

void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
{
    candle_usb_rx(itf, buffer, bufsize);
}

void tud_vendor_tx_cb(uint8_t itf, uint32_t sent_bytes)
{
    (void)sent_bytes;
    candle_usb_tx_done(itf);
}

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        candle_usb_mount_changed(true);
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        candle_usb_mount_changed(false);
    }
}

/* ------------------------------------------------ Console ------------------------------------------------ */

static int cmd_usb(int argc, char **argv)
{
    candle_print_status();
    return 0;
}

void register_candle_commands(void)
{
    const esp_console_cmd_t cmd = {
        .command = "usb",
        .help = "USB Candlelight adapter state (host connection, protocol, channels, IN FIFOs)",
        .func = cmd_usb,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ------------------------------------------------ Init ------------------------------------------------ */

esp_err_t candle_usb_init(void)
{
    static char serial[13];
    static const char *strings[STRID_COUNT];

    ESP_RETURN_ON_ERROR(candle_init(), TAG, "protocol init failed");
    build_ms_os_desc();

    // The USB serial identifies the adapter in cangaroo (adapter table): use the base MAC
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    strings[STRID_LANGID] = "\x09\x04";
    strings[STRID_MANUFACTURER] = "Espressif";
    strings[STRID_PRODUCT] = "ESP32-S31 CAN-FD 2ch";
    strings[STRID_SERIAL] = serial;
    strings[STRID_CAN1] = "CAN1";
    strings[STRID_CAN2] = "CAN2";

    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG(usb_event_cb, NULL);
    cfg.descriptor.device = &s_device;
    cfg.descriptor.qualifier = &s_qualifier;
    cfg.descriptor.string = strings;
    cfg.descriptor.string_count = STRID_COUNT;
    cfg.descriptor.full_speed_config = s_fs_config;
    cfg.descriptor.high_speed_config = s_hs_config;
    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&cfg), TAG, "TinyUSB install failed");

    ESP_LOGI(TAG, "USB Candlelight adapter on the HS OTG port: VID 0x303A PID 0x%04X, serial %s",
             CONFIG_CANDLE_USB_PID, serial);
    return ESP_OK;
}
