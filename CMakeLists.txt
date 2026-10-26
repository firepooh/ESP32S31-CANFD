# ESP32-S31 CAN-FD 2-port test (ESP-IDF v6.1)
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only the components that main depends on
idf_build_set_property(MINIMAL_BUILD ON)
project(s31_canfd_test)
