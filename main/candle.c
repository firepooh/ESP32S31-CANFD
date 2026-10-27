/*
 * Candlelight (gs_usb) and ElmueSoft CANable 2.5 protocol engine.
 *
 * The request handling, frame packing and error reporting follow control.c, buffer.c and
 * error.c of the CANable 2.5 firmware (MIT License, Copyright (c) 2025 ElmueSoft /
 * Hubert Denkmair), so hosts written for that firmware (cangaroo, Linux gs_usb, Elmue tools)
 * see the same behavior. The CAN side runs on canfd_port (ESP32-S31 TWAI-FD).
 *
 * Threads:
 * - TinyUSB task: SETUP requests, OUT data (CAN TX), IN transfer completion
 * - canN_rx tasks (canfd_port): received frames and TX echoes -> host FIFO
 * - candle task: error reports, TX timeout, bus load, identify LED, USB kick
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "hal/twaifd_ll.h"      // TWAI_LL_* bit timing limits of the TWAI-FD controller
#include "tusb.h"
#include "canfd_port.h"
#include "candle_def.h"
#include "candle.h"

static const char *TAG = "candle";

#define HOST_FIFO_DEPTH         CONFIG_CANDLE_HOST_FIFO_DEPTH
#define HOST_FIFO_RESERVE       8           // entries only error reports may use
#define HOST_MSG_MAX            sizeof(kHostFrameLegacy)    // 80, the largest single message
#define CANDLE_TASK_PERIOD_MS   5
#define CANDLE_TASK_STACK_SIZE  4096
#define CANDLE_TASK_PRIORITY    6
#define TX_TIMEOUT_US           500000      // pending TX without progress -> abort (as CAN_TX_TIMEOUT)
#define TX_SEND_TIMEOUT_MS      20          // wait for a free TX slot before reporting an overflow
#define AUTO_RECOVER_MS         10
#define ERR_REPORT_CHANGE_US    100000      // report a changed error state after 100 ms
#define ERR_REPORT_REPEAT_US    3000000     // repeat an unchanged error state every 3 s
#define LED_GPIO                60          // addressable RGB LED of ESP32-S31-Function-CoreBoard-1
#define LED_BLINK_US            250000

/* eUserFlags of the CANable 2.5 firmware (utils.h) */
#define USR_Retransmit      0x01
#define USR_TxEcho          0x02
#define USR_ReportESI       0x04
#define USR_ErrorReport     0x08
#define USR_DebugReport     0x10
#define USR_Timestamp       0x40
#define USR_SendBlobs       0x80
#define USR_CandleDefault   (USR_Retransmit | USR_TxEcho | USR_ErrorReport | USR_ReportESI)

/* eBusStatus of the CANable 2.5 firmware (settings.h) */
#define BUS_StatusActive    0x00
#define BUS_StatusWarning   0x10
#define BUS_StatusPassive   0x20
#define BUS_StatusOff       0x30

typedef enum {
    PROTO_ERR_NONE = 0,
    PROTO_ERR_ACK,
    PROTO_ERR_BIT,
    PROTO_ERR_STUFF,
    PROTO_ERR_FORM,
} proto_err_t;

typedef struct {
    uint8_t len;
    uint8_t data[HOST_MSG_MAX];
} host_msg_t;

/* One USB interface: FIFO of messages for the IN endpoint */
typedef struct {
    host_msg_t *fifo;
    uint32_t rd;
    uint32_t count;
    portMUX_TYPE lock;
    SemaphoreHandle_t tx_mutex;     // one sender builds the IN transfer at a time
    uint8_t tx_buf[MAX_BLOB_SIZE];  // stays untouched until the transfer is done (copied by TinyUSB)
    uint32_t sent_msgs;
    uint32_t overflows;
} usb_itf_t;

typedef struct {
    uint8_t bus_status;
    bool back_to_active;
    uint8_t tx_err;
    uint8_t rx_err;
    uint8_t last_proto_err;
    uint8_t app_flags;
} err_state_t;

typedef struct {
    volatile bool open;             // started by the host
    uint32_t user_flags;
    bool nom_set;
    bool data_set;
    twai_timing_advanced_config_t nom;
    twai_timing_advanced_config_t data;
    uint32_t nom_bitrate;
    uint32_t data_bitrate;
    canfd_mode_t mode;
    // error reporting
    err_state_t cur;
    err_state_t last;
    bool report_now;
    int64_t last_report_us;
    canfd_port_stats_t prev_stats;
    bool recovering;
    // TX timeout
    volatile uint32_t tx_queued;
    volatile uint32_t tx_done;
    volatile int64_t tx_progress_us;
    // bus load
    uint32_t busload_interval;      // in 100 ms, 0 = off
    uint32_t busload_ticks;
    volatile uint32_t busy_ns;      // bus time of the frames since the last report
    uint8_t old_busload;
    int64_t busload_next_us;
    // identify
    volatile bool identify;
} candle_ch_t;

static usb_itf_t s_itf[CANDLE_CHANNEL_COUNT];
static candle_ch_t s_ch[CANDLE_CHANNEL_COUNT];
static volatile bool s_proto_elmue;
static volatile bool s_mounted;
static eFeedback s_last_error = FBK_Success;
static SemaphoreHandle_t s_port_mutex;      // serializes port open/close against sends
static uint8_t s_ep0_buf[64];
static led_strip_handle_t s_led;

static kCapabilityClassic s_cap;
static kCapabilityFD s_cap_fd;
static kDeviceVersion s_version;
static kBoardInfo s_board;

/* ------------------------------------------------ Helpers ------------------------------------------------ */

static inline int ch_port(int ch)
{
    return ch + CANFD_PORT_FIRST;
}

static inline int itf_for_channel(int ch)
{
    // Elmue: each channel has its own interface. Legacy: everything goes through interface 0.
    return s_proto_elmue ? ch : 0;
}

static inline uint32_t timestamp_us(void)
{
    return (uint32_t)esp_timer_get_time();
}

static inline uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool any_channel_open(void)
{
    for (int c = 0; c < CANDLE_CHANNEL_COUNT; c++) {
        if (s_ch[c].open) {
            return true;
        }
    }
    return false;
}

static void error_assert(int ch, uint8_t app_flag, bool report_now)
{
    s_ch[ch].cur.app_flags |= app_flag;
    if (report_now) {
        s_ch[ch].report_now = true;
    }
}

/* Approximate bus time of a frame in ns (fixed fields only, stuffing is added by the bus load formula) */
static uint32_t frame_time_ns(const candle_ch_t *c, const twai_frame_header_t *h, uint8_t len)
{
    const uint32_t nom_ns = c->nom_bitrate ? 1000000000u / c->nom_bitrate : 0;
    const uint32_t data_ns = c->data_bitrate ? 1000000000u / c->data_bitrate : nom_ns;
    const uint32_t ext = h->ide ? 20 : 0;
    if (!h->fdf) {
        return (47 + ext + (h->rtr ? 0 : 8u * len)) * nom_ns;
    }
    const uint32_t crc = len > 16 ? 21 : 17;
    const uint32_t data_bits = 8u * len + crc + 10;
    if (h->brs) {
        return (31 + ext) * nom_ns + data_bits * data_ns;
    }
    return (31 + ext + data_bits) * nom_ns;
}

/* ------------------------------------------------ Host FIFO and IN endpoint ------------------------------------------------ */

static void fifo_clear(int itf)
{
    usb_itf_t *u = &s_itf[itf];
    portENTER_CRITICAL(&u->lock);
    u->rd = 0;
    u->count = 0;
    portEXIT_CRITICAL(&u->lock);
}

/* Frames leave the last HOST_FIFO_RESERVE entries free, so an error report still fits while frames overflow */
static bool fifo_push(int itf, const void *msg, uint8_t len, bool use_reserve)
{
    usb_itf_t *u = &s_itf[itf];
    const uint32_t limit = use_reserve ? HOST_FIFO_DEPTH : HOST_FIFO_DEPTH - HOST_FIFO_RESERVE;
    bool ok;
    portENTER_CRITICAL(&u->lock);
    ok = u->count < limit;
    if (ok) {
        host_msg_t *m = &u->fifo[(u->rd + u->count) % HOST_FIFO_DEPTH];
        m->len = len;
        memcpy(m->data, msg, len);
        u->count++;
    } else {
        u->overflows++;
    }
    portEXIT_CRITICAL(&u->lock);
    return ok;
}

/* Start the next IN transfer if the endpoint is free. Safe to call from any task. */
static void usb_kick(int itf)
{
    usb_itf_t *u = &s_itf[itf];
    if (!s_mounted || !tud_vendor_n_mounted(itf)) {
        return;
    }
    xSemaphoreTake(u->tx_mutex, portMAX_DELAY);
    if (tud_vendor_n_write_available(itf) == 0) {
        goto out;   // transfer in progress, candle_usb_tx_done() kicks again
    }

    uint32_t count;
    portENTER_CRITICAL(&u->lock);
    count = u->count;
    portEXIT_CRITICAL(&u->lock);
    if (count == 0) {
        goto out;
    }

    // Only this function removes entries, so [rd, rd + count) is stable while producers append
    const uint32_t mps = (tud_speed_get() == TUSB_SPEED_HIGH) ? 512 : 64;
    const host_msg_t *first = &u->fifo[u->rd];
    uint32_t n;
    uint32_t len;
    const bool blob = s_proto_elmue && (s_ch[itf].user_flags & USR_SendBlobs) && count > 1;
    if (!blob) {
        memcpy(u->tx_buf, first->data, first->len);
        len = first->len;
        n = 1;
    } else {
        kBlob *b = (kBlob *)u->tx_buf;
        uint8_t last_len = 0;
        len = sizeof(kBlob);
        n = 0;
        while (n < count && n < 250) {
            const host_msg_t *m = &u->fifo[(u->rd + n) % HOST_FIFO_DEPTH];
            if (len + m->len >= MAX_BLOB_SIZE) {
                break;
            }
            memcpy(u->tx_buf + len, m->data, m->len);
            len += m->len;
            last_len = m->len;
            n++;
        }
        // A transfer ending exactly on a packet boundary would need a zero length packet:
        // keep the last message for the next transfer instead
        if (len % mps == 0) {
            n--;
            len -= last_len;
        }
        b->frame_count = (uint8_t)n;
        b->msg_type = MSG_RxBlob;
    }

    if (tud_vendor_n_write(itf, u->tx_buf, len) == len) {
        portENTER_CRITICAL(&u->lock);
        u->rd = (u->rd + n) % HOST_FIFO_DEPTH;
        u->count -= n;
        portEXIT_CRITICAL(&u->lock);
        u->sent_msgs += n;
    }
out:
    xSemaphoreGive(u->tx_mutex);
}

static bool host_send_ex(int ch, const void *msg, uint8_t len, bool use_reserve)
{
    const int itf = itf_for_channel(ch);
    const bool ok = fifo_push(itf, msg, len, use_reserve);
    if (!ok) {
        error_assert(ch, APP_UsbInOverflow, false);
    }
    usb_kick(itf);
    return ok;
}

static void host_send(int ch, const void *msg, uint8_t len)
{
    host_send_ex(ch, msg, len, false);
}

static void send_debug_msg(int ch, const char *text)
{
    if (!(s_ch[ch].user_flags & USR_DebugReport)) {
        return;
    }
    uint8_t buf[HOST_MSG_MAX];
    size_t n = strnlen(text, HOST_MSG_MAX - sizeof(kHeader));
    buf[0] = (uint8_t)(sizeof(kHeader) + n);
    buf[1] = MSG_String;
    memcpy(buf + sizeof(kHeader), text, n);
    host_send(ch, buf, buf[0]);
}

/* ------------------------------------------------ Frames to the host ------------------------------------------------ */

/* Received frame (echo_id == ECHO_RxData) or legacy TX echo (echo_id = host value) */
static void store_frame(int ch, const canfd_msg_t *m, uint32_t echo_id, uint32_t ts)
{
    const candle_ch_t *c = &s_ch[ch];
    const twai_frame_header_t *h = &m->header;
    uint32_t can_id = h->ide ? ((h->id & CAN_MASK_29) | CAN_ID_29Bit) : (h->id & CAN_MASK_11);
    if (h->rtr) {
        can_id |= CAN_ID_RTR;
    }
    uint8_t flags = 0;
    if (h->fdf) {
        flags |= FRM_FDF;
        if (h->brs) {
            flags |= FRM_BRS;
        }
        if (h->esi) {
            flags |= FRM_ESI;
        }
    }
    const bool use_ts = c->user_flags & USR_Timestamp;

    if (s_proto_elmue) {
        uint8_t buf[HOST_MSG_MAX];
        uint8_t count;
        uint8_t pos = sizeof(kRxFrameElmue);
        buf[1] = MSG_RxFrame;
        buf[2] = flags;
        memcpy(buf + 3, &can_id, 4);
        if (use_ts) {
            memcpy(buf + pos, &ts, 4);
            pos += 4;
        }
        if (h->rtr) {
            buf[pos] = (uint8_t)h->dlc;     // remote frames send their DLC as the only data byte
            count = 1;
        } else {
            count = m->len;
            memcpy(buf + pos, m->data, count);
        }
        buf[0] = (uint8_t)(pos + count);
        host_send(ch, buf, buf[0]);
        return;
    }

    kHostFrameLegacy f = { 0 };
    f.echo_id = echo_id;
    f.can_id = can_id;
    f.can_dlc = (uint8_t)h->dlc;
    f.channel = (uint8_t)ch;
    f.flags = flags;
    if (!h->rtr) {
        memcpy(f.raw_data, m->data, m->len);
    }
    // The legacy layout puts the timestamp behind a fixed 8 or 64 byte data field
    uint8_t len = sizeof(kHostFrameLegacy);
    if (h->fdf) {
        f.pack_FD.timestamp_us = ts;
    } else {
        f.pack_classic.timestamp_us = ts;
        len -= 56;
    }
    if (!use_ts) {
        len -= 4;
    }
    host_send(ch, &f, len);
}

static void store_error(int ch)
{
    candle_ch_t *c = &s_ch[ch];
    const err_state_t *st = &c->cur;
    uint8_t data[8] = { 0 };
    uint32_t can_id = 0;

    switch (st->bus_status) {
    case BUS_StatusOff:
        can_id |= ERID_Bus_is_off;
        break;
    case BUS_StatusPassive:
        if (st->tx_err > 0) data[1] |= ER1_Tx_Passive_status_reached;
        if (st->rx_err > 0) data[1] |= ER1_Rx_Passive_status_reached;
        break;
    case BUS_StatusWarning:
        if (st->tx_err > 0) data[1] |= ER1_Tx_Errors_at_warning_level;
        if (st->rx_err > 0) data[1] |= ER1_Rx_Errors_at_warning_level;
        break;
    default:
        if (st->back_to_active) data[1] |= ER1_Bus_is_back_active;
        break;
    }
    switch (st->last_proto_err) {
    case PROTO_ERR_ACK:   can_id |= ERID_No_ACK_received;     break;
    case PROTO_ERR_BIT:   data[2] |= ER2_Single_bit_error;    break;
    case PROTO_ERR_STUFF: data[2] |= ER2_Bit_stuffing_error;  break;
    case PROTO_ERR_FORM:  data[2] |= ER2_Frame_format_error;  break;
    default: break;
    }
    if (!s_proto_elmue) {
        // Legacy hosts only know these flags, clone the application flags
        if (st->app_flags & APP_CanTxTimeout)  can_id  |= ERID_Tx_Timeout;
        if (st->app_flags & APP_UsbInOverflow) data[1] |= ER1_Rx_Buffer_Overflow;
        if (st->app_flags & APP_CanTxOverflow) data[1] |= ER1_Tx_Buffer_Overflow;
        if (data[1]) can_id |= ERID_Controller_problem;
        if (data[2]) can_id |= ERID_Protocol_violation;
    }
    data[5] = st->app_flags;
    data[6] = st->tx_err;
    data[7] = st->rx_err;

    const uint32_t ts = timestamp_us();
    const bool use_ts = c->user_flags & USR_Timestamp;
    bool sent;
    if (s_proto_elmue) {
        uint8_t buf[sizeof(kErrorElmue)];
        buf[0] = use_ts ? sizeof(kErrorElmue) : sizeof(kErrorElmue) - 4;
        buf[1] = MSG_Error;
        memcpy(buf + 2, &can_id, 4);
        memcpy(buf + 6, data, 8);
        memcpy(buf + 14, &ts, 4);
        sent = host_send_ex(ch, buf, buf[0], true);
    } else {
        kHostFrameLegacy f = { 0 };
        f.echo_id = ECHO_RxData;
        f.can_id = can_id | CAN_ID_Error;
        f.can_dlc = 8;
        f.channel = (uint8_t)ch;
        memcpy(f.pack_classic.data, data, 8);
        f.pack_classic.timestamp_us = ts;
        sent = host_send_ex(ch, &f, use_ts ? 24 : 20, true);
    }
    // A report that did not fit stays pending, so the flags are not lost
    if (sent) {
        memset(&c->cur, 0, sizeof(c->cur));
    }
}

/* ------------------------------------------------ canfd_port hooks (canN_rx task) ------------------------------------------------ */

static void on_rx_frame(int port, const canfd_msg_t *msg, void *arg)
{
    const int ch = (int)(intptr_t)arg;
    candle_ch_t *c = &s_ch[ch];
    if (!c->open) {
        return;
    }
    c->busy_ns += frame_time_ns(c, &msg->header, msg->len);
    store_frame(ch, msg, ECHO_RxData, (uint32_t)msg->header.timestamp);
}

static void on_tx_done(int port, const canfd_msg_t *msg, bool success, void *arg)
{
    const int ch = (int)(intptr_t)arg;
    candle_ch_t *c = &s_ch[ch];
    c->tx_done++;
    c->tx_progress_us = esp_timer_get_time();
    if (!c->open || !success) {
        return;
    }
    c->busy_ns += frame_time_ns(c, &msg->header, msg->len);

    if (s_proto_elmue) {
        const uint8_t marker = (uint8_t)msg->tag;
        if ((c->user_flags & USR_TxEcho) && marker > 0) {
            uint8_t buf[sizeof(kTxEchoElmue)];
            const uint32_t ts = timestamp_us();
            buf[0] = (c->user_flags & USR_Timestamp) ? sizeof(kTxEchoElmue) : sizeof(kTxEchoElmue) - 4;
            buf[1] = MSG_TxEcho;
            buf[2] = marker;
            memcpy(buf + 3, &ts, 4);
            host_send(ch, buf, buf[0]);
        }
    } else {
        // Legacy echo: the whole frame with the host's echo_id, sent after the frame was ACKed
        store_frame(ch, msg, msg->tag, timestamp_us());
    }
}

/* ------------------------------------------------ CAN TX from the host ------------------------------------------------ */

static void tx_frame(int ch, uint32_t can_id, uint8_t flags, uint8_t dlc_or_len, bool is_len,
                     const uint8_t *data, uint32_t data_avail, uint32_t tag)
{
    if (ch < 0 || ch >= CANDLE_CHANNEL_COUNT) {
        return;
    }
    candle_ch_t *c = &s_ch[ch];
    if (!c->open) {
        error_assert(ch, APP_CanTxFail, true);
        return;
    }

    canfd_msg_t m = { 0 };
    m.header.ide = (can_id & CAN_ID_29Bit) != 0;
    m.header.id = m.header.ide ? (can_id & CAN_MASK_29) : (can_id & CAN_MASK_11);
    m.header.rtr = (can_id & CAN_ID_RTR) != 0;
    m.tag = tag;

    uint32_t len;
    if (is_len) {
        len = dlc_or_len;
        if (len > TWAI_FRAME_MAX_LEN && !m.header.rtr) {
            flags |= FRM_FDF;
        }
    } else {
        if (dlc_or_len > 8) {
            flags |= FRM_FDF;
        }
        len = (flags & FRM_FDF) ? twaifd_dlc2len(dlc_or_len > 15 ? 15 : dlc_or_len)
                                : (dlc_or_len > 8 ? 8 : dlc_or_len);
    }
    if (flags & FRM_FDF) {
        if (!c->data_set || m.header.rtr) {
            error_assert(ch, APP_CanTxFail, true);  // CAN FD frame while the data bitrate is not set
            return;
        }
        m.header.fdf = 1;
        m.header.brs = (flags & FRM_BRS) != 0;
    }
    if (m.header.rtr) {
        m.len = (uint8_t)(len > 8 ? 8 : len);   // canfd_port sends len as the requested DLC
    } else {
        if (len > TWAIFD_FRAME_MAX_LEN) {
            len = TWAIFD_FRAME_MAX_LEN;
        }
        if (len > data_avail) {
            len = data_avail;
        }
        m.len = (uint8_t)len;
        memcpy(m.data, data, len);
    }

    xSemaphoreTake(s_port_mutex, portMAX_DELAY);
    esp_err_t err = c->open ? canfd_port_send(ch_port(ch), &m, TX_SEND_TIMEOUT_MS) : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_port_mutex);

    if (err == ESP_OK) {
        if (c->tx_queued == c->tx_done) {
            c->tx_progress_us = esp_timer_get_time();
        }
        c->tx_queued++;
    } else if (err == ESP_ERR_TIMEOUT) {
        error_assert(ch, APP_CanTxOverflow, true);
    } else {
        error_assert(ch, APP_CanTxFail, true);      // bus off, listen-only or invalid frame
    }
}

static void tx_elmue_frame(int ch, const uint8_t *p, uint32_t avail)
{
    if (avail < sizeof(kTxFrameElmue) || p[0] < sizeof(kTxFrameElmue) || p[0] > avail || p[1] != MSG_TxFrame) {
        error_assert(ch, APP_CanTxFail, true);
        return;
    }
    const uint32_t count = p[0] - sizeof(kTxFrameElmue);
    const uint32_t can_id = rd32(p + 3);
    if (can_id & CAN_ID_RTR) {
        // remote frame: the host may put the DLC into the first data byte
        tx_frame(ch, can_id, p[2], count ? p[8] : 0, true, NULL, 0, p[7]);
    } else {
        tx_frame(ch, can_id, p[2], (uint8_t)count, true, p + sizeof(kTxFrameElmue), count, p[7]);
    }
}

void candle_usb_rx(uint8_t itf, const uint8_t *buf, uint32_t len)
{
    if (!s_proto_elmue) {
        // Legacy: one kHostFrameLegacy per transfer, the channel is in the frame
        if (len < 12) {
            return;
        }
        kHostFrameLegacy f = { 0 };
        memcpy(&f, buf, len < sizeof(f) ? len : sizeof(f));
        tx_frame(f.channel, f.can_id, f.flags, f.can_dlc, false, f.raw_data,
                 len > 12 ? len - 12 : 0, f.echo_id);
        return;
    }

    const int ch = itf;
    if (len >= sizeof(kBlob) && buf[1] == MSG_TxBlob) {
        uint32_t off = sizeof(kBlob);
        for (uint8_t i = 0; i < buf[0] && off < len; i++) {
            const uint8_t size = buf[off];
            if (size == 0 || off + size > len) {
                error_assert(ch, APP_CanTxFail, true);
                return;
            }
            tx_elmue_frame(ch, buf + off, len - off);
            off += size;
        }
        return;
    }
    tx_elmue_frame(ch, buf, len);
}

void candle_usb_tx_done(uint8_t itf)
{
    usb_kick(itf);
}

/* ------------------------------------------------ Channel open / close ------------------------------------------------ */

static void ch_close(int ch)
{
    candle_ch_t *c = &s_ch[ch];
    xSemaphoreTake(s_port_mutex, portMAX_DELAY);
    c->open = false;
    if (canfd_port_is_open(ch_port(ch))) {
        canfd_port_close(ch_port(ch));
    }
    xSemaphoreGive(s_port_mutex);
}

static eFeedback ch_open(int ch)
{
    candle_ch_t *c = &s_ch[ch];
    const int port = ch_port(ch);

    canfd_port_config_t cfg;
    canfd_port_get_default_config(&cfg);
    cfg.timing_advanced = true;
    cfg.nominal_timing = c->nom;
    cfg.nominal_bitrate = c->nom_bitrate;
    cfg.nominal_sp_permill = 0;
    if (c->data_set) {
        cfg.data_timing = c->data;
        cfg.data_bitrate = c->data_bitrate;
    } else {
        cfg.data_bitrate = 0;
    }
    cfg.data_ssp_permill = 0;
    cfg.retry_limit = (c->user_flags & USR_Retransmit) ? -1 : 0;
    cfg.auto_recover_ms = AUTO_RECOVER_MS;
    cfg.mode = c->mode;

    xSemaphoreTake(s_port_mutex, portMAX_DELAY);
    if (canfd_port_is_open(port)) {
        canfd_port_close(port);     // opened by the console or left from an earlier session
    }
    canfd_port_set_rx_hook(port, on_rx_frame, (void *)(intptr_t)ch);
    canfd_port_set_tx_hook(port, on_tx_done, (void *)(intptr_t)ch);
    esp_err_t err = canfd_port_open(port, &cfg);
    if (err != ESP_OK) {
        canfd_port_set_rx_hook(port, NULL, NULL);
        canfd_port_set_tx_hook(port, NULL, NULL);
        xSemaphoreGive(s_port_mutex);
        ESP_LOGE(TAG, "CAN%d: open failed: %s", port, esp_err_to_name(err));
        return FBK_ErrorFromHAL;
    }
    canfd_port_set_errlog(port, false);     // errors go to the host, keep the console quiet
    memset(&c->cur, 0, sizeof(c->cur));
    memset(&c->last, 0, sizeof(c->last));
    memset(&c->prev_stats, 0, sizeof(c->prev_stats));
    c->report_now = false;
    c->recovering = false;
    c->tx_queued = c->tx_done = 0;
    c->busy_ns = 0;
    c->identify = false;
    c->open = true;
    xSemaphoreGive(s_port_mutex);

    ESP_LOGI(TAG, "CAN%d started by host: %" PRIu32 " / %" PRIu32 " bps, %s, %s%s", port,
             c->nom_bitrate, c->data_set ? c->data_bitrate : 0, canfd_mode_str(c->mode),
             s_proto_elmue ? "ElmueSoft" : "legacy", (c->user_flags & USR_Timestamp) ? ", timestamps" : "");
    char msg[64];
    snprintf(msg, sizeof(msg), "Nominal: %" PRIu32 " baud, Data: %" PRIu32 " baud", c->nom_bitrate,
             c->data_set ? c->data_bitrate : 0);
    send_debug_msg(ch, msg);
    return FBK_Success;
}

static void reset_all(void)
{
    for (int ch = 0; ch < CANDLE_CHANNEL_COUNT; ch++) {
        if (s_ch[ch].open) {
            ch_close(ch);
            ESP_LOGI(TAG, "CAN%d released by host", ch_port(ch));
        }
        s_ch[ch].identify = false;
    }
    s_proto_elmue = false;
    for (int itf = 0; itf < CANDLE_CHANNEL_COUNT; itf++) {
        fifo_clear(itf);
    }
}

void candle_usb_mount_changed(bool mounted)
{
    s_mounted = mounted;
    if (!mounted) {
        reset_all();
    }
    ESP_LOGI(TAG, "USB %s", mounted ? "connected" : "disconnected");
}

bool candle_channel_is_open(int channel)
{
    return channel >= 0 && channel < CANDLE_CHANNEL_COUNT && s_ch[channel].open;
}

/* ------------------------------------------------ SETUP requests ------------------------------------------------ */

/* GS bit timing (seg1 includes prop) -> TWAI-FD timing, checked against the controller limits */
static eFeedback set_bit_timing(int ch, bool data_phase, const kBitTiming *t)
{
    candle_ch_t *c = &s_ch[ch];
    if (c->open) {
        return FBK_AdapterMustBeClosed;
    }
    const kTimeMinMax *lim = data_phase ? &s_cap_fd.time_data : &s_cap_fd.time_nom;
    const uint32_t ph1_max = data_phase ? TWAI_LL_TSEG1_MAX_FD : TWAI_LL_TSEG1_MAX;
    const uint32_t seg1 = t->prop + t->seg1;
    if (t->brp < lim->brp_min || t->brp > lim->brp_max || seg1 < 1 || seg1 > lim->seg1_max ||
            t->seg2 < lim->seg2_min || t->seg2 > lim->seg2_max || t->sjw < 1 || t->sjw > lim->sjw_max) {
        return FBK_ParamOutOfRange;
    }

    twai_timing_advanced_config_t adv = {
        .brp = t->brp,
        .tseg_1 = (uint8_t)(seg1 < ph1_max ? seg1 : ph1_max),
        .tseg_2 = (uint8_t)t->seg2,
        .sjw = (uint8_t)(t->sjw < t->seg2 ? t->sjw : t->seg2),
    };
    adv.prop_seg = (uint8_t)(seg1 - adv.tseg_1);
    const uint32_t bitrate = CANFD_CLOCK_HZ / (t->brp * (1 + seg1 + t->seg2));

    if (data_phase) {
        c->data = adv;
        c->data_bitrate = bitrate;
        c->data_set = true;
    } else {
        c->nom = adv;
        c->nom_bitrate = bitrate;
        c->nom_set = true;
    }
    return FBK_Success;
}

static eFeedback set_device_mode(int ch, const kDeviceMode *dm)
{
    candle_ch_t *c = &s_ch[ch];
    if (dm->mode != GS_ModeStart && dm->mode != GS_ModeReset) {
        return FBK_InvalidParameter;
    }
    if (dm->mode == GS_ModeStart) {
        if (c->open) {
            return FBK_AdapterMustBeClosed;
        }
        if (!c->nom_set || ((dm->flags & GS_DevFlagCAN_FD) && !c->data_set)) {
            return FBK_BaudrateNotSet;
        }
    }

    // The protocol is global for all channels and can only change while all channels are closed
    const bool was_elmue = s_proto_elmue;
    if (!any_channel_open()) {
        s_proto_elmue = false;
    }
    if (dm->flags & ELM_DevFlagProtocolElmue) {
        s_proto_elmue = true;
    }
    if (s_proto_elmue != was_elmue) {
        for (int itf = 0; itf < CANDLE_CHANNEL_COUNT; itf++) {
            fifo_clear(itf);
        }
    }

    c->user_flags = USR_CandleDefault;
    if (dm->flags & GS_DevFlagOneShot) {
        c->user_flags &= ~USR_Retransmit;
    }
    if (dm->flags & GS_DevFlagTimestamp) {
        c->user_flags |= USR_Timestamp;
    }
    if (s_proto_elmue) {
        if (dm->flags & ELM_DevFlagSendUsbBlobs) {
            c->user_flags |= USR_SendBlobs;
        }
        for (int i = 0; i < CANDLE_CHANNEL_COUNT; i++) {
            s_ch[i].user_flags |= USR_DebugReport;
        }
    }

    if (dm->mode == GS_ModeReset) {
        if (c->open) {
            ch_close(ch);
            ESP_LOGI(TAG, "CAN%d stopped by host", ch_port(ch));
        }
        return FBK_Success;
    }

    c->mode = CANFD_MODE_NORMAL;
    if (dm->flags & GS_DevFlagListenOnly) {
        c->mode = (dm->flags & GS_DevFlagLoopback) ? CANFD_MODE_SELF_TEST : CANFD_MODE_LISTEN_ONLY;
    } else if (dm->flags & GS_DevFlagLoopback) {
        c->mode = CANFD_MODE_LOOPBACK;
    }
    return ch_open(ch);
}

static void get_error_state(int ch, kErrorState *es)
{
    memset(es, 0, sizeof(*es));
    twai_node_status_t st;
    if (!s_ch[ch].open || canfd_port_get_status(ch_port(ch), &st, NULL) != ESP_OK) {
        es->state = GS_Stopped;
        return;
    }
    es->rx_err = st.rx_error_count;
    es->tx_err = st.tx_error_count;
    switch (st.state) {
    case TWAI_ERROR_WARNING: es->state = GS_ErrorWarning; break;
    case TWAI_ERROR_PASSIVE: es->state = GS_ErrorPassive; break;
    case TWAI_ERROR_BUS_OFF: es->state = GS_BusOff;       break;
    default:                 es->state = GS_BusActive;    break;
    }
}

/* SETUP stage: answer IN requests, accept the data of OUT requests (executed in the DATA stage) */
static bool control_setup(uint8_t rhport, const tusb_control_request_t *req)
{
    static union {
        kErrorState err_state;
        uint32_t u32;
        uint8_t u8;
    } resp;

    if (req->bRequest == ELM_ReqGetLastError && req->bmRequestType_bit.direction == TUSB_DIR_IN) {
        resp.u8 = (uint8_t)s_last_error;
        return tud_control_xfer(rhport, req, &resp.u8, 1);
    }
    if (req->bRequest >= ELM_ReqFIRST && req->bRequest <= ELM_ReqWriteFlash && !s_proto_elmue) {
        s_last_error = FBK_InvalidCommand;
        return false;
    }

    int ch;
    switch (req->bRequest) {
    case GS_ReqSetHostFormat:
    case GS_ReqGetDeviceVersion:
    case GS_ReqGetTimestamp:
    case ELM_ReqGetBoardInfo:
        // per-device requests: wValue is ignored (Linux sends 1)
        ch = req->wValue < CANDLE_CHANNEL_COUNT ? req->wValue : CANDLE_CHANNEL_COUNT - 1;
        break;
    default:
        ch = req->wValue;
        if (ch >= CANDLE_CHANNEL_COUNT) {
            s_last_error = FBK_InvalidParameter;
            return false;
        }
        break;
    }
    s_last_error = FBK_Success;

    if (req->bmRequestType_bit.direction == TUSB_DIR_OUT) {
        uint16_t min_len;
        switch (req->bRequest) {
        case GS_ReqSetHostFormat:
        case GS_ReqIdentify:
        case GS_ReqSetTermination:
            min_len = 4;
            break;
        case GS_ReqSetBitTiming:
        case GS_ReqSetBitTimingFD:
            min_len = sizeof(kBitTiming);
            break;
        case GS_ReqSetDeviceMode:
            min_len = sizeof(kDeviceMode);
            break;
        case ELM_ReqSetBusLoadReport:
            min_len = 1;
            break;
        case GS_ReqSetTdc:
        case GS_ReqBusOffRecovery:
        case ELM_ReqSetFilter:
        case ELM_ReqSetPinStatus:
            min_len = 0;
            break;
        default:
            s_last_error = FBK_InvalidCommand;
            return false;
        }
        if (req->wLength < min_len || req->wLength > sizeof(s_ep0_buf)) {
            s_last_error = FBK_InvalidParameter;
            return false;
        }
        if (req->wLength == 0) {
            return tud_control_status(rhport, req);
        }
        return tud_control_xfer(rhport, req, s_ep0_buf, req->wLength);
    }

    const void *src;
    uint16_t len;
    switch (req->bRequest) {
    case GS_ReqGetCapabilities:
        src = &s_cap;
        len = sizeof(s_cap);
        break;
    case GS_ReqGetCapabilitiesFD:
        src = &s_cap_fd;
        len = sizeof(s_cap_fd);
        break;
    case GS_ReqGetDeviceVersion:
        src = &s_version;
        len = sizeof(s_version);
        break;
    case GS_ReqGetTimestamp:
        resp.u32 = timestamp_us();
        src = &resp.u32;
        len = 4;
        break;
    case GS_ReqGetErrorState:
        get_error_state(ch, &resp.err_state);
        src = &resp.err_state;
        len = sizeof(resp.err_state);
        break;
    case ELM_ReqGetBoardInfo:
        src = &s_board;
        len = sizeof(s_board);
        break;
    case GS_ReqGetTermination:
    case ELM_ReqGetPinStatus:
    case ELM_ReqReadFlash:
        s_last_error = FBK_UnsupportedFeature;
        return false;
    default:
        s_last_error = FBK_InvalidCommand;
        return false;
    }
    return tud_control_xfer(rhport, req, (void *)src, len < req->wLength ? len : req->wLength);
}

/* DATA stage of OUT requests: the host data is in s_ep0_buf */
static void control_out_data(const tusb_control_request_t *req)
{
    const int ch = req->wValue < CANDLE_CHANNEL_COUNT ? req->wValue : 0;
    switch (req->bRequest) {
    case GS_ReqSetHostFormat: {
        uint32_t format;
        memcpy(&format, s_ep0_buf, 4);
        if (format != 0xbeef) {
            s_last_error = FBK_UnsupportedFeature;   // only little endian
        }
        break;
    }
    case GS_ReqSetBitTiming:
    case GS_ReqSetBitTimingFD: {
        kBitTiming t;
        memcpy(&t, s_ep0_buf, sizeof(t));
        s_last_error = set_bit_timing(ch, req->bRequest == GS_ReqSetBitTimingFD, &t);
        break;
    }
    case GS_ReqSetDeviceMode: {
        kDeviceMode dm;
        memcpy(&dm, s_ep0_buf, sizeof(dm));
        s_last_error = set_device_mode(ch, &dm);
        break;
    }
    case GS_ReqIdentify: {
        uint32_t mode;
        memcpy(&mode, s_ep0_buf, 4);
        s_ch[ch].identify = mode != 0;
        break;
    }
    case GS_ReqSetTdc:
        // The TWAI-FD transmitter delay compensation is on by default (measured delay + offset)
        break;
    case GS_ReqBusOffRecovery:
        if (s_ch[ch].open) {
            canfd_port_recover(ch_port(ch), 0);     // starts the recovery, finishing is reported by errors
        }
        break;
    case ELM_ReqSetBusLoadReport: {
        const uint8_t interval = s_ep0_buf[0];
        if (interval > 100) {
            s_last_error = FBK_ParamOutOfRange;
        } else {
            s_ch[ch].busload_interval = interval;
            s_ch[ch].busload_next_us = 0;
        }
        break;
    }
    case GS_ReqSetTermination:
    case ELM_ReqSetFilter:
    case ELM_ReqSetPinStatus:
    default:
        s_last_error = FBK_UnsupportedFeature;
        break;
    }
}

bool candle_control_request(uint8_t rhport, uint8_t stage, const tusb_control_request_t *req)
{
    if (stage == CONTROL_STAGE_SETUP) {
        return control_setup(rhport, req);
    }
    if (stage == CONTROL_STAGE_DATA && req->bmRequestType_bit.direction == TUSB_DIR_OUT) {
        control_out_data(req);
    }
    return true;
}

/* ------------------------------------------------ Background task ------------------------------------------------ */

/* Same rules as error_is_report_due() of the CANable 2.5 firmware */
static void ch_update_errors(int ch, int64_t now)
{
    candle_ch_t *c = &s_ch[ch];
    twai_node_status_t st;
    canfd_port_stats_t s;
    if (canfd_port_get_status(ch_port(ch), &st, &s) != ESP_OK) {
        return;
    }
    err_state_t *cur = &c->cur;
    const canfd_port_stats_t *p = &c->prev_stats;

    switch (st.state) {
    case TWAI_ERROR_WARNING: cur->bus_status = BUS_StatusWarning; break;
    case TWAI_ERROR_PASSIVE: cur->bus_status = BUS_StatusPassive; break;
    case TWAI_ERROR_BUS_OFF: cur->bus_status = BUS_StatusOff;     break;
    default: break;
    }
    // A bus-off with a fast automatic recovery may be over before this poll
    const bool new_bus_off = s.bus_off != p->bus_off;
    if (new_bus_off) {
        cur->bus_status = BUS_StatusOff;
    }
    if (cur->bus_status == BUS_StatusActive && c->last.bus_status != BUS_StatusActive) {
        cur->back_to_active = true;
    }
    cur->tx_err = st.tx_error_count > 255 ? 255 : (uint8_t)st.tx_error_count;
    cur->rx_err = st.rx_error_count > 255 ? 255 : (uint8_t)st.rx_error_count;

    if (cur->last_proto_err == PROTO_ERR_NONE) {
        if (s.ack_err != p->ack_err) {
            cur->last_proto_err = PROTO_ERR_ACK;
        } else if (s.bit_err != p->bit_err) {
            cur->last_proto_err = PROTO_ERR_BIT;
        } else if (s.stuff_err != p->stuff_err) {
            cur->last_proto_err = PROTO_ERR_STUFF;
        } else if (s.form_err != p->form_err) {
            cur->last_proto_err = PROTO_ERR_FORM;
        }
    }
    if (s.rx_drop != p->rx_drop) {
        cur->app_flags |= APP_CanRxFail;
    }
    c->prev_stats = s;

    // Debug messages around the bus-off recovery (ElmueSoft protocol only)
    if (new_bus_off && !c->recovering) {
        c->recovering = true;
        send_debug_msg(ch, ">> Start recovery from Bus Off");
    } else if (c->recovering && st.state != TWAI_ERROR_BUS_OFF && !new_bus_off) {
        c->recovering = false;
        send_debug_msg(ch, "<< Successfully recovered from Bus Off");
    }

    bool due = false;
    const int64_t elapsed = now - c->last_report_us;
    const err_state_t empty = { 0 };
    if (c->report_now) {
        c->report_now = false;
        due = true;
    } else if (memcmp(cur, &empty, sizeof(empty)) == 0) {
        return;     // no errors present
    } else if (cur->bus_status == BUS_StatusOff && c->last.bus_status != BUS_StatusOff) {
        due = true;
    } else if (elapsed >= ERR_REPORT_CHANGE_US && memcmp(cur, &c->last, sizeof(*cur)) != 0) {
        due = true;
    } else if (elapsed >= ERR_REPORT_REPEAT_US) {
        due = true;
    }
    if (!due) {
        return;
    }
    c->last_report_us = now;
    c->last = *cur;
    store_error(ch);
}

/* Frames that are not ACKed are repeated forever: abort them like the CANable 2.5 firmware */
static void ch_check_tx_timeout(int ch, int64_t now)
{
    candle_ch_t *c = &s_ch[ch];
    if (c->tx_queued == c->tx_done || now - c->tx_progress_us < TX_TIMEOUT_US) {
        return;
    }
    ESP_LOGW(TAG, "CAN%d: TX not acknowledged for %d ms, pending frames dropped", ch_port(ch), TX_TIMEOUT_US / 1000);
    ch_close(ch);
    ch_open(ch);
    error_assert(ch, APP_CanTxTimeout, false);
}

static void ch_update_busload(int ch, int64_t now)
{
    candle_ch_t *c = &s_ch[ch];
    if (c->busload_interval == 0) {
        return;
    }
    if (c->busload_next_us == 0) {
        c->busload_next_us = now + (int64_t)c->busload_interval * 100000;
        c->busy_ns = 0;
        return;
    }
    if (now < c->busload_next_us) {
        return;
    }
    const uint64_t window_ns = (uint64_t)c->busload_interval * 100000000ull;
    // +12.5 % for stuff bits, as STUFFING_FACTOR of the CANable 2.5 firmware
    uint64_t pct = (uint64_t)c->busy_ns * 1125 / window_ns / 10;
    c->busy_ns = 0;
    c->busload_next_us = now + (int64_t)c->busload_interval * 100000;
    if (pct > 100) {
        pct = 100;
    }
    if (s_proto_elmue && (pct > 0 || c->old_busload > 0)) {
        uint8_t buf[sizeof(kBusloadElmue)] = { sizeof(kBusloadElmue), MSG_Busload, (uint8_t)pct };
        host_send(ch, buf, sizeof(buf));
    }
    c->old_busload = (uint8_t)pct;
}

static void led_update(int64_t now)
{
    static bool shown;
    if (!s_led) {
        return;
    }
    const bool on = (now / LED_BLINK_US) & 1;
    const int phase = (int)((now / (2 * LED_BLINK_US)) % CANDLE_CHANNEL_COUNT);
    bool any = false;
    for (int ch = 0; ch < CANDLE_CHANNEL_COUNT; ch++) {
        any |= s_ch[ch].identify;
    }
    if (!any) {
        if (shown) {
            led_strip_clear(s_led);
            shown = false;
        }
        return;
    }
    // CAN1 blinks blue, CAN2 green; with both active they alternate
    int ch = s_ch[phase].identify ? phase : (s_ch[0].identify ? 0 : 1);
    if (on) {
        led_strip_set_pixel(s_led, 0, 0, ch == 1 ? 40 : 0, ch == 0 ? 40 : 0);
        led_strip_refresh(s_led);
    } else {
        led_strip_clear(s_led);
    }
    shown = true;
}

static void candle_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(CANDLE_TASK_PERIOD_MS));
        const int64_t now = esp_timer_get_time();
        for (int ch = 0; ch < CANDLE_CHANNEL_COUNT; ch++) {
            if (!s_ch[ch].open) {
                continue;
            }
            ch_update_errors(ch, now);
            ch_check_tx_timeout(ch, now);
            ch_update_busload(ch, now);
        }
        for (int itf = 0; itf < CANDLE_CHANNEL_COUNT; itf++) {
            usb_kick(itf);  // safety net, normally the producers and the IN completion kick
        }
        led_update(now);
    }
}

/* ------------------------------------------------ Init / status ------------------------------------------------ */

static void init_led(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_led) != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED on GPIO%d not available, identify has no effect", LED_GPIO);
        s_led = NULL;
        return;
    }
    led_strip_clear(s_led);
}

static void init_descriptors(void)
{
    const uint32_t features = GS_DevFlagListenOnly | GS_DevFlagLoopback | GS_DevFlagOneShot | GS_DevFlagTimestamp |
                              GS_DevFlagIdentify | GS_DevFlagCAN_FD | GS_DevFlagBitTimingFD | GS_DevFlagGetErrorState |
                              ELM_DevFlagProtocolElmue | ELM_DevFlagSendUsbBlobs;
    const kTimeMinMax nom = {
        .seg1_min = 1, .seg1_max = TWAI_LL_PROP_MAX + TWAI_LL_TSEG1_MAX,
        .seg2_min = 1, .seg2_max = TWAI_LL_TSEG2_MAX,
        .sjw_max = TWAI_LL_SJW_MAX,
        .brp_min = 1, .brp_max = TWAI_LL_BRP_MAX, .brp_inc = 1,
    };
    const kTimeMinMax data = {
        .seg1_min = 1, .seg1_max = TWAI_LL_PROP_MAX_FD + TWAI_LL_TSEG1_MAX_FD,
        .seg2_min = 1, .seg2_max = TWAI_LL_TSEG2_MAX_FD,
        .sjw_max = TWAI_LL_SJW_MAX_FD,
        .brp_min = 1, .brp_max = TWAI_LL_BRP_MAX_FD, .brp_inc = 1,
    };
    s_cap = (kCapabilityClassic) { .feature = features, .fclk_can = CANFD_CLOCK_HZ, .time = nom };
    s_cap_fd = (kCapabilityFD) { .feature = features, .fclk_can = CANFD_CLOCK_HZ, .time_nom = nom, .time_data = data };

    s_version = (kDeviceVersion) {
        .hal_ver_high = 6, .hal_ver_mid = 1, .hal_ver_low = 0,  // ESP-IDF v6.1
        .icount = CANDLE_CHANNEL_COUNT - 1,
        .sw_version_bcd = CANDLE_FW_VERSION_BCD,
        .hw_version_bcd = CANDLE_HW_VERSION_BCD,
    };
    s_board = (kBoardInfo) { .McuDeviceID = 0x5331, .BoardFlags = BRD_Quartz_In_Use | BRD_USB_HighSpeed };
    strlcpy(s_board.McuName, "ESP32-S31", sizeof(s_board.McuName));
    strlcpy(s_board.BoardName, "ESP32-S31 CoreBoard", sizeof(s_board.BoardName));
}

esp_err_t candle_init(void)
{
    s_port_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_port_mutex, ESP_ERR_NO_MEM, TAG, "no mem");
    for (int itf = 0; itf < CANDLE_CHANNEL_COUNT; itf++) {
        usb_itf_t *u = &s_itf[itf];
        // Only tasks touch the host FIFO, so it may live in PSRAM
        u->fifo = heap_caps_calloc(HOST_FIFO_DEPTH, sizeof(host_msg_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!u->fifo) {
            u->fifo = heap_caps_calloc(HOST_FIFO_DEPTH, sizeof(host_msg_t), MALLOC_CAP_8BIT);
        }
        u->tx_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(u->fifo && u->tx_mutex, ESP_ERR_NO_MEM, TAG, "no mem for host FIFO");
        portMUX_INITIALIZE(&u->lock);
    }
    for (int ch = 0; ch < CANDLE_CHANNEL_COUNT; ch++) {
        s_ch[ch].user_flags = USR_CandleDefault;
    }
    init_descriptors();
    init_led();
    ESP_RETURN_ON_FALSE(xTaskCreate(candle_task, "candle", CANDLE_TASK_STACK_SIZE, NULL, CANDLE_TASK_PRIORITY, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "no mem for task");
    return ESP_OK;
}

void candle_print_status(void)
{
    printf("USB: %s, %s speed, protocol %s\n", s_mounted ? "connected" : "not connected",
           tud_speed_get() == TUSB_SPEED_HIGH ? "high" : "full", s_proto_elmue ? "ElmueSoft" : "legacy");
    for (int ch = 0; ch < CANDLE_CHANNEL_COUNT; ch++) {
        const candle_ch_t *c = &s_ch[ch];
        const usb_itf_t *u = &s_itf[ch];
        printf("  CAN%d: %s", ch_port(ch), c->open ? "started by host" : "closed");
        if (c->open) {
            printf(", %" PRIu32 " / %" PRIu32 " bps, %s, tx queued %" PRIu32 " done %" PRIu32,
                   c->nom_bitrate, c->data_set ? c->data_bitrate : 0, canfd_mode_str(c->mode), c->tx_queued, c->tx_done);
        }
        printf("\n        USB IN interface %d: fifo %" PRIu32 "/%d, sent %" PRIu32 ", overflow %" PRIu32 "\n",
               ch, u->count, HOST_FIFO_DEPTH, u->sent_msgs, u->overflows);
    }
}
