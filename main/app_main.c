/*
 * ESP32-S31 CAN-FD 2-port test with console (ESP-IDF v6.1)
 *
 * Board : ESP32-S31-Function-CoreBoard-1 (no on-board CAN transceiver)
 * CAN1  : TX GPIO35, RX GPIO36  -> external CAN-FD transceiver
 * CAN2  : TX GPIO37, RX GPIO38  -> external CAN-FD transceiver
 *
 * Connect CANH/CANL of both transceivers to one bus (120 ohm at each end)
 * and run "can_test" on the console.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_console.h"
#include "canfd_port.h"
#include "cmd_canfd.h"
#include "cmd_system.h"
#if CONFIG_CANDLE_USB_ENABLE
#include "candle_usb.h"
#endif

static const char *TAG = "app";

static void print_banner(void)
{
    printf("\n"
           "==============================================================\n"
           " ESP32-S31 CAN-FD 2-port test\n"
           "   CAN1: TX GPIO%d / RX GPIO%d\n"
           "   CAN2: TX GPIO%d / RX GPIO%d\n"
           "   default: nominal %d bps, data %d bps\n"
           "--------------------------------------------------------------\n"
           " can_status            port state and statistics\n"
           " can_test              CAN1->CAN2, CAN2->CAN1 transfer test\n"
           " can_test -D dual      both directions at the same time\n"
           " can_send 1 123 1122   send a frame (see 'help can_send')\n"
           " can_dump              print received frames\n"
           " mem [-t 1024]         RAM / PSRAM usage and PSRAM test\n"
#if CONFIG_CANDLE_USB_ENABLE
           " usb                   USB Candlelight adapter (HS OTG) state\n"
#endif
           " help                  all commands\n"
           "==============================================================\n",
           CONFIG_CANFD_PORT1_TX_GPIO, CONFIG_CANFD_PORT1_RX_GPIO,
           CONFIG_CANFD_PORT2_TX_GPIO, CONFIG_CANFD_PORT2_RX_GPIO,
           CONFIG_CANFD_DEFAULT_NOMINAL_BITRATE, CONFIG_CANFD_DEFAULT_DATA_BITRATE);
}

void app_main(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "canfd>";
    repl_config.max_cmdline_length = 256;
    repl_config.task_stack_size = 8192;

    ESP_LOGI(TAG, "Starting CAN-FD 2-port test");

#if CONFIG_ESP_CONSOLE_UART
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t cdc_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&cdc_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t usbjtag_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usbjtag_config, &repl_config, &repl));
#else
    ESP_LOGE(TAG, "No console device configured");
    return;
#endif

    register_canfd_commands();
    register_system_commands();
#if CONFIG_CANDLE_USB_ENABLE
    register_candle_commands();
#endif
    print_banner();

#if CONFIG_CANFD_AUTO_INIT
    canfd_open_all_default();
#endif
#if CONFIG_CANDLE_USB_ENABLE
    ESP_ERROR_CHECK(candle_usb_init());
#endif

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
