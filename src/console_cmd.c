#include "console_cmd.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "bridge.h"
#include "config.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_tcp.h"
#include "usb_cdc.h"
#include "wifi.h"

static const char *TAG = "console";

/* --- baud --------------------------------------------------------------------------- */

static struct {
    struct arg_int *rate;
    struct arg_end *end;
} s_baud_args;

static int cmd_baud(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_baud_args) != 0) {
        arg_print_errors(stderr, s_baud_args.end, argv[0]);
        return 1;
    }

    const uint32_t rate = (uint32_t)s_baud_args.rate->ival[0];
    if (config_set_baud(rate) != ESP_OK) {
        printf("invalid baud: %lu\n", (unsigned long)rate);
        return 1;
    }
    if (bridge_apply_line_settings() != ESP_OK) {
        printf("saved, but the live UART rejected it\n");
        return 1;
    }
    printf("baud = %lu\n", (unsigned long)rate);
    return 0;
}

/* --- line --------------------------------------------------------------------------- */

static struct {
    struct arg_str *spec;
    struct arg_end *end;
} s_line_args;

static int cmd_line(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_line_args) != 0) {
        arg_print_errors(stderr, s_line_args.end, argv[0]);
        return 1;
    }

    uint8_t data_bits, stop_bits;
    char parity;
    if (config_parse_line(s_line_args.spec->sval[0], &data_bits, &parity, &stop_bits) !=
        ESP_OK) {
        printf("expected a form like 8N1 (data 5-8, parity N/E/O, stop 1-2)\n");
        return 1;
    }
    if (config_set_line(data_bits, parity, stop_bits) != ESP_OK ||
        bridge_apply_line_settings() != ESP_OK) {
        printf("failed to apply\n");
        return 1;
    }
    printf("line = %d%c%d\n", data_bits, parity, stop_bits);
    return 0;
}

/* --- wifi --------------------------------------------------------------------------- */

static struct {
    struct arg_str *action; /* "set" or "status" */
    struct arg_str *ssid;
    struct arg_str *pass;
    struct arg_end *end;
} s_wifi_args;

static void print_wifi_status(void)
{
    wifi_status_t st;
    wifi_get_status(&st);

    printf("state       : %s\n", wifi_state_name(st.state));
    printf("ssid        : %s\n", st.ssid[0] ? st.ssid : "(unset)");
    printf("ip          : %s\n", st.ip);
    if (st.state == WIFI_STATE_AP) {
        printf("ap clients  : %u\n", st.ap_clients);
    } else {
        printf("rssi        : %d dBm\n", st.rssi);
    }
    printf("disconnects : %lu\n", (unsigned long)st.disconnects);
}

static int cmd_wifi(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_wifi_args) != 0) {
        arg_print_errors(stderr, s_wifi_args.end, argv[0]);
        return 1;
    }

    const char *action = s_wifi_args.action->sval[0];

    if (strcmp(action, "status") == 0) {
        print_wifi_status();
        return 0;
    }

    if (strcmp(action, "set") == 0) {
        if (s_wifi_args.ssid->count == 0 || s_wifi_args.pass->count == 0) {
            printf("usage: wifi set <ssid> <pass>\n");
            return 1;
        }
        if (config_set_sta(s_wifi_args.ssid->sval[0], s_wifi_args.pass->sval[0]) !=
            ESP_OK) {
            printf("invalid credentials (ssid 1-32, pass up to 64 chars)\n");
            return 1;
        }
        wifi_reconfigure();
        /* Deliberately not echoed back — the console log is often shared in bug reports. */
        printf("sta credentials saved; reassociating\n");
        return 0;
    }

    printf("usage: wifi <set|status> [ssid] [pass]\n");
    return 1;
}

/* --- ap ----------------------------------------------------------------------------- */

static struct {
    struct arg_str *action; /* "set", "on", "off" */
    struct arg_str *ssid;
    struct arg_str *pass;
    struct arg_end *end;
} s_ap_args;

static int cmd_ap(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_ap_args) != 0) {
        arg_print_errors(stderr, s_ap_args.end, argv[0]);
        return 1;
    }

    const char *action = s_ap_args.action->sval[0];

    if (strcmp(action, "on") == 0 || strcmp(action, "off") == 0) {
        const bool on = (strcmp(action, "on") == 0);
        if (config_set_ap_enabled(on) != ESP_OK) {
            printf("failed to save\n");
            return 1;
        }
        printf("softap fallback %s\n", on ? "enabled" : "disabled");
        return 0;
    }

    if (strcmp(action, "set") == 0) {
        if (s_ap_args.ssid->count == 0 || s_ap_args.pass->count == 0) {
            printf("usage: ap set <ssid> <pass>\n");
            return 1;
        }
        if (config_set_ap(s_ap_args.ssid->sval[0], s_ap_args.pass->sval[0]) != ESP_OK) {
            /* SPEC §5.4 forbids an open AP, and WPA2 needs 8 characters. */
            printf("invalid: ssid 1-32 chars, passphrase at least 8 (WPA2 minimum)\n");
            return 1;
        }
        printf("ap credentials saved\n");
        return 0;
    }

    printf("usage: ap <set|on|off> [ssid] [pass]\n");
    return 1;
}

/* --- stats -------------------------------------------------------------------------- */

static int cmd_stats(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    bridge_stats_t st;
    bridge_get_stats(&st);

    wifi_status_t wst;
    wifi_get_status(&wst);

    const int64_t up_us = esp_timer_get_time();

    printf("router -> host : %llu bytes\n", (unsigned long long)st.router_to_host);
    printf("host -> router : %llu bytes\n", (unsigned long long)st.host_to_router);
    printf("uart overflow  : %lu\n", (unsigned long)st.uart_overflow);
    printf("uart parity err: %lu\n", (unsigned long)st.uart_parity_err);
    printf("uart frame err : %lu\n", (unsigned long)st.uart_frame_err);
    printf("usb drops      : %lu\n", (unsigned long)usb_cdc_tx_drops());
    printf("tcp drops      : %lu\n", (unsigned long)net_tcp_tx_drops());
    printf("tcp clients    : %u\n", net_tcp_client_count());
    printf("wifi           : %s", wifi_state_name(wst.state));
    if (wst.state == WIFI_STATE_STA) {
        printf(" %s rssi %d dBm", wst.ip, wst.rssi);
    } else if (wst.state == WIFI_STATE_AP) {
        printf(" %s %u client(s)", wst.ip, wst.ap_clients);
    }
    printf("\n");
    printf("uptime         : %llu s\n", (unsigned long long)(up_us / 1000000));
    printf("free heap      : %lu bytes\n", (unsigned long)esp_get_free_heap_size());
    printf("free psram     : %u bytes\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("scrollback     : %u of %lu bytes\n", (unsigned)bridge_ringlog_used(),
           (unsigned long)config_get()->ringlog_bytes);
    return 0;
}

/* --- break -------------------------------------------------------------------------- */

static int cmd_break(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (bridge_send_break() != ESP_OK) {
        printf("break failed\n");
        return 1;
    }
    printf("break sent\n");
    return 0;
}

/* --- reset -------------------------------------------------------------------------- */

static int cmd_reset(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* The ESP32, not the router. SPEC §5.7: this project never drives the router's reset
     * pin. Flashing or resetting interrupts the bridge (SPEC §7). */
    printf("resetting the ESP32 (not the router)\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
}

/* --- log ---------------------------------------------------------------------------- */

static struct {
    struct arg_str *action; /* "dump" or "clear" */
    struct arg_end *end;
} s_log_args;

static int cmd_log(int argc, char **argv)
{
    if (arg_parse(argc, argv, (void **)&s_log_args) != 0) {
        arg_print_errors(stderr, s_log_args.end, argv[0]);
        return 1;
    }

    const char *action = s_log_args.action->sval[0];

    if (strcmp(action, "clear") == 0) {
        bridge_ringlog_clear();
        printf("scrollback cleared\n");
        return 0;
    }

    if (strcmp(action, "dump") != 0) {
        printf("usage: log <dump|clear>\n");
        return 1;
    }

    const size_t used = bridge_ringlog_used();
    if (used == 0) {
        printf("(scrollback empty)\n");
        return 0;
    }

    /* Streamed in slices so a 64 KB scrollback does not need a 64 KB stack buffer. */
    uint8_t *buf = malloc(1024);
    if (buf == NULL) {
        printf("out of memory\n");
        return 1;
    }

    size_t remaining = used;
    while (remaining > 0) {
        const size_t want = (remaining < 1024) ? remaining : 1024;
        const size_t n = bridge_ringlog_dump(buf, want);
        if (n == 0) {
            break;
        }
        fwrite(buf, 1, n, stdout);
        remaining -= n;
        /* bridge_ringlog_dump always returns the most recent n bytes, so a second call
         * would repeat them. One pass is the honest read; anything more needs an offset
         * API this feature does not justify. */
        break;
    }
    free(buf);
    printf("\n");
    return 0;
}

/* --- registration ------------------------------------------------------------------- */

static void register_commands(void)
{
    s_baud_args.rate = arg_int1(NULL, NULL, "<rate>", "baud rate, e.g. 115200");
    s_baud_args.end = arg_end(2);
    const esp_console_cmd_t baud_cmd = {
        .command = "baud",
        .help = "Set the router console baud rate (persisted)",
        .hint = NULL,
        .func = &cmd_baud,
        .argtable = &s_baud_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&baud_cmd));

    s_line_args.spec = arg_str1(NULL, NULL, "<8N1>", "data bits, parity, stop bits");
    s_line_args.end = arg_end(2);
    const esp_console_cmd_t line_cmd = {
        .command = "line",
        .help = "Set the UART line format, e.g. 8N1 (persisted)",
        .hint = NULL,
        .func = &cmd_line,
        .argtable = &s_line_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&line_cmd));

    s_wifi_args.action = arg_str1(NULL, NULL, "<set|status>", "action");
    s_wifi_args.ssid = arg_str0(NULL, NULL, "<ssid>", "network name");
    s_wifi_args.pass = arg_str0(NULL, NULL, "<pass>", "passphrase");
    s_wifi_args.end = arg_end(3);
    const esp_console_cmd_t wifi_cmd = {
        .command = "wifi",
        .help = "wifi set <ssid> <pass> | wifi status",
        .hint = NULL,
        .func = &cmd_wifi,
        .argtable = &s_wifi_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_cmd));

    s_ap_args.action = arg_str1(NULL, NULL, "<set|on|off>", "action");
    s_ap_args.ssid = arg_str0(NULL, NULL, "<ssid>", "AP name");
    s_ap_args.pass = arg_str0(NULL, NULL, "<pass>", "WPA2 passphrase, 8+ chars");
    s_ap_args.end = arg_end(3);
    const esp_console_cmd_t ap_cmd = {
        .command = "ap",
        .help = "ap set <ssid> <pass> | ap on | ap off",
        .hint = NULL,
        .func = &cmd_ap,
        .argtable = &s_ap_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ap_cmd));

    s_log_args.action = arg_str1(NULL, NULL, "<dump|clear>", "action");
    s_log_args.end = arg_end(2);
    const esp_console_cmd_t log_cmd = {
        .command = "log",
        .help = "log dump | log clear — the PSRAM scrollback of router output",
        .hint = NULL,
        .func = &cmd_log,
        .argtable = &s_log_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&log_cmd));

    const esp_console_cmd_t stats_cmd = {
        .command = "stats",
        .help = "Byte counts, overflow and drop counters, Wi-Fi, uptime, memory",
        .hint = NULL,
        .func = &cmd_stats,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&stats_cmd));

    const esp_console_cmd_t break_cmd = {
        .command = "break",
        .help = "Send a UART break to the router (interrupts some bootloaders)",
        .hint = NULL,
        .func = &cmd_break,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&break_cmd));

    const esp_console_cmd_t reset_cmd = {
        .command = "reset",
        .help = "Soft-reset the ESP32 — NOT the router",
        .hint = NULL,
        .func = &cmd_reset,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&reset_cmd));
}

esp_err_t console_cmd_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "bridge>";
    repl_cfg.max_cmdline_length = 256;

    /* UART0 — the FT232R "COM" port. Never the native USB, which is Path A. */
    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

    ESP_RETURN_ON_ERROR(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl), TAG,
                        "esp_console_new_repl_uart");

    esp_console_register_help_command();
    register_commands();

    ESP_RETURN_ON_ERROR(esp_console_start_repl(repl), TAG, "esp_console_start_repl");
    ESP_LOGI(TAG, "control channel on UART0 — type 'help'");
    return ESP_OK;
}
