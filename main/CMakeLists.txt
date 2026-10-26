idf_component_register(SRCS "app_main.c" "canfd_port.c" "cmd_canfd.c"
                       REQUIRES esp_driver_twai esp_driver_gpio esp_timer console
                       INCLUDE_DIRS ".")
