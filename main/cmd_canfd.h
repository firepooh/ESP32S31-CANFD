/*
 * Console commands for the CAN-FD 2-port test.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register all can_* console commands.
 */
void register_canfd_commands(void);

/**
 * @brief Open every port with the default (Kconfig) settings and print the result.
 */
void canfd_open_all_default(void);

#ifdef __cplusplus
}
#endif
