set(srcs "app_main.c" "canfd_port.c" "cmd_canfd.c" "cmd_system.c")
if(CONFIG_CANDLE_USB_ENABLE)
    list(APPEND srcs "candle.c" "candle_usb.c")
endif()

idf_component_register(SRCS ${srcs}
                       REQUIRES esp_driver_twai esp_driver_gpio esp_timer esp_psram heap console esp_hw_support
                       INCLUDE_DIRS ".")
