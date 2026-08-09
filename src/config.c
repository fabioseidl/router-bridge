#include "config.h"

#include <ctype.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cfg";

/* One namespace, short keys. NVS keys are capped at 15 characters. */
#define CFG_NS "bridge"

static bridge_config_t s_cfg;

static void load_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.baud = CFG_DEFAULT_BAUD;
    s_cfg.data_bits = CFG_DEFAULT_DATA_BITS;
    s_cfg.parity = CFG_DEFAULT_PARITY;
    s_cfg.stop_bits = CFG_DEFAULT_STOP_BITS;
    s_cfg.ap_enabled = true; /* SPEC §5.4: fallback is required, so on by default. */
    s_cfg.ap_fallback_s = CFG_DEFAULT_AP_FALLBACK_S;
    s_cfg.ringlog_bytes = CFG_DEFAULT_RINGLOG_BYTES;
    s_cfg.telnet_iac_escape = false; /* SPEC §5.4.1 */
}

/* Reading a string that was never written is not an error — it means "keep the default". */
static void read_str(nvs_handle_t h, const char *key, char *out, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        out[0] = '\0';
    }
}

static void read_u32(nvs_handle_t h, const char *key, uint32_t *out)
{
    uint32_t v;
    if (nvs_get_u32(h, key, &v) == ESP_OK) {
        *out = v;
    }
}

static void read_u8(nvs_handle_t h, const char *key, uint8_t *out)
{
    uint8_t v;
    if (nvs_get_u8(h, key, &v) == ESP_OK) {
        *out = v;
    }
}

esp_err_t config_init(void)
{
    load_defaults();

    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* First boot. Defaults stand; nothing is written until a setter runs. */
        ESP_LOGI(TAG, "no saved config, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    read_u32(h, "baud", &s_cfg.baud);
    read_u8(h, "databits", &s_cfg.data_bits);
    read_u8(h, "stopbits", &s_cfg.stop_bits);

    uint8_t parity = (uint8_t)s_cfg.parity;
    read_u8(h, "parity", &parity);
    s_cfg.parity = (char)parity;

    read_str(h, "sta_ssid", s_cfg.sta_ssid, sizeof(s_cfg.sta_ssid));
    read_str(h, "sta_pass", s_cfg.sta_pass, sizeof(s_cfg.sta_pass));
    read_str(h, "ap_ssid", s_cfg.ap_ssid, sizeof(s_cfg.ap_ssid));
    read_str(h, "ap_pass", s_cfg.ap_pass, sizeof(s_cfg.ap_pass));

    uint8_t ap_en = s_cfg.ap_enabled ? 1 : 0;
    read_u8(h, "ap_en", &ap_en);
    s_cfg.ap_enabled = (ap_en != 0);

    read_u32(h, "ap_fb_s", &s_cfg.ap_fallback_s);
    read_u32(h, "ringlog", &s_cfg.ringlog_bytes);

    uint8_t iac = 0;
    read_u8(h, "iac", &iac);
    s_cfg.telnet_iac_escape = (iac != 0);

    nvs_close(h);

    /* Credentials are deliberately not logged, not even truncated. */
    ESP_LOGI(TAG, "loaded: %lu %d%c%d, sta=%s, ap=%s (%s)", (unsigned long)s_cfg.baud,
             s_cfg.data_bits, s_cfg.parity, s_cfg.stop_bits,
             s_cfg.sta_ssid[0] ? "set" : "unset", s_cfg.ap_ssid[0] ? "set" : "unset",
             s_cfg.ap_enabled ? "enabled" : "disabled");
    return ESP_OK;
}

const bridge_config_t *config_get(void)
{
    return &s_cfg;
}

/* Commit helpers. Each opens, writes, commits and closes: these run at human speed from
 * the console, so holding a handle open buys nothing and risks losing a write on reset. */
static esp_err_t commit_u32(const char *key, uint32_t val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u32(h, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t commit_u8(const char *key, uint8_t val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, key, val);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t commit_str2(const char *k1, const char *v1, const char *k2,
                             const char *v2)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, k1, v1);
    if (err == ESP_OK) {
        err = nvs_set_str(h, k2, v2);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t config_set_baud(uint32_t baud)
{
    /* The UART divisor cannot represent arbitrarily low or high rates from the default
     * clock; these bounds comfortably contain every console rate in SPEC §2.4. */
    if (baud < 300 || baud > 4000000) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg.baud = baud;
    return commit_u32("baud", baud);
}

esp_err_t config_parse_line(const char *spec, uint8_t *data_bits, char *parity,
                            uint8_t *stop_bits)
{
    if (spec == NULL || strlen(spec) != 3) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t d = (uint8_t)(spec[0] - '0');
    const char p = (char)toupper((unsigned char)spec[1]);
    const uint8_t s = (uint8_t)(spec[2] - '0');

    if (d < 5 || d > 8) {
        return ESP_ERR_INVALID_ARG;
    }
    if (p != 'N' && p != 'E' && p != 'O') {
        return ESP_ERR_INVALID_ARG;
    }
    if (s != 1 && s != 2) {
        return ESP_ERR_INVALID_ARG;
    }

    *data_bits = d;
    *parity = p;
    *stop_bits = s;
    return ESP_OK;
}

esp_err_t config_set_line(uint8_t data_bits, char parity, uint8_t stop_bits)
{
    if (data_bits < 5 || data_bits > 8 || (stop_bits != 1 && stop_bits != 2)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (parity != 'N' && parity != 'E' && parity != 'O') {
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg.data_bits = data_bits;
    s_cfg.parity = parity;
    s_cfg.stop_bits = stop_bits;

    esp_err_t err = commit_u8("databits", data_bits);
    if (err == ESP_OK) {
        err = commit_u8("parity", (uint8_t)parity);
    }
    if (err == ESP_OK) {
        err = commit_u8("stopbits", stop_bits);
    }
    return err;
}

esp_err_t config_set_sta(const char *ssid, const char *pass)
{
    if (ssid == NULL || pass == NULL || ssid[0] == '\0' ||
        strlen(ssid) >= CFG_SSID_MAX || strlen(pass) >= CFG_PASS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_cfg.sta_ssid, ssid, sizeof(s_cfg.sta_ssid));
    strlcpy(s_cfg.sta_pass, pass, sizeof(s_cfg.sta_pass));
    return commit_str2("sta_ssid", ssid, "sta_pass", pass);
}

esp_err_t config_set_ap(const char *ssid, const char *pass)
{
    /* SPEC §5.4: no open AP. WPA2 needs 8 characters minimum, so a shorter passphrase
     * would silently produce an open network — reject it instead. */
    if (ssid == NULL || pass == NULL || ssid[0] == '\0' ||
        strlen(ssid) >= CFG_SSID_MAX || strlen(pass) < 8 ||
        strlen(pass) >= CFG_PASS_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_cfg.ap_ssid, ssid, sizeof(s_cfg.ap_ssid));
    strlcpy(s_cfg.ap_pass, pass, sizeof(s_cfg.ap_pass));
    return commit_str2("ap_ssid", ssid, "ap_pass", pass);
}

esp_err_t config_set_ap_enabled(bool enabled)
{
    s_cfg.ap_enabled = enabled;
    return commit_u8("ap_en", enabled ? 1 : 0);
}
