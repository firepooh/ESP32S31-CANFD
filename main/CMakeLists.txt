set(srcs "app_main.c" "canfd_port.c" "cmd_canfd.c" "cmd_system.c" "ch_mode.c" "obd_ecu.c" "cmd_mode.c")
if(CONFIG_CANDLE_USB_ENABLE)
    list(APPEND srcs "candle.c" "candle_usb.c")
endif()

idf_component_register(SRCS ${srcs}
                       REQUIRES esp_driver_twai esp_driver_gpio esp_timer esp_psram heap console esp_hw_support nvs_flash
                       INCLUDE_DIRS ".")
