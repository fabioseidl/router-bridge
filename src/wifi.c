#include "wifi.h"

#include <string.h>

#include "config.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "wifi";

/* Exponential backoff between association attempts, SPEC §5.4. Capped so a router that
 * comes back after an hour is still picked up promptly. */
#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 30000

/* While the SoftAP is serving we keep probing for the configured network so the device
 * returns to STA on its own — SPEC §5.4: "do not strand the device in AP mode". */
#define STA_RETRY_WHILE_AP_MS 30000

#define BIT_STA_GOT_IP BIT0

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif_sta;
static esp_netif_t *s_netif_ap;

static wifi_state_t s_state = WIFI_STATE_DISABLED;
static uint32_t s_disconnects;
static uint32_t s_backoff_ms = BACKOFF_MIN_MS;
static bool s_ap_active;
static char s_ip[16] = "0.0.0.0";
static TaskHandle_t s_mgr;

/* The unconfigured state is steady, so its diagnosis must be logged once and not on every
 * manager pass — this log is UART0, which the operator is reading for router output. */
static bool s_warned_unconfigured;

const char *wifi_state_name(wifi_state_t s)
{
    switch (s) {
    case WIFI_STATE_DISABLED:
        return "disabled";
    case WIFI_STATE_CONNECTING:
        return "connecting";
    case WIFI_STATE_STA:
        return "sta";
    case WIFI_STATE_AP:
        return "ap";
    default:
        return "?";
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state == WIFI_STATE_STA) {
            s_disconnects++;
            ESP_LOGW(TAG, "STA disconnected");
        }
        xEventGroupClearBits(s_events, BIT_STA_GOT_IP);
        if (s_state != WIFI_STATE_AP) {
            s_state = WIFI_STATE_CONNECTING;
        }
        /* Reconnection is driven by the manager task, not from here: retrying inside the
         * event handler would spin without backoff. */
        if (s_mgr != NULL) {
            xTaskNotifyGive(s_mgr);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_backoff_ms = BACKOFF_MIN_MS;
        s_state = WIFI_STATE_STA;
        xEventGroupSetBits(s_events, BIT_STA_GOT_IP);
        ESP_LOGI(TAG, "STA up, ip=%s", s_ip);
        if (s_mgr != NULL) {
            xTaskNotifyGive(s_mgr);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "AP: client joined");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "AP: client left");
    }
}

static esp_err_t apply_sta_config(void)
{
    const bridge_config_t *cfg = config_get();

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, cfg->sta_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, cfg->sta_pass, sizeof(wc.sta.password));
    /* An empty password means an open network; forcing WPA2 would make it unjoinable. */
    wc.sta.threshold.authmode = cfg->sta_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    return esp_wifi_set_config(WIFI_IF_STA, &wc);
}

static esp_err_t apply_ap_config(void)
{
    const bridge_config_t *cfg = config_get();

    wifi_config_t wc = {0};
    strlcpy((char *)wc.ap.ssid, cfg->ap_ssid, sizeof(wc.ap.ssid));
    strlcpy((char *)wc.ap.password, cfg->ap_pass, sizeof(wc.ap.password));
    wc.ap.ssid_len = (uint8_t)strlen(cfg->ap_ssid);
    wc.ap.channel = 1;
    /* SPEC §5.4: no open AP. config_set_ap() already rejects passphrases under 8
     * characters, so WPA2 is always satisfiable here. */
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.max_connection = 4;

    return esp_wifi_set_config(WIFI_IF_AP, &wc);
}

static void ap_start(void)
{
    const bridge_config_t *cfg = config_get();

    if (s_ap_active) {
        return;
    }
    if (!cfg->ap_enabled) {
        if (!s_warned_unconfigured) {
            ESP_LOGW(TAG, "STA failed and SoftAP fallback is disabled — Path B is down");
            s_warned_unconfigured = true;
        }
        return;
    }
    if (cfg->ap_ssid[0] == '\0' || cfg->ap_pass[0] == '\0') {
        if (!s_warned_unconfigured) {
            ESP_LOGE(TAG,
                     "STA failed and no AP credentials set. Path B is unreachable until "
                     "you run: ap set <ssid> <pass>");
            s_warned_unconfigured = true;
        }
        return;
    }

    /* APSTA rather than AP: the STA interface must stay alive so the manager can keep
     * probing for the configured network and hand back automatically. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(apply_ap_config());

    s_ap_active = true;
    s_state = WIFI_STATE_AP;

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_netif_ap, &ip) == ESP_OK) {
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ip.ip));
    }
    ESP_LOGW(TAG, "SoftAP fallback active: ssid='%s' ip=%s", cfg->ap_ssid, s_ip);
}

static void ap_stop(void)
{
    if (!s_ap_active) {
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_ap_active = false;
    ESP_LOGI(TAG, "SoftAP stopped, back on STA");
}

/*
 * Association manager.
 *
 * Owns every connect attempt so backoff is honoured in one place. Runs forever: even in
 * AP mode it keeps reaching for the configured network, which is what makes the return
 * to STA automatic.
 */
static void wifi_mgr_task(void *arg)
{
    (void)arg;

    const bridge_config_t *cfg = config_get();
    TickType_t fallback_deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(cfg->ap_fallback_s * 1000);

    for (;;) {
        cfg = config_get();

        if (cfg->sta_ssid[0] == '\0') {
            /* Nothing to associate with. Go straight to the fallback so Path B still
             * exists on a freshly flashed device. */
            if (!s_ap_active) {
                if (!s_warned_unconfigured) {
                    ESP_LOGW(TAG, "no STA credentials; falling back to SoftAP");
                }
                ap_start();
                if (!s_ap_active) {
                    s_state = WIFI_STATE_DISABLED;
                }
            }
            /* Nothing will change until the console sets credentials, and that path
             * notifies us directly. Sleep long rather than spinning. */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        const bool connected =
            (xEventGroupGetBits(s_events) & BIT_STA_GOT_IP) != 0;

        if (connected) {
            if (s_ap_active) {
                ap_stop();
                s_state = WIFI_STATE_STA;
            }
            fallback_deadline =
                xTaskGetTickCount() + pdMS_TO_TICKS(cfg->ap_fallback_s * 1000);
            /* Nothing to do until something breaks. */
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
            continue;
        }

        /* Not connected: try again, then wait out the backoff. */
        if (s_state != WIFI_STATE_AP) {
            s_state = WIFI_STATE_CONNECTING;
        }
        apply_sta_config();
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGD(TAG, "connect: %s", esp_err_to_name(err));
        }

        const uint32_t wait_ms = s_ap_active ? STA_RETRY_WHILE_AP_MS : s_backoff_ms;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));

        if (!s_ap_active) {
            s_backoff_ms *= 2;
            if (s_backoff_ms > BACKOFF_MAX_MS) {
                s_backoff_ms = BACKOFF_MAX_MS;
            }
            /* SPEC §5.4: start the fallback once the timeout has elapsed without an
             * association, whether this is the first boot or a mid-session drop. */
            if ((xEventGroupGetBits(s_events) & BIT_STA_GOT_IP) == 0 &&
                xTaskGetTickCount() >= fallback_deadline) {
                ap_start();
            }
        }
    }
}

void wifi_reconfigure(void)
{
    s_backoff_ms = BACKOFF_MIN_MS;
    s_warned_unconfigured = false; /* new credentials deserve a fresh diagnosis */
    esp_wifi_disconnect();
    if (s_mgr != NULL) {
        xTaskNotifyGive(s_mgr);
    }
}

void wifi_get_status(wifi_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = s_state;
    out->disconnects = s_disconnects;
    strlcpy(out->ip, s_ip, sizeof(out->ip));

    const bridge_config_t *cfg = config_get();
    if (s_state == WIFI_STATE_AP) {
        strlcpy(out->ssid, cfg->ap_ssid, sizeof(out->ssid));
        wifi_sta_list_t list;
        if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) {
            out->ap_clients = (uint8_t)list.num;
        }
    } else {
        strlcpy(out->ssid, cfg->sta_ssid, sizeof(out->ssid));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out->rssi = ap.rssi;
        }
    }
}

esp_err_t wifi_start(void)
{
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "esp_event_loop_create");

    s_netif_sta = esp_netif_create_default_wifi_sta();
    s_netif_ap = esp_netif_create_default_wifi_ap();
    if (s_netif_sta == NULL || s_netif_ap == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&ic), TAG, "esp_wifi_init");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            on_wifi_event, NULL, NULL),
                        TAG, "wifi event handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            on_wifi_event, NULL, NULL),
                        TAG, "ip event handler");

    /* Credentials live in NVS, and the Wi-Fi driver keeps its own copy there too. Ours is
     * the authority — SPEC §5.4 requires them settable from the control channel. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set_storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");

    s_state = WIFI_STATE_CONNECTING;
    if (xTaskCreate(wifi_mgr_task, "wifi_mgr", 4096, NULL, 5, &s_mgr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
