/*
 * Wi-Fi STA lifecycle with mandatory SoftAP fallback (SPEC §5.4).
 *
 * The fallback is not a convenience. The access point this device associates with is
 * usually the router under test, so the moment the console is most needed — the router
 * wedged, rebooting, or sitting in its bootloader — is exactly the moment STA dies.
 * SPEC §5.4 therefore requires the AP path, and acceptance criterion §8.8 tests it by
 * killing the router's Wi-Fi from the console itself.
 */
#ifndef BRIDGE_WIFI_H
#define BRIDGE_WIFI_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    WIFI_STATE_DISABLED,   /* no credentials configured */
    WIFI_STATE_CONNECTING, /* STA associating or retrying */
    WIFI_STATE_STA,        /* STA associated, IP acquired */
    WIFI_STATE_AP,         /* SoftAP fallback serving */
} wifi_state_t;

typedef struct {
    wifi_state_t state;
    char ssid[33];
    char ip[16];
    int8_t rssi;         /* STA only; 0 in AP mode */
    uint32_t disconnects;
    uint8_t ap_clients;  /* AP only */
} wifi_status_t;

esp_err_t wifi_start(void);
void wifi_get_status(wifi_status_t *out);

/* Re-reads credentials from config and restarts the association attempt. Called by the
 * console after `wifi set`, so a new network takes effect without a reboot. */
void wifi_reconfigure(void);

const char *wifi_state_name(wifi_state_t s);

#endif /* BRIDGE_WIFI_H */
