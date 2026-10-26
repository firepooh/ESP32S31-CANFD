idf_component_register(SRCS "app_main.c" "canfd_port.c" "cmd_canfd.c" "cmd_system.c"
                       REQUIRES esp_driver_twai esp_driver_gpio esp_timer esp_psram heap console
                       INCLUDE_DIRS ".")
