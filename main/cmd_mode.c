/*
 * Console commands for the per-port mode and the OBD ECU settings.
 *
 *   mode                          show the mode of both ports
 *   mode <port> <0|1|2>           set and store the mode
 *   obd [port]                    show the OBD ECU settings
 *   obd <port> addr <hex>         physical request ID (response = request + 8)
 *   obd <port> on|off <item|all>  answer an item or not (05 0C 0D 1F 2F A6 VIN)
 *   obd <port> set <item> <val>   value of 05 0C 0D 2F A6
 *   obd <port> vin <17 chars>
 *   obd <port> dtc <P0301 ...>|none   stored DTCs (service 03)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_console.h"
#include "esp_err.h"
#include "ch_mode.h"
#include "obd_ecu.h"

static bool parse_port(const char *s, int *port)
{
    if (strncasecmp(s, "can", 3) == 0) {
        s += 3;
    }
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || !canfd_port_is_valid((int)v)) {
        return false;
    }
    *port = (int)v;
    return true;
}

static int cmd_mode(int argc, char **argv)
{
    if (argc == 1) {
        for (int port = CANFD_PORT_FIRST; port <= CANFD_PORT_LAST; port++) {
            printf("CAN%d: mode %d (%s)\n", port, ch_mode_get(port), ch_mode_str(ch_mode_get(port)));
        }
        return 0;
    }
    int port;
    char *end;
    long mode = argc == 3 ? strtol(argv[2], &end, 10) : -1;
    if (argc != 3 || !parse_port(argv[1], &port) || *end != '\0' || mode < 0 || mode >= CH_MODE_COUNT) {
        printf("usage: mode <port> <0|1|2>   0 off, 1 USB adapter, 2 USB adapter + OBD ECU\n");
        return 1;
    }
    esp_err_t err = ch_mode_set(port, (ch_mode_t)mode);
    if (err == ESP_ERR_INVALID_STATE) {
        printf("CAN%d is started by the USB host, stop it in cangaroo first\n", port);
        return 1;
    }
    if (err != ESP_OK) {
        printf("CAN%d: %s\n", port, esp_err_to_name(err));
        return 1;
    }
    printf("CAN%d: mode %ld (%s), saved\n", port, mode, ch_mode_str((ch_mode_t)mode));
    return 0;
}

static int obd_usage(void)
{
    printf("usage: obd [port]\n"
           "       obd <port> addr <hex>\n"
           "       obd <port> on|off <05|0C|0D|1F|2F|A6|VIN|DTC|all>\n"
           "       obd <port> set <05|0C|0D|2F|A6> <value>\n"
           "       obd <port> vin <17 characters>\n"
           "       obd <port> dtc <P0301 ...>|none\n");
    return 1;
}

static int cmd_obd(int argc, char **argv)
{
    int port;
    if (argc == 1) {
        for (port = CANFD_PORT_FIRST; port <= CANFD_PORT_LAST; port++) {
            obd_ecu_print(port);
        }
        return 0;
    }
    if (!parse_port(argv[1], &port)) {
        return obd_usage();
    }
    if (argc == 2) {
        obd_ecu_print(port);
        return 0;
    }

    obd_ecu_cfg_t *cfg = obd_ecu_cfg(port);
    const char *op = argv[2];
    char *end;
    if (strcasecmp(op, "addr") == 0 && argc == 4) {
        unsigned long id = strtoul(argv[3], &end, 16);
        if (*end != '\0' || id > 0x7F7 || id == 0x7DF) {
            printf("request ID must be 000..7F7 and not 7DF\n");
            return 1;
        }
        cfg->req_id = (uint16_t)id;
    } else if ((strcasecmp(op, "on") == 0 || strcasecmp(op, "off") == 0) && argc == 4) {
        const bool on = strcasecmp(op, "on") == 0;
        uint8_t mask;
        if (strcasecmp(argv[3], "all") == 0) {
            mask = (1u << OBD_ITEM_COUNT) - 1;
        } else {
            obd_item_t item = obd_ecu_item_find(argv[3]);
            if (item == OBD_ITEM_COUNT) {
                return obd_usage();
            }
            mask = 1u << item;
        }
        cfg->enabled = on ? (cfg->enabled | mask) : (cfg->enabled & ~mask);
    } else if (strcasecmp(op, "set") == 0 && argc == 5) {
        long v = strtol(argv[4], &end, 10);
        if (*end != '\0') {
            return obd_usage();
        }
        switch (obd_ecu_item_find(argv[3])) {
        case OBD_ITEM_COOLANT: cfg->coolant_c = (int16_t)(v < -40 ? -40 : v > 215 ? 215 : v); break;
        case OBD_ITEM_RPM:     cfg->rpm = (uint16_t)(v < 0 ? 0 : v > 16383 ? 16383 : v); break;
        case OBD_ITEM_SPEED:   cfg->speed_kmh = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); break;
        case OBD_ITEM_FUEL:    cfg->fuel_pct = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v); break;
        case OBD_ITEM_ODO:     cfg->odo_km = (uint32_t)(v < 0 ? 0 : v > 429496729 ? 429496729 : v); break;
        default:
            printf("value can be set for 05, 0C, 0D, 2F and A6 (1F counts from boot)\n");
            return 1;
        }
    } else if (strcasecmp(op, "vin") == 0 && argc == 4) {
        if (strlen(argv[3]) != OBD_VIN_LEN) {
            printf("VIN must have %d characters\n", OBD_VIN_LEN);
            return 1;
        }
        strlcpy(cfg->vin, argv[3], sizeof(cfg->vin));
    } else if (strcasecmp(op, "dtc") == 0 && argc >= 4) {
        const bool none = argc == 4 && strcasecmp(argv[3], "none") == 0;
        if (!obd_ecu_set_dtcs(port, none ? 0 : argc - 3, argv + 3)) {
            printf("DTCs like P0301 C0123 B1234 U0100, at most %d\n", OBD_DTC_MAX);
            return 1;
        }
    } else {
        return obd_usage();
    }

    esp_err_t err = obd_ecu_save(port);
    if (err != ESP_OK) {
        printf("save failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    obd_ecu_print(port);
    return 0;
}

void register_mode_commands(void)
{
    const esp_console_cmd_t mode_cmd = {
        .command = "mode",
        .help = "Show or set the port mode (stored in NVS): 0 off, 1 USB adapter, 2 USB adapter + OBD ECU",
        .hint = "[<port> <0|1|2>]",
        .func = cmd_mode,
    };
    const esp_console_cmd_t obd_cmd = {
        .command = "obd",
        .help = "Show or change the OBD ECU settings of a port (stored in NVS)\n"
                "  obd <port> addr <hex>              request ID, response = request + 8\n"
                "  obd <port> on|off <item|all>       items: 05 0C 0D 1F 2F A6 VIN DTC\n"
                "  obd <port> set <item> <value>      05 [C], 0C [rpm], 0D [km/h], 2F [%], A6 [km]\n"
                "  obd <port> vin <17 characters>\n"
                "  obd <port> dtc <P0301 ...>|none    stored DTCs for service 03 (at most 8)",
        .hint = "[<port> ...]",
        .func = cmd_obd,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&mode_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&obd_cmd));
}
