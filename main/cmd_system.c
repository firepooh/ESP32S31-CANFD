/*
 * System console commands (memory / PSRAM).
 *
 *   mem          internal RAM and PSRAM heap usage
 *   mem -t <KB>  PSRAM write/verify test with throughput
 *   mem -b <KB>  start a background PSRAM write/verify load (bus and cache pressure)
 *   mem -s       stop the background load and print its result
 */
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "cmd_system.h"

#define MEM_TEST_DEFAULT_KB     1024
#define MALLOC_PROBE_SIZE       (64 * 1024)
#define BG_TASK_STACK_SIZE      3072
#define BG_TASK_PRIORITY        3       // above the console (2), below the CAN RX tasks (10)

static struct {
    struct arg_int *test_kb;
    struct arg_int *bg_kb;
    struct arg_lit *stop;
    struct arg_end *end;
} s_mem_args;

static struct {
    TaskHandle_t task;
    volatile bool run;
    size_t kb;
    volatile uint32_t loops;
    volatile uint32_t errors;
    int64_t start_us;
} s_bg;

static void psram_bg_task(void *arg)
{
    const size_t words = s_bg.kb * 1024 / sizeof(uint32_t);
    uint32_t *buf = heap_caps_malloc(words * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    uint32_t seed = 0;

    while (buf && s_bg.run) {
        for (size_t i = 0; i < words; i++) {
            buf[i] = (uint32_t)i * 2654435761u ^ seed;
        }
        for (size_t i = 0; i < words; i++) {
            if (buf[i] != ((uint32_t)i * 2654435761u ^ seed)) {
                s_bg.errors++;
            }
        }
        seed += 0x9E3779B9u;
        s_bg.loops++;
        vTaskDelay(1);
    }
    if (buf == NULL) {
        printf("PSRAM load: cannot allocate %u KB\n", (unsigned)s_bg.kb);
    }
    heap_caps_free(buf);
    s_bg.task = NULL;
    vTaskDelete(NULL);
}

static int psram_bg_start(size_t kb)
{
    if (s_bg.task) {
        printf("PSRAM load already running (%u KB), stop it with 'mem -s'\n", (unsigned)s_bg.kb);
        return 1;
    }
    s_bg.kb = kb;
    s_bg.loops = 0;
    s_bg.errors = 0;
    s_bg.run = true;
    s_bg.start_us = esp_timer_get_time();
    if (xTaskCreate(psram_bg_task, "psram_load", BG_TASK_STACK_SIZE, NULL, BG_TASK_PRIORITY, &s_bg.task) != pdPASS) {
        s_bg.run = false;
        printf("PSRAM load: cannot create task\n");
        return 1;
    }
    printf("PSRAM load started: %u KB write/verify loop, priority %d\n", (unsigned)kb, BG_TASK_PRIORITY);
    return 0;
}

static int psram_bg_stop(void)
{
    if (s_bg.task == NULL) {
        printf("PSRAM load is not running\n");
        return 1;
    }
    s_bg.run = false;
    for (int i = 0; i < 200 && s_bg.task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const uint64_t us = (uint64_t)(esp_timer_get_time() - s_bg.start_us);
    const uint64_t kb_moved = (uint64_t)s_bg.loops * s_bg.kb * 2;     // one write and one read pass per loop
    printf("PSRAM load stopped: %" PRIu32 " loops, %" PRIu64 " KB moved in %" PRIu64 " ms (%" PRIu64 " KB/s), errors %" PRIu32 "  %s\n",
           s_bg.loops, kb_moved, us / 1000, us ? kb_moved * 1000000 / us : 0, s_bg.errors, s_bg.errors ? "FAIL" : "PASS");
    return s_bg.errors ? 1 : 0;
}

static void print_heap(const char *name, uint32_t caps)
{
    printf("%-9s total %8u  free %8u  min free %8u  largest %8u  bytes\n", name,
           (unsigned)heap_caps_get_total_size(caps), (unsigned)heap_caps_get_free_size(caps),
           (unsigned)heap_caps_get_minimum_free_size(caps), (unsigned)heap_caps_get_largest_free_block(caps));
}

/* Fill with an address dependent pattern, read back and compare. Returns the number of bad words. */
static int psram_test(size_t kb)
{
    const size_t words = kb * 1024 / sizeof(uint32_t);
    uint32_t *buf = heap_caps_malloc(words * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        printf("PSRAM test: cannot allocate %u KB\n", (unsigned)kb);
        return -1;
    }

    int64_t t0 = esp_timer_get_time();
    for (size_t i = 0; i < words; i++) {
        buf[i] = (uint32_t)i * 2654435761u ^ 0xA5A5A5A5u;
    }
    int64_t t1 = esp_timer_get_time();
    volatile uint32_t sum = 0;
    for (size_t i = 0; i < words; i++) {
        sum += buf[i];
    }
    int64_t t2 = esp_timer_get_time();
    int bad = 0;
    for (size_t i = 0; i < words; i++) {
        if (buf[i] != ((uint32_t)i * 2654435761u ^ 0xA5A5A5A5u)) {
            if (bad < 4) {
                printf("  mismatch at word %u: 0x%08" PRIx32 "\n", (unsigned)i, buf[i]);
            }
            bad++;
        }
    }

    const uint64_t bytes = (uint64_t)words * sizeof(uint32_t);
    printf("PSRAM test: %u KB at %p  write %" PRIu64 " KB/s  read %" PRIu64 " KB/s  errors %d  %s\n",
           (unsigned)kb, (void *)buf,
           (t1 > t0) ? bytes * 1000000 / 1024 / (uint64_t)(t1 - t0) : 0,
           (t2 > t1) ? bytes * 1000000 / 1024 / (uint64_t)(t2 - t1) : 0,
           bad, bad ? "FAIL" : "PASS");
    heap_caps_free(buf);
    return bad;
}

static int cmd_mem(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_mem_args) != 0) {
        arg_print_errors(stderr, s_mem_args.end, argv[0]);
        return 1;
    }
    if (s_mem_args.stop->count) {
        return psram_bg_stop();
    }
    if (s_mem_args.bg_kb->count) {
        if (!esp_psram_is_initialized()) {
            printf("PSRAM not initialized\n");
            return 1;
        }
        return psram_bg_start(s_mem_args.bg_kb->ival[0] > 0 ? (size_t)s_mem_args.bg_kb->ival[0] : MEM_TEST_DEFAULT_KB);
    }

    print_heap("internal", MALLOC_CAP_INTERNAL);
    if (!esp_psram_is_initialized()) {
        printf("PSRAM     not initialized (check CONFIG_SPIRAM and the boot log)\n");
        return s_mem_args.test_kb->count ? 1 : 0;
    }
    print_heap("psram", MALLOC_CAP_SPIRAM);
    printf("PSRAM     chip size %u bytes (%u MB), mode %s, %d MHz\n",
           (unsigned)esp_psram_get_size(), (unsigned)(esp_psram_get_size() / (1024 * 1024)),
#if CONFIG_SPIRAM_MODE_OCT
           "octal",
#else
           "quad",
#endif
           CONFIG_SPIRAM_SPEED);

    // Show where a plain malloc() of a large block ends up
    void *probe = malloc(MALLOC_PROBE_SIZE);
    if (probe) {
        printf("malloc(%u) -> %p (%s)\n", MALLOC_PROBE_SIZE, probe,
               esp_ptr_external_ram(probe) ? "PSRAM" : "internal");
        free(probe);
    }

    if (s_mem_args.test_kb->count) {
        int kb = s_mem_args.test_kb->ival[0] > 0 ? s_mem_args.test_kb->ival[0] : MEM_TEST_DEFAULT_KB;
        return psram_test((size_t)kb) == 0 ? 0 : 1;
    }
    return 0;
}

void register_system_commands(void)
{
    s_mem_args.test_kb = arg_int0("t", "test", "<KB>", "PSRAM write/verify test size in KB (e.g. 1024)");
    s_mem_args.bg_kb = arg_int0("b", "bg", "<KB>", "start a background PSRAM write/verify load");
    s_mem_args.stop = arg_lit0("s", "stop", "stop the background PSRAM load and print the result");
    s_mem_args.end = arg_end(4);

    const esp_console_cmd_t cmd = {
        .command = "mem",
        .help = "Show internal RAM and PSRAM heap usage.\n"
                "'mem -t 1024' runs a 1 MB PSRAM test, 'mem -b 2048' starts a background PSRAM load, 'mem -s' stops it",
        .func = cmd_mem,
        .argtable = &s_mem_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
