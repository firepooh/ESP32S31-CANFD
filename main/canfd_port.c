/*
 * CAN-FD port abstraction on top of the ESP-IDF TWAI (on-chip) driver.
 *
 * - The TWAI driver keeps a pointer to every queued TX frame until it is sent,
 *   so each port owns a pool of TX frame slots that are handed back in `on_tx_done`.
 * - Received frames and state changes are pushed from the ISR into an event queue
 *   and handled by a per-port RX task (dump print or test hook).
 * - The RX task also prints bus error summaries and starts the automatic bus-off recovery.
 * - With CONFIG_CANFD_QUEUE_IN_PSRAM the queue buffers and the TX frame pool live in PSRAM.
 *   The queue control blocks stay in internal RAM because they hold spinlocks (atomic access).
 */
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "driver/gpio.h"
#include "esp_private/gpio.h"
#include "soc/gpio_sig_map.h"
#include "canfd_port.h"

static const char *TAG = "canfd_port";

#define TX_POOL_SIZE            (CONFIG_CANFD_TX_QUEUE_DEPTH + 4)
#define RX_TASK_STACK_SIZE      4096
#define RX_TASK_PRIORITY        10
#define TIMESTAMP_RES_HZ        1000000     // 1 us RX timestamp
#define ERRLOG_PERIOD_US        500000

#if CONFIG_CANFD_QUEUE_IN_PSRAM
#define QUEUE_BUF_CAPS          (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define QUEUE_BUF_CAPS          (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

typedef uint16_t tx_idx_t;      // index into tx_slots, queued in tx_free_q

typedef enum {
    PORT_EVT_RX = 0,
    PORT_EVT_STATE,
    PORT_EVT_TX_DONE,
    PORT_EVT_STOP,
} port_evt_type_t;

typedef struct {
    uint8_t type;           // port_evt_type_t
    uint8_t old_state;      // twai_error_state_t, for PORT_EVT_STATE; TX success for PORT_EVT_TX_DONE
    uint8_t new_state;
    canfd_msg_t msg;        // for PORT_EVT_RX and PORT_EVT_TX_DONE
} port_evt_t;

typedef struct {
    twai_frame_t frame;     // must stay the first member, see port_on_tx_done()
    uint8_t data[TWAIFD_FRAME_MAX_LEN];
    uint32_t tag;           // canfd_msg_t.tag of the queued frame
} tx_slot_t;

typedef struct {
    int tx_gpio;
    int rx_gpio;
    bool is_open;
    canfd_port_config_t cfg;
    twai_node_handle_t node;
    QueueHandle_t evt_q;
    QueueHandle_t tx_free_q;        // indexes of free entries in tx_slots
    StaticQueue_t evt_q_cb;         // control blocks: internal RAM (this struct is static)
    StaticQueue_t tx_free_q_cb;
    uint8_t *evt_q_buf;             // queue storage: QUEUE_BUF_CAPS
    uint8_t *tx_free_q_buf;
    tx_slot_t *tx_slots;            // TX frame pool: QUEUE_BUF_CAPS
    SemaphoreHandle_t tx_lock;
    TaskHandle_t rx_task;
    TaskHandle_t closer;
    canfd_port_stats_t stats;
    canfd_port_stats_t errlog_stats;    // snapshot of the last error summary
    volatile bool dump;
    volatile bool errlog;
    canfd_rx_hook_t rx_hook;
    void *rx_hook_arg;
    canfd_tx_hook_t volatile tx_hook;
    void *tx_hook_arg;
    int64_t recover_at_us;             // pending automatic recovery, 0 = none (RX task only)
    canfd_fault_t fault;
    uint32_t saved_sig_out;             // TX pin signal before the fault was injected
    bool sig_saved;
} canfd_port_t;

static canfd_port_t s_ports[CANFD_PORT_NUM] = {
    { .tx_gpio = CONFIG_CANFD_PORT1_TX_GPIO, .rx_gpio = CONFIG_CANFD_PORT1_RX_GPIO, .errlog = true },
    { .tx_gpio = CONFIG_CANFD_PORT2_TX_GPIO, .rx_gpio = CONFIG_CANFD_PORT2_RX_GPIO, .errlog = true },
};

static portMUX_TYPE s_hook_lock = portMUX_INITIALIZER_UNLOCKED;

static inline canfd_port_t *port_get(int port)
{
    return &s_ports[port - CANFD_PORT_FIRST];
}

static inline int port_num(const canfd_port_t *p)
{
    return (int)(p - s_ports) + CANFD_PORT_FIRST;
}

/* ------------------------------------------------ ISR callbacks ------------------------------------------------ */

static IRAM_ATTR bool port_on_tx_done(twai_node_handle_t handle, const twai_tx_done_event_data_t *edata, void *user_ctx)
{
    canfd_port_t *p = (canfd_port_t *)user_ctx;
    BaseType_t woken = pdFALSE;

    if (edata->is_tx_success) {
        p->stats.tx_ok++;
    } else {
        p->stats.tx_fail++;
    }

    const tx_slot_t *slot = (const tx_slot_t *)edata->done_tx_frame;
    if (slot >= p->tx_slots && slot < p->tx_slots + TX_POOL_SIZE) {
        if (p->tx_hook) {
            // Copy the frame before the slot is handed back and reused
            port_evt_t evt = {
                .type = PORT_EVT_TX_DONE,
                .old_state = edata->is_tx_success,
                .msg = {
                    .header = slot->frame.header,
                    .len = (uint8_t)slot->frame.buffer_len,
                    .tag = slot->tag,
                },
            };
            memcpy(evt.msg.data, slot->data, slot->frame.buffer_len);
            if (xQueueSendFromISR(p->evt_q, &evt, &woken) != pdTRUE) {
                p->stats.rx_drop++;
            }
        }
        tx_idx_t idx = (tx_idx_t)(slot - p->tx_slots);
        xQueueSendFromISR(p->tx_free_q, &idx, &woken);
    }
    return woken == pdTRUE;
}

static IRAM_ATTR bool port_on_rx_done(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *user_ctx)
{
    canfd_port_t *p = (canfd_port_t *)user_ctx;
    BaseType_t woken = pdFALSE;
    port_evt_t evt = { .type = PORT_EVT_RX };
    twai_frame_t frame = {
        .buffer = evt.msg.data,
        .buffer_len = sizeof(evt.msg.data),
    };

    if (twai_node_receive_from_isr(handle, &frame) != ESP_OK) {
        return false;
    }
    evt.msg.header = frame.header;
    if (frame.header.rtr) {
        evt.msg.len = 0;
    } else {
        uint16_t len = twaifd_dlc2len(frame.header.dlc);
        evt.msg.len = (!frame.header.fdf && len > TWAI_FRAME_MAX_LEN) ? TWAI_FRAME_MAX_LEN : len;
    }

    p->stats.rx++;
    if (xQueueSendFromISR(p->evt_q, &evt, &woken) != pdTRUE) {
        p->stats.rx_drop++;
    }
    UBaseType_t fill = uxQueueMessagesWaitingFromISR(p->evt_q);
    if (fill > p->stats.rxq_peak) {
        p->stats.rxq_peak = fill;
    }
    return woken == pdTRUE;
}

static IRAM_ATTR bool port_on_state_change(twai_node_handle_t handle, const twai_state_change_event_data_t *edata, void *user_ctx)
{
    canfd_port_t *p = (canfd_port_t *)user_ctx;
    BaseType_t woken = pdFALSE;
    port_evt_t evt = {
        .type = PORT_EVT_STATE,
        .old_state = (uint8_t)edata->old_sta,
        .new_state = (uint8_t)edata->new_sta,
    };

    if (edata->new_sta == TWAI_ERROR_BUS_OFF) {
        p->stats.bus_off++;
    } else if (edata->new_sta == TWAI_ERROR_PASSIVE && edata->old_sta != TWAI_ERROR_PASSIVE) {
        p->stats.err_passive++;
    }
    xQueueSendFromISR(p->evt_q, &evt, &woken);
    return woken == pdTRUE;
}

static IRAM_ATTR bool port_on_error(twai_node_handle_t handle, const twai_error_event_data_t *edata, void *user_ctx)
{
    canfd_port_t *p = (canfd_port_t *)user_ctx;
    const twai_error_flags_t f = edata->err_flags;
    const twai_error_flags_t arb_only = { .arb_lost = 1 };

    if (f.arb_lost) {
        p->stats.arb_lost++;
    }
    if (f.bit_err) {
        p->stats.bit_err++;
    }
    if (f.form_err) {
        p->stats.form_err++;
    }
    if (f.stuff_err) {
        p->stats.stuff_err++;
    }
    if (f.ack_err) {
        p->stats.ack_err++;
    }
    // Losing arbitration is normal bus behavior, not an error
    if (f.val != arb_only.val) {
        p->stats.bus_err++;
    }
    return false;
}

/* ------------------------------------------------ RX task ------------------------------------------------ */

static void port_print_msg(int port, const canfd_msg_t *m)
{
    char line[48 + TWAIFD_FRAME_MAX_LEN * 3];
    const twai_frame_header_t *h = &m->header;
    int n = snprintf(line, sizeof(line), "CAN%d %5" PRIu32 ".%06" PRIu32 "  ",
                     port, (uint32_t)(h->timestamp / 1000000), (uint32_t)(h->timestamp % 1000000));

    if (h->ide) {
        n += snprintf(line + n, sizeof(line) - n, "%08" PRIX32 " ", h->id);
    } else {
        n += snprintf(line + n, sizeof(line) - n, "     %03" PRIX32 " ", h->id);
    }
    n += snprintf(line + n, sizeof(line) - n, "%s%s%s",
                  h->fdf ? "FD " : "   ", h->brs ? "BRS " : "    ", h->esi ? "ESI " : "    ");

    if (h->rtr) {
        snprintf(line + n, sizeof(line) - n, "[%u] RTR", h->dlc);
    } else {
        n += snprintf(line + n, sizeof(line) - n, "[%2u] ", m->len);
        for (int i = 0; i < m->len && n < (int)sizeof(line) - 4; i++) {
            n += snprintf(line + n, sizeof(line) - n, " %02X", m->data[i]);
        }
    }
    printf("%s\n", line);
}

/* Print what changed since the last summary, so error storms produce one line per period */
static void port_log_errors(canfd_port_t *p, int port)
{
    const canfd_port_stats_t now = p->stats;
    const canfd_port_stats_t *last = &p->errlog_stats;

    if (p->errlog && (now.bus_err != last->bus_err || now.tx_fail != last->tx_fail)) {
        twai_node_status_t st;
        twai_node_get_info(p->node, &st, NULL);
        ESP_LOGW(TAG, "CAN%d bus errors +%" PRIu32 " (bit %" PRIu32 " form %" PRIu32 " stuff %" PRIu32 " ack %" PRIu32
                 ")  tx_fail +%" PRIu32 "  %s TEC=%u REC=%u", port,
                 now.bus_err - last->bus_err, now.bit_err - last->bit_err, now.form_err - last->form_err,
                 now.stuff_err - last->stuff_err, now.ack_err - last->ack_err, now.tx_fail - last->tx_fail,
                 canfd_state_str(st.state), st.tx_error_count, st.rx_error_count);
    }
    p->errlog_stats = now;
}

static void port_auto_recover(canfd_port_t *p, int port)
{
    twai_node_status_t st;
    p->recover_at_us = 0;
    if (!p->is_open) {
        return;
    }
    twai_node_get_info(p->node, &st, NULL);
    if (st.state == TWAI_ERROR_BUS_OFF && twai_node_recover(p->node) == ESP_OK) {
        p->stats.recover++;
        ESP_LOGI(TAG, "CAN%d: automatic bus-off recovery started", port);
    }
}

static void port_rx_task(void *arg)
{
    canfd_port_t *p = (canfd_port_t *)arg;
    const int port = port_num(p);
    int64_t next_log_us = esp_timer_get_time() + ERRLOG_PERIOD_US;
    port_evt_t evt;

    while (true) {
        int64_t now = esp_timer_get_time();
        if (now >= next_log_us) {
            port_log_errors(p, port);
            next_log_us = now + ERRLOG_PERIOD_US;
        }
        if (p->recover_at_us && now >= p->recover_at_us) {
            port_auto_recover(p, port);
        }

        int64_t wake_us = next_log_us;
        if (p->recover_at_us && p->recover_at_us < wake_us) {
            wake_us = p->recover_at_us;
        }
        TickType_t wait = pdMS_TO_TICKS((wake_us - now + 999) / 1000);
        if (xQueueReceive(p->evt_q, &evt, wait ? wait : 1) != pdTRUE) {
            continue;
        }
        if (evt.type == PORT_EVT_STOP) {
            break;
        }
        if (evt.type == PORT_EVT_STATE) {
            if (evt.new_state == evt.old_state) {
                // The driver reports ERROR_ACTIVE once on enable, nothing changed
            } else if (evt.new_state == TWAI_ERROR_ACTIVE) {
                ESP_LOGI(TAG, "CAN%d state: %s -> %s", port,
                         canfd_state_str(evt.old_state), canfd_state_str(evt.new_state));
            } else {
                port_log_errors(p, port);   // show the errors that led here before the state line
                ESP_LOGW(TAG, "CAN%d state: %s -> %s", port,
                         canfd_state_str(evt.old_state), canfd_state_str(evt.new_state));
            }
            if (evt.new_state == TWAI_ERROR_BUS_OFF && p->cfg.auto_recover_ms) {
                p->recover_at_us = esp_timer_get_time() + (int64_t)p->cfg.auto_recover_ms * 1000;
            }
            continue;
        }
        if (evt.type == PORT_EVT_TX_DONE) {
            canfd_tx_hook_t tx_hook;
            void *tx_hook_arg;
            taskENTER_CRITICAL(&s_hook_lock);
            tx_hook = p->tx_hook;
            tx_hook_arg = p->tx_hook_arg;
            taskEXIT_CRITICAL(&s_hook_lock);
            if (tx_hook) {
                tx_hook(port, &evt.msg, evt.old_state != 0, tx_hook_arg);
            }
            continue;
        }

        canfd_rx_hook_t hook;
        void *hook_arg;
        taskENTER_CRITICAL(&s_hook_lock);
        hook = p->rx_hook;
        hook_arg = p->rx_hook_arg;
        taskEXIT_CRITICAL(&s_hook_lock);

        if (hook) {
            hook(port, &evt.msg, hook_arg);
        } else if (p->dump) {
            port_print_msg(port, &evt.msg);
        }
    }

    xTaskNotifyGive(p->closer);
    vTaskDelete(NULL);
}

/* ------------------------------------------------ Helpers ------------------------------------------------ */

static void port_refill_tx_pool(canfd_port_t *p)
{
    xQueueReset(p->tx_free_q);
    for (tx_idx_t i = 0; i < TX_POOL_SIZE; i++) {
        xQueueSend(p->tx_free_q, &i, 0);
    }
}

static void port_release(canfd_port_t *p, bool node_enabled)
{
    if (p->node && node_enabled) {
        twai_node_disable(p->node);
    }
    if (p->rx_task) {
        // The node is disabled, so the ISR no longer feeds the queue: the STOP event is the last one.
        // Stop the RX task before deleting the node, it may still use the node (error log, recovery).
        port_evt_t evt = { .type = PORT_EVT_STOP };
        p->closer = xTaskGetCurrentTaskHandle();
        xQueueSend(p->evt_q, &evt, portMAX_DELAY);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        p->rx_task = NULL;
    }
    if (p->node) {
        twai_node_delete(p->node);
        p->node = NULL;
    }
    if (p->evt_q) {
        vQueueDelete(p->evt_q);     // static queue: only detaches, the buffers are freed below
        p->evt_q = NULL;
    }
    if (p->tx_free_q) {
        vQueueDelete(p->tx_free_q);
        p->tx_free_q = NULL;
    }
    heap_caps_free(p->evt_q_buf);
    heap_caps_free(p->tx_free_q_buf);
    heap_caps_free(p->tx_slots);
    p->evt_q_buf = NULL;
    p->tx_free_q_buf = NULL;
    p->tx_slots = NULL;
    if (p->tx_lock) {
        vSemaphoreDelete(p->tx_lock);
        p->tx_lock = NULL;
    }
}

uint8_t canfd_round_fd_len(uint8_t len)
{
    if (len > TWAIFD_FRAME_MAX_LEN) {
        len = TWAIFD_FRAME_MAX_LEN;
    }
    return (uint8_t)twaifd_dlc2len(twaifd_len2dlc(len));
}

const char *canfd_state_str(twai_error_state_t state)
{
    switch (state) {
    case TWAI_ERROR_ACTIVE:  return "ERROR_ACTIVE";
    case TWAI_ERROR_WARNING: return "ERROR_WARNING";
    case TWAI_ERROR_PASSIVE: return "ERROR_PASSIVE";
    case TWAI_ERROR_BUS_OFF: return "BUS_OFF";
    default:                 return "UNKNOWN";
    }
}

const char *canfd_fault_str(canfd_fault_t fault)
{
    switch (fault) {
    case CANFD_FAULT_NONE:      return "none";
    case CANFD_FAULT_DOMINANT:  return "dominant";
    case CANFD_FAULT_RECESSIVE: return "recessive";
    default:                    return "unknown";
    }
}

const char *canfd_mode_str(canfd_mode_t mode)
{
    switch (mode) {
    case CANFD_MODE_NORMAL:      return "normal";
    case CANFD_MODE_LISTEN_ONLY: return "listen";
    case CANFD_MODE_SELF_TEST:   return "selftest";
    case CANFD_MODE_LOOPBACK:    return "loopback";
    default:                     return "unknown";
    }
}

/* ------------------------------------------------ Public API ------------------------------------------------ */

void canfd_port_get_default_config(canfd_port_config_t *cfg)
{
    *cfg = (canfd_port_config_t) {
        .nominal_bitrate = CONFIG_CANFD_DEFAULT_NOMINAL_BITRATE,
        .data_bitrate = CONFIG_CANFD_DEFAULT_DATA_BITRATE,
        .nominal_sp_permill = CONFIG_CANFD_DEFAULT_NOMINAL_SP_PERMILL,
        .data_sp_permill = CONFIG_CANFD_DEFAULT_DATA_SP_PERMILL,
        .data_ssp_permill = CONFIG_CANFD_DEFAULT_DATA_SSP_PERMILL,
        .retry_limit = -1,
        .auto_recover_ms = CONFIG_CANFD_DEFAULT_AUTO_RECOVER_MS,
        .mode = CANFD_MODE_NORMAL,
    };
}

bool canfd_port_is_valid(int port)
{
    return port >= CANFD_PORT_FIRST && port <= CANFD_PORT_LAST;
}

bool canfd_port_is_open(int port)
{
    return canfd_port_is_valid(port) && port_get(port)->is_open;
}

void canfd_port_get_pins(int port, int *tx_gpio, int *rx_gpio)
{
    const canfd_port_t *p = port_get(port);
    *tx_gpio = p->tx_gpio;
    *rx_gpio = p->rx_gpio;
}

const canfd_port_config_t *canfd_port_get_config(int port)
{
    return canfd_port_is_open(port) ? &port_get(port)->cfg : NULL;
}

esp_err_t canfd_port_open(int port, const canfd_port_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(canfd_port_is_valid(port) && cfg, ESP_ERR_INVALID_ARG, TAG, "invalid port %d", port);
    canfd_port_t *p = port_get(port);
    ESP_RETURN_ON_FALSE(!p->is_open, ESP_ERR_INVALID_STATE, TAG, "CAN%d already open", port);

    esp_err_t ret = ESP_OK;
    p->cfg = *cfg;
    memset(&p->stats, 0, sizeof(p->stats));
    memset(&p->errlog_stats, 0, sizeof(p->errlog_stats));
    p->recover_at_us = 0;
    // The driver routes the TX pin to the controller again, any injected fault is gone
    p->fault = CANFD_FAULT_NONE;
    p->sig_saved = false;

    // Large buffers go to QUEUE_BUF_CAPS (PSRAM or internal); the queue control blocks are part
    // of the static port struct in internal RAM because they hold spinlocks
    p->evt_q_buf = heap_caps_malloc(CONFIG_CANFD_RX_QUEUE_DEPTH * sizeof(port_evt_t), QUEUE_BUF_CAPS);
    p->tx_free_q_buf = heap_caps_malloc(TX_POOL_SIZE * sizeof(tx_idx_t), QUEUE_BUF_CAPS);
    p->tx_slots = heap_caps_calloc(TX_POOL_SIZE, sizeof(tx_slot_t), QUEUE_BUF_CAPS);
    ESP_GOTO_ON_FALSE(p->evt_q_buf && p->tx_free_q_buf && p->tx_slots, ESP_ERR_NO_MEM, err, TAG,
                      "CAN%d: no mem for queue buffers", port);
    p->evt_q = xQueueCreateStatic(CONFIG_CANFD_RX_QUEUE_DEPTH, sizeof(port_evt_t), p->evt_q_buf, &p->evt_q_cb);
    p->tx_free_q = xQueueCreateStatic(TX_POOL_SIZE, sizeof(tx_idx_t), p->tx_free_q_buf, &p->tx_free_q_cb);
    p->tx_lock = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(p->evt_q && p->tx_free_q && p->tx_lock, ESP_ERR_NO_MEM, err, TAG, "no mem");
    port_refill_tx_pool(p);

    twai_onchip_node_config_t node_cfg = {
        .io_cfg = {
            .tx = (gpio_num_t)p->tx_gpio,
            .rx = (gpio_num_t)p->rx_gpio,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing = {
            .bitrate = cfg->nominal_bitrate,
            .sp_permill = cfg->nominal_sp_permill,
        },
        .data_timing = {
            // With exact timing the data phase is set below, skip the bitrate calculation here
            .bitrate = cfg->timing_advanced ? 0 : cfg->data_bitrate,
            .sp_permill = cfg->data_sp_permill,
            .ssp_permill = cfg->data_ssp_permill,
        },
        .timestamp_resolution_hz = TIMESTAMP_RES_HZ,
        .fail_retry_cnt = cfg->retry_limit,
        .tx_queue_depth = CONFIG_CANFD_TX_QUEUE_DEPTH,
        .intr_priority = 0,
        .flags = {
            .enable_self_test = (cfg->mode == CANFD_MODE_SELF_TEST),
            .enable_loopback = (cfg->mode == CANFD_MODE_SELF_TEST || cfg->mode == CANFD_MODE_LOOPBACK),
            .enable_listen_only = (cfg->mode == CANFD_MODE_LISTEN_ONLY),
        },
    };
    ESP_GOTO_ON_ERROR(twai_new_node_onchip(&node_cfg, &p->node), err, TAG, "CAN%d: create node failed", port);

    if (cfg->timing_advanced) {
        ESP_GOTO_ON_ERROR(twai_node_reconfig_timing(p->node, &cfg->nominal_timing,
                                                    cfg->data_bitrate ? &cfg->data_timing : NULL),
                          err, TAG, "CAN%d: bit timing rejected", port);
    }

    twai_event_callbacks_t cbs = {
        .on_tx_done = port_on_tx_done,
        .on_rx_done = port_on_rx_done,
        .on_state_change = port_on_state_change,
        .on_error = port_on_error,
    };
    ESP_GOTO_ON_ERROR(twai_node_register_event_callbacks(p->node, &cbs, p), err, TAG, "CAN%d: register callbacks failed", port);

    char task_name[configMAX_TASK_NAME_LEN];
    snprintf(task_name, sizeof(task_name), "can%d_rx", port);
    ESP_GOTO_ON_FALSE(xTaskCreate(port_rx_task, task_name, RX_TASK_STACK_SIZE, p, RX_TASK_PRIORITY, &p->rx_task) == pdPASS,
                      ESP_ERR_NO_MEM, err, TAG, "CAN%d: create rx task failed", port);

    ESP_GOTO_ON_ERROR(twai_node_enable(p->node), err, TAG, "CAN%d: enable failed", port);
    p->is_open = true;
    return ESP_OK;

err:
    port_release(p, false);
    return ret;
}

esp_err_t canfd_port_close(int port)
{
    ESP_RETURN_ON_FALSE(canfd_port_is_open(port), ESP_ERR_INVALID_STATE, TAG, "CAN%d not open", port);
    canfd_port_t *p = port_get(port);

    p->is_open = false;
    p->dump = false;
    canfd_port_set_rx_hook(port, NULL, NULL);
    canfd_port_set_tx_hook(port, NULL, NULL);
    port_release(p, true);
    return ESP_OK;
}

esp_err_t canfd_port_recover(int port, int timeout_ms)
{
    ESP_RETURN_ON_FALSE(canfd_port_is_open(port), ESP_ERR_INVALID_STATE, TAG, "CAN%d not open", port);
    canfd_port_t *p = port_get(port);
    twai_node_status_t st;

    twai_node_get_info(p->node, &st, NULL);
    ESP_RETURN_ON_FALSE(st.state == TWAI_ERROR_BUS_OFF, ESP_ERR_INVALID_STATE, TAG, "CAN%d is not in bus-off (%s)",
                        port, canfd_state_str(st.state));
    ESP_RETURN_ON_ERROR(twai_node_recover(p->node), TAG, "CAN%d: recover failed", port);
    p->stats.recover++;

    // Recovery needs 128 x 11 recessive bits on the bus
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    do {
        vTaskDelay(pdMS_TO_TICKS(10));
        twai_node_get_info(p->node, &st, NULL);
    } while (st.state == TWAI_ERROR_BUS_OFF && esp_timer_get_time() < deadline);
    ESP_RETURN_ON_FALSE(st.state != TWAI_ERROR_BUS_OFF, ESP_ERR_TIMEOUT, TAG, "CAN%d: still bus-off, check the bus", port);

    // Frames that were on the hardware at bus-off time are not reported back, reclaim their slots
    canfd_port_wait_tx_done(port, timeout_ms);
    return ESP_OK;
}

esp_err_t canfd_port_send(int port, const canfd_msg_t *msg, int timeout_ms)
{
    ESP_RETURN_ON_FALSE(msg, ESP_ERR_INVALID_ARG, TAG, "msg is NULL");
    ESP_RETURN_ON_FALSE(canfd_port_is_open(port), ESP_ERR_INVALID_STATE, TAG, "CAN%d not open", port);
    canfd_port_t *p = port_get(port);
    const twai_frame_header_t *h = &msg->header;

    ESP_RETURN_ON_FALSE(!h->brs || h->fdf, ESP_ERR_INVALID_ARG, TAG, "BRS needs an FD frame");
    ESP_RETURN_ON_FALSE(!h->rtr || !h->fdf, ESP_ERR_INVALID_ARG, TAG, "FD frames have no RTR");
    ESP_RETURN_ON_FALSE(!h->brs || p->cfg.data_bitrate, ESP_ERR_INVALID_ARG, TAG, "BRS needs a data bitrate");
    ESP_RETURN_ON_FALSE(msg->len <= (h->fdf ? TWAIFD_FRAME_MAX_LEN : TWAI_FRAME_MAX_LEN), ESP_ERR_INVALID_ARG, TAG,
                        "length %u too long", msg->len);

    const uint8_t len = h->fdf ? canfd_round_fd_len(msg->len) : msg->len;
    const TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    esp_err_t ret = ESP_ERR_TIMEOUT;
    tx_idx_t idx;

    xSemaphoreTake(p->tx_lock, portMAX_DELAY);
    if (xQueueReceive(p->tx_free_q, &idx, ticks) == pdTRUE) {
        uint32_t in_use = TX_POOL_SIZE - uxQueueMessagesWaiting(p->tx_free_q);
        if (in_use > p->stats.txpool_peak) {
            p->stats.txpool_peak = in_use;
        }
        tx_slot_t *slot = &p->tx_slots[idx];
        slot->tag = msg->tag;
        slot->frame = (twai_frame_t) {
            .header = *h,
            .buffer = slot->data,
        };
        slot->frame.header.trigger_time = 0;
        if (h->rtr) {
            // Remote frame: DLC carries the requested length, no payload
            slot->frame.header.dlc = len;
            slot->frame.buffer_len = 0;
        } else {
            memcpy(slot->data, msg->data, msg->len);
            memset(slot->data + msg->len, 0, len - msg->len);
            slot->frame.header.dlc = twaifd_len2dlc(len);
            slot->frame.buffer_len = len;
        }
        ret = twai_node_transmit(p->node, &slot->frame, timeout_ms);
        if (ret != ESP_OK) {
            xQueueSend(p->tx_free_q, &idx, 0);
        }
    }
    xSemaphoreGive(p->tx_lock);
    return ret;
}

esp_err_t canfd_port_wait_tx_done(int port, int timeout_ms)
{
    ESP_RETURN_ON_FALSE(canfd_port_is_open(port), ESP_ERR_INVALID_STATE, TAG, "CAN%d not open", port);
    canfd_port_t *p = port_get(port);

    esp_err_t ret = twai_node_transmit_wait_all_done(p->node, timeout_ms);
    if (ret == ESP_OK) {
        xSemaphoreTake(p->tx_lock, portMAX_DELAY);
        // Senders hold tx_lock, so if the driver is still idle here every slot outside the free list is lost
        if (uxQueueMessagesWaiting(p->tx_free_q) != TX_POOL_SIZE &&
                twai_node_transmit_wait_all_done(p->node, 0) == ESP_OK) {
            ESP_LOGW(TAG, "CAN%d: reclaimed %u lost TX slot(s)", port,
                     (unsigned)(TX_POOL_SIZE - uxQueueMessagesWaiting(p->tx_free_q)));
            port_refill_tx_pool(p);
        }
        xSemaphoreGive(p->tx_lock);
    }
    return ret;
}

esp_err_t canfd_port_get_status(int port, twai_node_status_t *status, canfd_port_stats_t *stats)
{
    ESP_RETURN_ON_FALSE(canfd_port_is_open(port), ESP_ERR_INVALID_STATE, TAG, "CAN%d not open", port);
    canfd_port_t *p = port_get(port);

    if (stats) {
        *stats = p->stats;
    }
    if (status) {
        return twai_node_get_info(p->node, status, NULL);
    }
    return ESP_OK;
}

void canfd_port_reset_stats(int port)
{
    if (canfd_port_is_valid(port)) {
        canfd_port_t *p = port_get(port);
        memset(&p->stats, 0, sizeof(canfd_port_stats_t));
        memset(&p->errlog_stats, 0, sizeof(canfd_port_stats_t));
    }
}

void canfd_port_reset_peaks(int port)
{
    if (canfd_port_is_valid(port)) {
        canfd_port_t *p = port_get(port);
        p->stats.rxq_peak = 0;
        p->stats.txpool_peak = 0;
    }
}

esp_err_t canfd_port_get_mem_info(int port, canfd_port_mem_info_t *info)
{
    ESP_RETURN_ON_FALSE(canfd_port_is_open(port) && info, ESP_ERR_INVALID_STATE, TAG, "CAN%d not open", port);
    const canfd_port_t *p = port_get(port);

    *info = (canfd_port_mem_info_t) {
        .rxq_depth = CONFIG_CANFD_RX_QUEUE_DEPTH,
        .rxq_item_size = sizeof(port_evt_t),
        .rxq_buf = p->evt_q_buf,
        .rxq_in_psram = esp_ptr_external_ram(p->evt_q_buf),
        .txpool_size = TX_POOL_SIZE,
        .txpool_item_size = sizeof(tx_slot_t),
        .txpool_buf = p->tx_slots,
        .txpool_in_psram = esp_ptr_external_ram(p->tx_slots),
    };
    return ESP_OK;
}

void canfd_port_set_errlog(int port, bool enable)
{
    if (canfd_port_is_valid(port)) {
        port_get(port)->errlog = enable;
    }
}

bool canfd_port_get_errlog(int port)
{
    return canfd_port_is_valid(port) && port_get(port)->errlog;
}

esp_err_t canfd_port_inject_fault(int port, canfd_fault_t fault)
{
    ESP_RETURN_ON_FALSE(canfd_port_is_valid(port), ESP_ERR_INVALID_ARG, TAG, "invalid port %d", port);
    canfd_port_t *p = port_get(port);
    const gpio_num_t tx = (gpio_num_t)p->tx_gpio;

    if (fault == CANFD_FAULT_NONE) {
        if (p->fault != CANFD_FAULT_NONE) {
            if (p->is_open && p->sig_saved) {
                gpio_matrix_output(tx, p->saved_sig_out, false, false);
            } else {
                gpio_set_level(tx, 1);  // closed port: leave the pin recessive
            }
            p->fault = CANFD_FAULT_NONE;
        }
        return ESP_OK;
    }

    if (p->fault == CANFD_FAULT_NONE) {
        gpio_io_config_t io;
        ESP_RETURN_ON_ERROR(gpio_get_io_config(tx, &io), TAG, "read TX pin config failed");
        p->saved_sig_out = io.sig_out;
        p->sig_saved = p->is_open && io.sig_out != SIG_GPIO_OUT_IDX;
    }
    // Set the level first so the pin does not glitch when it leaves the controller
    gpio_set_level(tx, fault == CANFD_FAULT_DOMINANT ? 0 : 1);
    gpio_matrix_output(tx, SIG_GPIO_OUT_IDX, false, false);
    gpio_output_enable(tx);
    p->fault = fault;
    return ESP_OK;
}

canfd_fault_t canfd_port_get_fault(int port)
{
    return canfd_port_is_valid(port) ? port_get(port)->fault : CANFD_FAULT_NONE;
}

void canfd_port_set_dump(int port, bool enable)
{
    if (canfd_port_is_valid(port)) {
        port_get(port)->dump = enable;
    }
}

bool canfd_port_get_dump(int port)
{
    return canfd_port_is_valid(port) && port_get(port)->dump;
}

void canfd_port_set_tx_hook(int port, canfd_tx_hook_t hook, void *arg)
{
    if (!canfd_port_is_valid(port)) {
        return;
    }
    canfd_port_t *p = port_get(port);
    taskENTER_CRITICAL(&s_hook_lock);
    p->tx_hook_arg = arg;
    p->tx_hook = hook;
    taskEXIT_CRITICAL(&s_hook_lock);
}

void canfd_port_set_rx_hook(int port, canfd_rx_hook_t hook, void *arg)
{
    if (!canfd_port_is_valid(port)) {
        return;
    }
    canfd_port_t *p = port_get(port);
    taskENTER_CRITICAL(&s_hook_lock);
    p->rx_hook = hook;
    p->rx_hook_arg = arg;
    taskEXIT_CRITICAL(&s_hook_lock);
}
