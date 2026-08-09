/*
 * NVS-backed runtime settings (SPEC §5.2, §5.4, §5.5).
 *
 * Nothing here is ever compiled into the image as a literal credential. SPEC §5.4 is
 * explicit: SSID and password come from NVS, are set through the control channel, and
 * are never committed. The defaults below cover line settings only.
 */
#ifndef BRIDGE_CONFIG_H
#define BRIDGE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define CFG_SSID_MAX 33 /* 32 + NUL, matches wifi_sta_config_t.ssid */
#define CFG_PASS_MAX 65 /* 64 + NUL */

/* SPEC §5.2: 115200 8N1, no flow control. */
#define CFG_DEFAULT_BAUD 115200
#define CFG_DEFAULT_DATA_BITS 8
#define CFG_DEFAULT_PARITY 'N'
#define CFG_DEFAULT_STOP_BITS 1

/* SPEC §5.4: SoftAP starts if STA cannot associate within this long. */
#define CFG_DEFAULT_AP_FALLBACK_S 30

/* SPEC §5.6: default 64 KB, in PSRAM. */
#define CFG_DEFAULT_RINGLOG_BYTES (64 * 1024)

typedef struct {
    uint32_t baud;
    uint8_t data_bits; /* 5-8 */
    char parity;       /* 'N', 'E', 'O' */
    uint8_t stop_bits; /* 1 or 2 */

    char sta_ssid[CFG_SSID_MAX];
    char sta_pass[CFG_PASS_MAX];

    char ap_ssid[CFG_SSID_MAX];
    char ap_pass[CFG_PASS_MAX];
    bool ap_enabled;         /* SPEC §5.4: runtime-disableable */
    uint32_t ap_fallback_s;

    uint32_t ringlog_bytes;
    bool telnet_iac_escape; /* SPEC §5.4.1: off by default */
} bridge_config_t;

/* Reads NVS into the singleton, filling defaults for anything absent. Safe to call once
 * at boot, after nvs_flash_init(). */
esp_err_t config_init(void);

/* The live settings. Never NULL after config_init(). Treat as read-only; mutate through
 * the setters so the change is persisted. */
const bridge_config_t *config_get(void);

/* Each setter validates, updates the singleton and writes through to NVS. A rejected
 * value leaves both untouched and returns ESP_ERR_INVALID_ARG. */
esp_err_t config_set_baud(uint32_t baud);
esp_err_t config_set_line(uint8_t data_bits, char parity, uint8_t stop_bits);
esp_err_t config_set_sta(const char *ssid, const char *pass);
esp_err_t config_set_ap(const char *ssid, const char *pass);
esp_err_t config_set_ap_enabled(bool enabled);

/* Parses "8N1" and friends. Exposed so the console command and the config module agree
 * on exactly one grammar. */
esp_err_t config_parse_line(const char *spec, uint8_t *data_bits, char *parity,
                            uint8_t *stop_bits);

#endif /* BRIDGE_CONFIG_H */
