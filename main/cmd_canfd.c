/*
 * Console commands for the CAN-FD 2-port test.
 *
 *   can_init     open / re-open ports with bitrate and mode settings
 *   can_deinit   close ports
 *   can_send     send one or more frames
 *   can_dump     print received frames
 *   can_status   show state, error counters and statistics
 *   can_recover  recover a port from bus-off
 *   can_test     CAN1 <-> CAN2 transfer test with payload verification
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "canfd_port.h"
#include "cmd_canfd.h"

#define SEND_TIMEOUT_MS             1000
#define TEST_SENDER_STACK_SIZE      4096
#define TEST_SENDER_PRIORITY        2
#define TEST_DEFAULT_COUNT          100
#define TEST_DEFAULT_LEN            64
#define TEST_DEFAULT_ID             0x100

/* ------------------------------------------------ Parsing helpers ------------------------------------------------ */

/* "1", "2", "can1", "CAN2" or "all" (NULL means all) */
static bool parse_port_range(const char *s, int *first, int *last)
{
    if (s == NULL || strcasecmp(s, "all") == 0) {
        *first = CANFD_PORT_FIRST;
        *last = CANFD_PORT_LAST;
        return true;
    }
    if (strncasecmp(s, "can", 3) == 0) {
        s += 3;
    }
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || !canfd_port_is_valid((int)v)) {
        return false;
    }
    *first = *last = (int)v;
    return true;
}

static bool parse_port(const char *s, int *port)
{
    int first, last;
    if (s == NULL || !parse_port_range(s, &first, &last) || first != last) {
        return false;
    }
    *port = first;
    return true;
}

static bool parse_hex_u32(const char *s, uint32_t *val)
{
    char *end;
    if (s == NULL || *s == '\0') {
        return false;
    }
    *val = strtoul(s, &end, 16);
    return *end == '\0';
}

/* Hex payload such as "1122AABB", "11.22.AA.BB" or "11:22:AA:BB". Returns the length or -1. */
static int parse_hex_data(const char *s, uint8_t *buf, int max_len)
{
    int len = 0;
    while (*s) {
        if (*s == '.' || *s == ':' || *s == '-' || *s == '_') {
            s++;
            continue;
        }
        if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1]) || len >= max_len) {
            return -1;
        }
        char byte[3] = { s[0], s[1], '\0' };
        buf[len++] = (uint8_t)strtoul(byte, NULL, 16);
        s += 2;
    }
    return len;
}

static bool parse_mode(const char *s, canfd_mode_t *mode)
{
    if (strcasecmp(s, "normal") == 0) {
        *mode = CANFD_MODE_NORMAL;
    } else if (strcasecmp(s, "listen") == 0) {
        *mode = CANFD_MODE_LISTEN_ONLY;
    } else if (strcasecmp(s, "selftest") == 0) {
        *mode = CANFD_MODE_SELF_TEST;
    } else {
        return false;
    }
    return true;
}

static void print_port_line(int port)
{
    int tx, rx;
    canfd_port_get_pins(port, &tx, &rx);
    const canfd_port_config_t *cfg = canfd_port_get_config(port);
    if (cfg == NULL) {
        printf("CAN%d  TX=GPIO%d RX=GPIO%d  (not initialized)\n", port, tx, rx);
        return;
    }
    printf("CAN%d  TX=GPIO%d RX=GPIO%d  mode=%s  nominal=%" PRIu32 " bps  data=", port, tx, rx,
           canfd_mode_str(cfg->mode), cfg->nominal_bitrate);
    if (cfg->data_bitrate) {
        printf("%" PRIu32 " bps", cfg->data_bitrate);
    } else {
        printf("off (classic)");
    }
    printf("  retry=%d  auto_recover=", cfg->retry_limit);
    if (cfg->auto_recover_ms) {
        printf("%" PRIu32 " ms", cfg->auto_recover_ms);
    } else {
        printf("off");
    }
    printf("  dump=%s\n", canfd_port_get_dump(port) ? "on" : "off");
}

void canfd_open_all_default(void)
{
    canfd_port_config_t cfg;
    canfd_port_get_default_config(&cfg);
    for (int port = CANFD_PORT_FIRST; port <= CANFD_PORT_LAST; port++) {
        esp_err_t err = canfd_port_open(port, &cfg);
        if (err != ESP_OK) {
            printf("CAN%d init failed: %s\n", port, esp_err_to_name(err));
            continue;
        }
        print_port_line(port);
    }
}

/* ------------------------------------------------ can_init ------------------------------------------------ */

static struct {
    struct arg_str *port;
    struct arg_int *bitrate;
    struct arg_int *dbitrate;
    struct arg_int *sp;
    struct arg_int *dsp;
    struct arg_int *dssp;
    struct arg_int *retry;
    struct arg_int *autorec;
    struct arg_str *mode;
    struct arg_lit *classic;
    struct arg_end *end;
} s_init_args;

static int cmd_can_init(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_init_args) != 0) {
        arg_print_errors(stderr, s_init_args.end, argv[0]);
        return 1;
    }
    int first, last;
    if (!parse_port_range(s_init_args.port->count ? s_init_args.port->sval[0] : NULL, &first, &last)) {
        printf("Invalid port, use 1, 2 or all\n");
        return 1;
    }

    canfd_port_config_t cfg;
    canfd_port_get_default_config(&cfg);
    if (s_init_args.bitrate->count) {
        cfg.nominal_bitrate = s_init_args.bitrate->ival[0];
    }
    if (s_init_args.dbitrate->count) {
        cfg.data_bitrate = s_init_args.dbitrate->ival[0];
    }
    if (s_init_args.classic->count) {
        cfg.data_bitrate = 0;
    }
    if (s_init_args.sp->count) {
        cfg.nominal_sp_permill = s_init_args.sp->ival[0];
    }
    if (s_init_args.dsp->count) {
        cfg.data_sp_permill = s_init_args.dsp->ival[0];
    }
    if (s_init_args.dssp->count) {
        cfg.data_ssp_permill = s_init_args.dssp->ival[0];
    }
    if (s_init_args.retry->count) {
        int retry = s_init_args.retry->ival[0];
        if (retry < -1 || retry > 15) {
            printf("Retry limit must be -1..15\n");
            return 1;
        }
        cfg.retry_limit = (int8_t)retry;
    }
    if (s_init_args.autorec->count) {
        if (s_init_args.autorec->ival[0] < 0) {
            printf("Auto recovery delay must be >= 0\n");
            return 1;
        }
        cfg.auto_recover_ms = (uint32_t)s_init_args.autorec->ival[0];
    }
    if (s_init_args.mode->count && !parse_mode(s_init_args.mode->sval[0], &cfg.mode)) {
        printf("Invalid mode, use normal, listen or selftest\n");
        return 1;
    }
    if (cfg.nominal_bitrate == 0) {
        printf("Nominal bitrate must not be 0\n");
        return 1;
    }

    int ret = 0;
    for (int port = first; port <= last; port++) {
        if (canfd_port_is_open(port)) {
            canfd_port_close(port);
        }
        esp_err_t err = canfd_port_open(port, &cfg);
        if (err != ESP_OK) {
            printf("CAN%d init failed: %s\n", port, esp_err_to_name(err));
            ret = 1;
            continue;
        }
        print_port_line(port);
    }
    return ret;
}

/* ------------------------------------------------ can_deinit ------------------------------------------------ */

static struct {
    struct arg_str *port;
    struct arg_end *end;
} s_deinit_args;

static int cmd_can_deinit(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_deinit_args) != 0) {
        arg_print_errors(stderr, s_deinit_args.end, argv[0]);
        return 1;
    }
    int first, last;
    if (!parse_port_range(s_deinit_args.port->count ? s_deinit_args.port->sval[0] : NULL, &first, &last)) {
        printf("Invalid port, use 1, 2 or all\n");
        return 1;
    }
    for (int port = first; port <= last; port++) {
        if (canfd_port_is_open(port)) {
            canfd_port_close(port);
            printf("CAN%d closed\n", port);
        }
    }
    return 0;
}

/* ------------------------------------------------ can_send ------------------------------------------------ */

static struct {
    struct arg_str *port;
    struct arg_str *id;
    struct arg_str *data;
    struct arg_lit *ext;
    struct arg_lit *fd;
    struct arg_lit *brs;
    struct arg_lit *rtr;
    struct arg_int *len;
    struct arg_int *count;
    struct arg_int *period;
    struct arg_end *end;
} s_send_args;

static int cmd_can_send(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_send_args) != 0) {
        arg_print_errors(stderr, s_send_args.end, argv[0]);
        return 1;
    }
    int port;
    if (!parse_port(s_send_args.port->sval[0], &port)) {
        printf("Invalid port, use 1 or 2\n");
        return 1;
    }
    const canfd_port_config_t *cfg = canfd_port_get_config(port);
    if (cfg == NULL) {
        printf("CAN%d is not initialized, run can_init first\n", port);
        return 1;
    }

    canfd_msg_t msg = { 0 };
    uint32_t id;
    if (!parse_hex_u32(s_send_args.id->sval[0], &id)) {
        printf("Invalid ID '%s' (hex expected)\n", s_send_args.id->sval[0]);
        return 1;
    }
    msg.header.ide = s_send_args.ext->count || id > TWAI_STD_ID_MASK;
    if (id > (msg.header.ide ? TWAI_EXT_ID_MASK : TWAI_STD_ID_MASK)) {
        printf("ID 0x%" PRIX32 " out of range\n", id);
        return 1;
    }
    msg.header.id = id;
    msg.header.brs = s_send_args.brs->count > 0;
    msg.header.fdf = s_send_args.fd->count || msg.header.brs;
    msg.header.rtr = s_send_args.rtr->count > 0;
    if (msg.header.rtr && msg.header.fdf) {
        printf("Remote frames are classic CAN only\n");
        return 1;
    }
    if (msg.header.brs && cfg->data_bitrate == 0) {
        printf("CAN%d has no data bitrate (classic), BRS not possible\n", port);
        return 1;
    }

    const int max_len = msg.header.fdf ? TWAIFD_FRAME_MAX_LEN : TWAI_FRAME_MAX_LEN;
    int len = 0;
    if (s_send_args.data->count) {
        len = parse_hex_data(s_send_args.data->sval[0], msg.data, max_len);
        if (len < 0) {
            printf("Invalid payload or longer than %d bytes\n", max_len);
            return 1;
        }
    }
    if (s_send_args.len->count) {
        int req = s_send_args.len->ival[0];
        if (req < 0 || req > max_len) {
            printf("Length must be 0..%d\n", max_len);
            return 1;
        }
        if (!s_send_args.data->count) {
            for (int i = 0; i < req; i++) {
                msg.data[i] = (uint8_t)i;
            }
        }
        len = req;  // longer than the given data: zero padded, shorter: truncated
    }
    msg.len = (uint8_t)len;
    if (msg.header.fdf && canfd_round_fd_len(msg.len) != msg.len) {
        printf("Note: FD length %u is padded to %u\n", msg.len, canfd_round_fd_len(msg.len));
    }

    const int count = s_send_args.count->count ? s_send_args.count->ival[0] : 1;
    const int period = s_send_args.period->count ? s_send_args.period->ival[0] : 0;
    if (count < 1 || period < 0) {
        printf("Count must be >= 1 and period >= 0\n");
        return 1;
    }
    canfd_port_stats_t before, after;
    twai_node_status_t st;
    canfd_port_get_status(port, NULL, &before);

    int queued = 0;
    for (; queued < count; queued++) {
        esp_err_t err = canfd_port_send(port, &msg, SEND_TIMEOUT_MS);
        if (err != ESP_OK) {
            printf("CAN%d send #%d failed: %s\n", port, queued + 1, esp_err_to_name(err));
            break;
        }
        if (period > 0) {
            vTaskDelay(pdMS_TO_TICKS(period));
        }
    }

    // Report what the bus actually did, not just what was queued
    esp_err_t err = canfd_port_wait_tx_done(port, SEND_TIMEOUT_MS);
    canfd_port_get_status(port, &st, &after);
    const uint32_t ok = after.tx_ok - before.tx_ok;
    const uint32_t fail = after.tx_fail - before.tx_fail;
    printf("CAN%d: queued %d, sent %" PRIu32 ", failed %" PRIu32 "  (%s TEC=%u REC=%u)\n", port, queued, ok, fail,
           canfd_state_str(st.state), st.tx_error_count, st.rx_error_count);
    if (st.state == TWAI_ERROR_BUS_OFF) {
        printf("      CAN%d is bus-off: run 'can_recover %d' or enable auto recovery (can_init %d -a <ms>)\n",
               port, port, port);
    } else if (err != ESP_OK) {
        printf("      TX not finished (%s). No ACK from another node? Check wiring/termination.\n"
               "      The frame is retried until acknowledged; 'can_init %d' flushes it.\n",
               esp_err_to_name(err), port);
    }
    return (queued == count && fail == 0 && err == ESP_OK) ? 0 : 1;
}

/* ------------------------------------------------ can_dump ------------------------------------------------ */

static struct {
    struct arg_str *args;
    struct arg_end *end;
} s_dump_args;

static int cmd_can_dump(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_dump_args) != 0) {
        arg_print_errors(stderr, s_dump_args.end, argv[0]);
        return 1;
    }
    int first = CANFD_PORT_FIRST, last = CANFD_PORT_LAST;
    bool enable = true;
    for (int i = 0; i < s_dump_args.args->count; i++) {
        const char *a = s_dump_args.args->sval[i];
        if (strcasecmp(a, "on") == 0) {
            enable = true;
        } else if (strcasecmp(a, "off") == 0) {
            enable = false;
        } else if (!parse_port_range(a, &first, &last)) {
            printf("Invalid argument '%s'\n", a);
            return 1;
        }
    }
    for (int port = first; port <= last; port++) {
        canfd_port_set_dump(port, enable);
        printf("CAN%d dump %s%s\n", port, enable ? "on" : "off",
               canfd_port_is_open(port) ? "" : " (port not initialized)");
    }
    return 0;
}

/* ------------------------------------------------ can_status ------------------------------------------------ */

static struct {
    struct arg_str *port;
    struct arg_lit *clear;
    struct arg_end *end;
} s_status_args;

static int cmd_can_status(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_status_args) != 0) {
        arg_print_errors(stderr, s_status_args.end, argv[0]);
        return 1;
    }
    int first, last;
    if (!parse_port_range(s_status_args.port->count ? s_status_args.port->sval[0] : NULL, &first, &last)) {
        printf("Invalid port, use 1, 2 or all\n");
        return 1;
    }
    for (int port = first; port <= last; port++) {
        print_port_line(port);
        if (!canfd_port_is_open(port)) {
            continue;
        }
        twai_node_status_t st;
        canfd_port_stats_t s;
        canfd_port_get_status(port, &st, &s);
        printf("      state=%s  TEC=%u  REC=%u  txq_free=%" PRIu32 "\n",
               canfd_state_str(st.state), st.tx_error_count, st.rx_error_count, st.tx_queue_remaining);
        printf("      tx_ok=%" PRIu32 "  tx_fail=%" PRIu32 "  rx=%" PRIu32 "  rx_drop=%" PRIu32 "\n",
               s.tx_ok, s.tx_fail, s.rx, s.rx_drop);
        printf("      bus_err=%" PRIu32 " (bit=%" PRIu32 " form=%" PRIu32 " stuff=%" PRIu32 " ack=%" PRIu32 ")"
               "  arb_lost=%" PRIu32 "\n",
               s.bus_err, s.bit_err, s.form_err, s.stuff_err, s.ack_err, s.arb_lost);
        printf("      err_passive=%" PRIu32 "  bus_off=%" PRIu32 "  recover=%" PRIu32 "  fault=%s\n",
               s.err_passive, s.bus_off, s.recover, canfd_fault_str(canfd_port_get_fault(port)));
        canfd_port_mem_info_t mi;
        if (canfd_port_get_mem_info(port, &mi) == ESP_OK) {
            printf("      rxq %" PRIu32 " x %" PRIu32 " B @%p (%s) peak=%" PRIu32
                   "   txpool %" PRIu32 " x %" PRIu32 " B @%p (%s) peak=%" PRIu32 "\n",
                   mi.rxq_depth, mi.rxq_item_size, mi.rxq_buf, mi.rxq_in_psram ? "PSRAM" : "internal", s.rxq_peak,
                   mi.txpool_size, mi.txpool_item_size, mi.txpool_buf, mi.txpool_in_psram ? "PSRAM" : "internal",
                   s.txpool_peak);
        }
        if (s_status_args.clear->count) {
            canfd_port_reset_stats(port);
            printf("      (statistics cleared)\n");
        }
    }
    return 0;
}

/* ------------------------------------------------ can_recover ------------------------------------------------ */

static struct {
    struct arg_str *port;
    struct arg_int *timeout;
    struct arg_end *end;
} s_recover_args;

static int cmd_can_recover(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_recover_args) != 0) {
        arg_print_errors(stderr, s_recover_args.end, argv[0]);
        return 1;
    }
    int first, last;
    if (!parse_port_range(s_recover_args.port->count ? s_recover_args.port->sval[0] : NULL, &first, &last)) {
        printf("Invalid port, use 1, 2 or all\n");
        return 1;
    }
    const int timeout_ms = s_recover_args.timeout->count ? s_recover_args.timeout->ival[0] : 2000;
    int ret = 0;
    for (int port = first; port <= last; port++) {
        if (!canfd_port_is_open(port)) {
            continue;
        }
        twai_node_status_t st;
        canfd_port_get_status(port, &st, NULL);
        if (st.state != TWAI_ERROR_BUS_OFF) {
            printf("CAN%d is %s, no recovery needed\n", port, canfd_state_str(st.state));
            continue;
        }
        esp_err_t err = canfd_port_recover(port, timeout_ms);
        printf("CAN%d recover: %s\n", port, err == ESP_OK ? "done" : esp_err_to_name(err));
        ret |= (err != ESP_OK);
    }
    return ret;
}

/* ------------------------------------------------ can_errlog ------------------------------------------------ */

static struct {
    struct arg_str *args;
    struct arg_end *end;
} s_errlog_args;

static int cmd_can_errlog(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_errlog_args) != 0) {
        arg_print_errors(stderr, s_errlog_args.end, argv[0]);
        return 1;
    }
    int first = CANFD_PORT_FIRST, last = CANFD_PORT_LAST;
    bool enable = true;
    for (int i = 0; i < s_errlog_args.args->count; i++) {
        const char *a = s_errlog_args.args->sval[i];
        if (strcasecmp(a, "on") == 0) {
            enable = true;
        } else if (strcasecmp(a, "off") == 0) {
            enable = false;
        } else if (!parse_port_range(a, &first, &last)) {
            printf("Invalid argument '%s'\n", a);
            return 1;
        }
    }
    for (int port = first; port <= last; port++) {
        canfd_port_set_errlog(port, enable);
        printf("CAN%d error log %s\n", port, enable ? "on" : "off");
    }
    return 0;
}

/* ------------------------------------------------ can_fault ------------------------------------------------ */

static struct {
    struct arg_str *port;
    struct arg_str *fault;
    struct arg_end *end;
} s_fault_args;

static int cmd_can_fault(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_fault_args) != 0) {
        arg_print_errors(stderr, s_fault_args.end, argv[0]);
        return 1;
    }
    int first, last;
    if (!parse_port_range(s_fault_args.port->sval[0], &first, &last)) {
        printf("Invalid port, use 1, 2 or all\n");
        return 1;
    }
    const char *f = s_fault_args.fault->sval[0];
    canfd_fault_t fault;
    if (strcasecmp(f, "dominant") == 0 || strcasecmp(f, "dom") == 0) {
        fault = CANFD_FAULT_DOMINANT;
    } else if (strcasecmp(f, "recessive") == 0 || strcasecmp(f, "rec") == 0) {
        fault = CANFD_FAULT_RECESSIVE;
    } else if (strcasecmp(f, "off") == 0 || strcasecmp(f, "none") == 0) {
        fault = CANFD_FAULT_NONE;
    } else {
        printf("Invalid fault '%s', use dominant, recessive or off\n", f);
        return 1;
    }
    int ret = 0;
    for (int port = first; port <= last; port++) {
        int tx, rx;
        canfd_port_get_pins(port, &tx, &rx);
        esp_err_t err = canfd_port_inject_fault(port, fault);
        if (err != ESP_OK) {
            printf("CAN%d fault injection failed: %s\n", port, esp_err_to_name(err));
            ret = 1;
            continue;
        }
        printf("CAN%d TX (GPIO%d): %s\n", port, tx,
               fault == CANFD_FAULT_DOMINANT ? "forced dominant (bus held low)" :
               fault == CANFD_FAULT_RECESSIVE ? "forced recessive (node cannot drive the bus)" :
               "released to the controller");
    }
    return ret;
}

/* ------------------------------------------------ can_test ------------------------------------------------ */

typedef struct {
    int src;
    int dst;
    canfd_msg_t tmpl;           // header and payload length of every test frame
    uint32_t count;
    uint32_t gap_ms;
    uint32_t rx_delay_us;       // simulated slow receiver: busy time per received frame
    /* sender side */
    uint32_t queued;
    esp_err_t tx_err;
    TaskHandle_t waiter;
    /* receiver side, updated from the dst RX task */
    volatile uint32_t rx_ok;
    volatile uint32_t rx_bad;   // flags, length or payload mismatch
    volatile uint32_t rx_lost;
    volatile uint32_t rx_dup;   // duplicated or out of order
    volatile uint32_t rx_other; // frames with another ID
    volatile uint32_t next_seq;
    volatile int64_t last_rx_us;
    SemaphoreHandle_t done;
} link_test_t;

// Static: the RX task may still touch a link for a moment after the test returns
static link_test_t s_links[2];

static struct {
    struct arg_str *dir;
    struct arg_int *count;
    struct arg_int *len;
    struct arg_str *id;
    struct arg_lit *ext;
    struct arg_lit *classic;
    struct arg_lit *nobrs;
    struct arg_int *gap;
    struct arg_int *rxdelay;
    struct arg_int *timeout;
    struct arg_end *end;
} s_test_args;

/* Payload: bytes 0..3 carry the sequence number (if len >= 4), the rest is a seq dependent pattern */
static void test_fill_payload(uint8_t *buf, uint8_t len, uint32_t seq)
{
    for (int i = 0; i < len; i++) {
        buf[i] = (uint8_t)(seq * 7 + i * 13 + 0x5A);
    }
    if (len >= 4) {
        buf[0] = (uint8_t)seq;
        buf[1] = (uint8_t)(seq >> 8);
        buf[2] = (uint8_t)(seq >> 16);
        buf[3] = (uint8_t)(seq >> 24);
    }
}

static void test_rx_hook(int port, const canfd_msg_t *m, void *arg)
{
    link_test_t *t = (link_test_t *)arg;
    const twai_frame_header_t *h = &m->header;
    const twai_frame_header_t *e = &t->tmpl.header;

    if (h->id != e->id || h->ide != e->ide) {
        t->rx_other++;
        return;
    }
    if (t->rx_delay_us) {
        esp_rom_delay_us(t->rx_delay_us);   // slow consumer: lets the RX queue fill up
    }
    t->last_rx_us = esp_timer_get_time();

    uint32_t seq = t->next_seq;
    bool seq_valid = true;
    if (t->tmpl.len >= 4 && m->len >= 4) {
        uint32_t rx_seq = m->data[0] | (m->data[1] << 8) | (m->data[2] << 16) | ((uint32_t)m->data[3] << 24);
        if (rx_seq < t->count) {
            seq = rx_seq;
        } else {
            seq_valid = false;  // corrupted sequence number, assume it is the expected frame
        }
    }
    if (seq > t->next_seq) {
        t->rx_lost += seq - t->next_seq;
    } else if (seq < t->next_seq) {
        t->rx_dup++;
    }

    uint8_t expect[TWAIFD_FRAME_MAX_LEN];
    test_fill_payload(expect, t->tmpl.len, seq);
    bool ok = seq_valid && h->fdf == e->fdf && h->brs == e->brs && !h->rtr &&
              m->len == t->tmpl.len && memcmp(m->data, expect, m->len) == 0;
    if (ok) {
        t->rx_ok++;
    } else {
        t->rx_bad++;
    }

    if (seq >= t->next_seq) {
        t->next_seq = seq + 1;
        if (t->next_seq >= t->count) {
            xSemaphoreGive(t->done);
        }
    }
}

static void test_send_all(link_test_t *t)
{
    canfd_msg_t msg = t->tmpl;
    for (uint32_t seq = 0; seq < t->count; seq++) {
        test_fill_payload(msg.data, msg.len, seq);
        esp_err_t err = canfd_port_send(t->src, &msg, SEND_TIMEOUT_MS);
        if (err != ESP_OK) {
            t->tx_err = err;
            break;
        }
        t->queued++;
        if (t->gap_ms) {
            vTaskDelay(pdMS_TO_TICKS(t->gap_ms));
        }
    }
}

static void test_sender_task(void *arg)
{
    link_test_t *t = (link_test_t *)arg;
    test_send_all(t);
    xTaskNotifyGive(t->waiter);
    vTaskDelete(NULL);
}

/* Rough frame time on the bus in ns, including ~20% bit stuffing */
static uint64_t test_frame_time_ns(const link_test_t *t, int port)
{
    const canfd_port_config_t *cfg = canfd_port_get_config(port);
    const twai_frame_header_t *h = &t->tmpl.header;
    const uint32_t id_bits = h->ide ? 18 : 0;
    const uint32_t len = t->tmpl.len;
    uint64_t ns;

    if (!h->fdf) {
        ns = (uint64_t)(47 + id_bits + len * 8) * 1000000000ULL / cfg->nominal_bitrate;
    } else if (h->brs && cfg->data_bitrate) {
        ns = (uint64_t)(30 + id_bits) * 1000000000ULL / cfg->nominal_bitrate +
             (uint64_t)(len * 8 + 40) * 1000000000ULL / cfg->data_bitrate;
    } else {
        ns = (uint64_t)(70 + id_bits + len * 8) * 1000000000ULL / cfg->nominal_bitrate;
    }
    return ns * 12 / 10;
}

static void test_print_link(const link_test_t *t, int64_t t0_us, const canfd_port_stats_t *src_diff)
{
    const twai_frame_header_t *h = &t->tmpl.header;
    const bool pass = t->tx_err == ESP_OK && t->queued == t->count && src_diff->tx_fail == 0 &&
                      t->rx_ok == t->count && t->rx_bad == 0 && t->rx_lost == 0 && t->rx_dup == 0;

    printf("--- CAN%d -> CAN%d : ID 0x%" PRIX32 " %s, %s, %u bytes, %" PRIu32 " frames ---\n",
           t->src, t->dst, h->id, h->ide ? "EXT" : "STD",
           h->fdf ? (h->brs ? "FD+BRS" : "FD") : "classic", t->tmpl.len, t->count);
    printf("  TX queued   : %" PRIu32 "  (tx_ok %" PRIu32 ", tx_fail %" PRIu32 ")%s%s\n",
           t->queued, src_diff->tx_ok, src_diff->tx_fail,
           t->tx_err == ESP_OK ? "" : "  error: ", t->tx_err == ESP_OK ? "" : esp_err_to_name(t->tx_err));
    printf("  RX ok       : %" PRIu32 "\n", t->rx_ok);
    printf("  RX mismatch : %" PRIu32 "\n", t->rx_bad);
    printf("  RX lost     : %" PRIu32 "\n", t->rx_lost + (t->count > t->next_seq ? t->count - t->next_seq : 0));
    printf("  RX dup/ooo  : %" PRIu32 "\n", t->rx_dup);
    if (t->rx_other) {
        printf("  RX other ID : %" PRIu32 "\n", t->rx_other);
    }
    if (t->rx_ok && t->last_rx_us > t0_us) {
        const uint64_t us = (uint64_t)(t->last_rx_us - t0_us);
        printf("  Elapsed     : %" PRIu64 ".%03" PRIu64 " ms  (%" PRIu64 " frames/s, payload %" PRIu64 " kbit/s)\n",
               us / 1000, us % 1000, (uint64_t)t->rx_ok * 1000000 / us,
               (uint64_t)t->rx_ok * t->tmpl.len * 8 * 1000 / us);
    }
    printf("  Result      : %s\n", pass ? "PASS" : "FAIL");
}

static void test_print_port_diff(int port, const canfd_port_stats_t *d)
{
    twai_node_status_t st;
    canfd_port_get_status(port, &st, NULL);
    printf("  CAN%d: bus_err +%" PRIu32 " (bit %" PRIu32 " form %" PRIu32 " stuff %" PRIu32 " ack %" PRIu32 ")"
           "  arb_lost +%" PRIu32 "  rx_drop +%" PRIu32 "  %s TEC=%u REC=%u\n",
           port, d->bus_err, d->bit_err, d->form_err, d->stuff_err, d->ack_err, d->arb_lost, d->rx_drop,
           canfd_state_str(st.state), st.tx_error_count, st.rx_error_count);
    printf("        rxq_peak %" PRIu32 "  txpool_peak %" PRIu32 "\n", d->rxq_peak, d->txpool_peak);
    if (d->err_passive || d->bus_off) {
        printf("        err_passive +%" PRIu32 "  bus_off +%" PRIu32 "  recover +%" PRIu32 "\n",
               d->err_passive, d->bus_off, d->recover);
    }
}

static void stats_diff(const canfd_port_stats_t *a, const canfd_port_stats_t *b, canfd_port_stats_t *d)
{
    d->tx_ok = b->tx_ok - a->tx_ok;
    d->tx_fail = b->tx_fail - a->tx_fail;
    d->rx = b->rx - a->rx;
    d->rx_drop = b->rx_drop - a->rx_drop;
    d->bus_err = b->bus_err - a->bus_err;
    d->arb_lost = b->arb_lost - a->arb_lost;
    d->bit_err = b->bit_err - a->bit_err;
    d->form_err = b->form_err - a->form_err;
    d->stuff_err = b->stuff_err - a->stuff_err;
    d->ack_err = b->ack_err - a->ack_err;
    d->bus_off = b->bus_off - a->bus_off;
    d->err_passive = b->err_passive - a->err_passive;
    d->recover = b->recover - a->recover;
    d->rxq_peak = b->rxq_peak;          // peaks are reset at the start of each run
    d->txpool_peak = b->txpool_peak;
}

/* Run 1 link, or 2 links at the same time (the second one from a helper task). Returns true on PASS. */
static bool test_run(link_test_t *links, int num, int timeout_ms)
{
    canfd_port_stats_t before[CANFD_PORT_NUM] = { 0 }, after[CANFD_PORT_NUM] = { 0 }, diff[CANFD_PORT_NUM];
    uint64_t est_ns = 0;

    for (int i = 0; i < num; i++) {
        link_test_t *t = &links[i];
        canfd_port_wait_tx_done(t->src, SEND_TIMEOUT_MS);   // also reclaims TX slots lost by an earlier bus-off
        t->queued = 0;
        t->tx_err = ESP_OK;
        t->rx_ok = t->rx_bad = t->rx_lost = t->rx_dup = t->rx_other = 0;
        t->next_seq = 0;
        t->last_rx_us = 0;
        t->waiter = xTaskGetCurrentTaskHandle();
        xSemaphoreTake(t->done, 0);
        est_ns += test_frame_time_ns(t, t->src) * t->count;
    }
    if (timeout_ms <= 0) {
        timeout_ms = (int)(est_ns * 2 / 1000000) + 1000;
        for (int i = 0; i < num; i++) {
            timeout_ms += links[i].gap_ms * links[i].count;
            timeout_ms += (int)((uint64_t)links[i].rx_delay_us * links[i].count / 1000);
        }
    }
    for (int port = CANFD_PORT_FIRST; port <= CANFD_PORT_LAST; port++) {
        canfd_port_reset_peaks(port);
        canfd_port_get_status(port, NULL, &before[port - 1]);
    }
    for (int i = 0; i < num; i++) {
        canfd_port_set_rx_hook(links[i].dst, test_rx_hook, &links[i]);
    }

    const int64_t t0 = esp_timer_get_time();
    if (num > 1 && xTaskCreate(test_sender_task, "can_test_tx", TEST_SENDER_STACK_SIZE, &links[1],
                               TEST_SENDER_PRIORITY, NULL) != pdPASS) {
        printf("Failed to create sender task\n");
        canfd_port_set_rx_hook(links[1].dst, NULL, NULL);
        num = 1;
    }
    test_send_all(&links[0]);
    if (num > 1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    for (int i = 0; i < num; i++) {
        int64_t left_ms = timeout_ms - (esp_timer_get_time() - t0) / 1000;
        if (links[i].queued) {
            xSemaphoreTake(links[i].done, pdMS_TO_TICKS(left_ms > 0 ? left_ms : 0));
        }
    }
    for (int i = 0; i < num; i++) {
        canfd_port_wait_tx_done(links[i].src, 100);
        canfd_port_set_rx_hook(links[i].dst, NULL, NULL);
    }

    for (int port = CANFD_PORT_FIRST; port <= CANFD_PORT_LAST; port++) {
        canfd_port_get_status(port, NULL, &after[port - 1]);
        stats_diff(&before[port - 1], &after[port - 1], &diff[port - 1]);
    }

    bool pass = true;
    for (int i = 0; i < num; i++) {
        const link_test_t *t = &links[i];
        test_print_link(t, t0, &diff[t->src - 1]);
        pass &= t->tx_err == ESP_OK && t->queued == t->count && diff[t->src - 1].tx_fail == 0 &&
                t->rx_ok == t->count && t->rx_bad == 0 && t->rx_lost == 0 && t->rx_dup == 0;
    }
    for (int port = CANFD_PORT_FIRST; port <= CANFD_PORT_LAST; port++) {
        if (canfd_port_is_open(port)) {
            test_print_port_diff(port, &diff[port - 1]);
        }
    }
    return pass;
}

static bool test_prepare_port(int port)
{
    if (canfd_port_is_open(port)) {
        return true;
    }
    canfd_port_config_t cfg;
    canfd_port_get_default_config(&cfg);
    printf("CAN%d not initialized, opening with defaults\n", port);
    esp_err_t err = canfd_port_open(port, &cfg);
    if (err != ESP_OK) {
        printf("CAN%d init failed: %s\n", port, esp_err_to_name(err));
        return false;
    }
    print_port_line(port);
    return true;
}

static int cmd_can_test(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_test_args) != 0) {
        arg_print_errors(stderr, s_test_args.end, argv[0]);
        return 1;
    }

    const char *dir = s_test_args.dir->count ? s_test_args.dir->sval[0] : "both";
    int pairs[2][2];
    int num_runs;           // sequential runs
    bool concurrent = false;
    if (strcmp(dir, "12") == 0) {
        pairs[0][0] = 1; pairs[0][1] = 2; num_runs = 1;
    } else if (strcmp(dir, "21") == 0) {
        pairs[0][0] = 2; pairs[0][1] = 1; num_runs = 1;
    } else if (strcmp(dir, "11") == 0) {
        pairs[0][0] = 1; pairs[0][1] = 1; num_runs = 1;
    } else if (strcmp(dir, "22") == 0) {
        pairs[0][0] = 2; pairs[0][1] = 2; num_runs = 1;
    } else if (strcasecmp(dir, "both") == 0 || strcasecmp(dir, "dual") == 0) {
        pairs[0][0] = 1; pairs[0][1] = 2;
        pairs[1][0] = 2; pairs[1][1] = 1;
        num_runs = 2;
        concurrent = strcasecmp(dir, "dual") == 0;
    } else {
        printf("Invalid direction '%s', use 12, 21, both, dual, 11 or 22\n", dir);
        return 1;
    }

    const uint32_t count = s_test_args.count->count ? (uint32_t)s_test_args.count->ival[0] : TEST_DEFAULT_COUNT;
    const bool fd = !s_test_args.classic->count;
    int len = s_test_args.len->count ? s_test_args.len->ival[0] : (fd ? TEST_DEFAULT_LEN : TWAI_FRAME_MAX_LEN);
    uint32_t id = TEST_DEFAULT_ID;
    if (s_test_args.id->count && !parse_hex_u32(s_test_args.id->sval[0], &id)) {
        printf("Invalid ID '%s' (hex expected)\n", s_test_args.id->sval[0]);
        return 1;
    }
    const bool ext = s_test_args.ext->count || id > TWAI_STD_ID_MASK;
    const uint32_t id_mask = ext ? TWAI_EXT_ID_MASK : TWAI_STD_ID_MASK;
    if (count == 0 || id > id_mask || len < 0 || len > (fd ? TWAIFD_FRAME_MAX_LEN : TWAI_FRAME_MAX_LEN)) {
        printf("Invalid count, ID or length\n");
        return 1;
    }
    if (fd && canfd_round_fd_len((uint8_t)len) != len) {
        printf("Note: FD length %d rounded up to %u\n", len, canfd_round_fd_len((uint8_t)len));
        len = canfd_round_fd_len((uint8_t)len);
    }

    for (int r = 0; r < num_runs; r++) {
        if (!test_prepare_port(pairs[r][0]) || !test_prepare_port(pairs[r][1])) {
            return 1;
        }
    }

    for (int r = 0; r < num_runs; r++) {
        link_test_t *t = &s_links[r];
        const canfd_port_config_t *src_cfg = canfd_port_get_config(pairs[r][0]);
        const canfd_port_config_t *dst_cfg = canfd_port_get_config(pairs[r][1]);
        if (src_cfg->mode == CANFD_MODE_LISTEN_ONLY) {
            printf("CAN%d is listen-only and cannot transmit\n", pairs[r][0]);
            return 1;
        }
        if (pairs[r][0] == pairs[r][1] && src_cfg->mode != CANFD_MODE_SELF_TEST) {
            printf("Single-port loop needs selftest mode: can_init %d -m selftest\n", pairs[r][0]);
            return 1;
        }
        t->src = pairs[r][0];
        t->dst = pairs[r][1];
        t->count = count;
        t->gap_ms = s_test_args.gap->count ? (uint32_t)s_test_args.gap->ival[0] : 0;
        memset(&t->tmpl, 0, sizeof(t->tmpl));
        t->tmpl.header.id = (id + r) & id_mask;     // each direction uses its own ID
        t->tmpl.header.ide = ext;
        t->tmpl.header.fdf = fd;
        t->tmpl.header.brs = fd && !s_test_args.nobrs->count && src_cfg->data_bitrate && dst_cfg->data_bitrate;
        t->tmpl.len = (uint8_t)len;
        t->rx_delay_us = s_test_args.rxdelay->count ? (uint32_t)s_test_args.rxdelay->ival[0] : 0;
        if (fd && !s_test_args.nobrs->count && !t->tmpl.header.brs) {
            printf("Note: no data bitrate on CAN%d/CAN%d, sending FD without BRS\n", t->src, t->dst);
        }
    }

    const int timeout_ms = s_test_args.timeout->count ? s_test_args.timeout->ival[0] : 0;
    bool pass = true;
    if (concurrent) {
        printf("Running CAN1 -> CAN2 and CAN2 -> CAN1 at the same time...\n");
        pass = test_run(s_links, 2, timeout_ms);
    } else {
        for (int r = 0; r < num_runs; r++) {
            pass &= test_run(&s_links[r], 1, timeout_ms);
        }
    }
    printf("=== can_test %s: %s ===\n", dir, pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

/* ------------------------------------------------ Registration ------------------------------------------------ */

void register_canfd_commands(void)
{
    s_init_args.port = arg_str0(NULL, NULL, "<port>", "1 | 2 | all (default: all)");
    s_init_args.bitrate = arg_int0("b", "bitrate", "<bps>", "nominal (arbitration) bitrate");
    s_init_args.dbitrate = arg_int0("d", "dbitrate", "<bps>", "data-phase bitrate (FD)");
    s_init_args.sp = arg_int0(NULL, "sp", "<permill>", "nominal sample point, e.g. 800");
    s_init_args.dsp = arg_int0(NULL, "dsp", "<permill>", "data-phase sample point, e.g. 750");
    s_init_args.dssp = arg_int0(NULL, "dssp", "<permill>", "data-phase secondary sample point (TDC), e.g. 700");
    s_init_args.retry = arg_int0("r", "retry", "<n>", "retransmission limit -1..15 (-1: forever)");
    s_init_args.autorec = arg_int0("a", "autorecover", "<ms>", "start bus-off recovery after <ms>, 0: manual (can_recover)");
    s_init_args.mode = arg_str0("m", "mode", "<mode>", "normal | listen | selftest");
    s_init_args.classic = arg_lit0("c", "classic", "classic CAN only (no data bitrate)");
    s_init_args.end = arg_end(10);

    s_deinit_args.port = arg_str0(NULL, NULL, "<port>", "1 | 2 | all (default: all)");
    s_deinit_args.end = arg_end(2);

    s_send_args.port = arg_str1(NULL, NULL, "<port>", "1 | 2");
    s_send_args.id = arg_str1(NULL, NULL, "<id>", "CAN ID in hex, e.g. 123 or 1ABCDEF0");
    s_send_args.data = arg_str0(NULL, NULL, "[data]", "payload in hex, e.g. 1122AABB or 11.22.AA.BB");
    s_send_args.ext = arg_lit0("e", "ext", "29-bit extended ID");
    s_send_args.fd = arg_lit0("f", "fd", "CAN-FD frame (up to 64 bytes)");
    s_send_args.brs = arg_lit0("b", "brs", "bit rate switch (implies --fd)");
    s_send_args.rtr = arg_lit0("r", "rtr", "remote frame (classic only, --len = requested DLC)");
    s_send_args.len = arg_int0("l", "len", "<n>", "payload length (filled with 00 01 02 .. if no data)");
    s_send_args.count = arg_int0("n", "count", "<n>", "number of frames (default: 1)");
    s_send_args.period = arg_int0("p", "period", "<ms>", "interval between frames (default: 0)");
    s_send_args.end = arg_end(10);

    s_dump_args.args = arg_strn(NULL, NULL, "[port] [on|off]", 0, 2, "port 1 | 2 | all (default: all), on (default) | off");
    s_dump_args.end = arg_end(3);

    s_status_args.port = arg_str0(NULL, NULL, "<port>", "1 | 2 | all (default: all)");
    s_status_args.clear = arg_lit0("c", "clear", "clear statistics after printing");
    s_status_args.end = arg_end(3);

    s_recover_args.port = arg_str0(NULL, NULL, "<port>", "1 | 2 | all (default: all)");
    s_recover_args.timeout = arg_int0("t", "timeout", "<ms>", "wait time for recovery (default: 2000)");
    s_recover_args.end = arg_end(3);

    s_errlog_args.args = arg_strn(NULL, NULL, "[port] [on|off]", 0, 2, "port 1 | 2 | all (default: all), on (default) | off");
    s_errlog_args.end = arg_end(3);

    s_fault_args.port = arg_str1(NULL, NULL, "<port>", "1 | 2 | all");
    s_fault_args.fault = arg_str1(NULL, NULL, "<fault>", "dominant | recessive | off");
    s_fault_args.end = arg_end(3);

    s_test_args.dir = arg_str0("D", "dir", "<dir>", "12 | 21 | both (default, sequential) | dual (simultaneous) | 11 | 22");
    s_test_args.count = arg_int0("n", "count", "<n>", "frames per direction (default: 100)");
    s_test_args.len = arg_int0("l", "len", "<n>", "payload length (default: 64 FD, 8 classic)");
    s_test_args.id = arg_str0("i", "id", "<hex>", "CAN ID of CAN1->CAN2, reverse uses ID+1 (default: 100)");
    s_test_args.ext = arg_lit0("e", "ext", "29-bit extended ID");
    s_test_args.classic = arg_lit0("c", "classic", "classic CAN frames instead of FD");
    s_test_args.nobrs = arg_lit0(NULL, "nobrs", "FD frames without bit rate switch");
    s_test_args.gap = arg_int0("g", "gap", "<ms>", "delay between frames (default: 0, back-to-back)");
    s_test_args.rxdelay = arg_int0("w", "rxdelay", "<us>", "slow receiver: busy time per received frame (fills the RX queue)");
    s_test_args.timeout = arg_int0("t", "timeout", "<ms>", "RX wait time (default: estimated from bitrate)");
    s_test_args.end = arg_end(10);

    for (int i = 0; i < 2; i++) {
        s_links[i].done = xSemaphoreCreateBinary();
        assert(s_links[i].done);
    }

    const esp_console_cmd_t cmds[] = {
        {
            .command = "can_init",
            .help = "Initialize (or re-initialize) CAN ports.\n"
                    "Examples: can_init | can_init 1 -b 500000 -d 2000000 | can_init all -c -b 1000000",
            .func = cmd_can_init,
            .argtable = &s_init_args,
        },
        {
            .command = "can_deinit",
            .help = "Close CAN ports",
            .func = cmd_can_deinit,
            .argtable = &s_deinit_args,
        },
        {
            .command = "can_send",
            .help = "Send frame(s).\n"
                    "Examples: can_send 1 123 11223344 | can_send 2 1ABCDEF0 -e -b -l 64 | can_send 1 100 -f -l 12 -n 10 -p 100",
            .func = cmd_can_send,
            .argtable = &s_send_args,
        },
        {
            .command = "can_dump",
            .help = "Print received frames. Examples: can_dump | can_dump 2 | can_dump off",
            .func = cmd_can_dump,
            .argtable = &s_dump_args,
        },
        {
            .command = "can_status",
            .help = "Show port state, error counters and statistics",
            .func = cmd_can_status,
            .argtable = &s_status_args,
        },
        {
            .command = "can_recover",
            .help = "Recover a port from bus-off",
            .func = cmd_can_recover,
            .argtable = &s_recover_args,
        },
        {
            .command = "can_errlog",
            .help = "Bus error summary log (every 500 ms while errors occur). Examples: can_errlog off | can_errlog 1 on",
            .func = cmd_can_errlog,
            .argtable = &s_errlog_args,
        },
        {
            .command = "can_fault",
            .help = "Fault injection on a TX pin: dominant = bus stuck dominant, recessive = TX line open.\n"
                    "Examples: can_fault 2 dominant | can_fault 1 recessive | can_fault all off",
            .func = cmd_can_fault,
            .argtable = &s_fault_args,
        },
        {
            .command = "can_test",
            .help = "CAN1 <-> CAN2 transfer test with sequence and payload check.\n"
                    "Examples: can_test | can_test -D dual -n 1000 | can_test -D 12 -c -l 8 | can_test -D 11 (selftest)",
            .func = cmd_can_test,
            .argtable = &s_test_args,
        },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}
