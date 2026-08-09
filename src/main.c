/*
 * ESP32-S3 Router Serial Console Bridge — application entry (SPEC §4, §10.3-§10.6).
 *
 * Brings up, in dependency order: NVS, config, the UART1 bridge core, the PSRAM
 * scrollback, Path A (USB CDC), Wi-Fi, Path B (TCP), and the UART0 control channel.
 *
 * ---------------------------------------------------------------------------
 * SAFETY: UART1 is now BIDIRECTIONAL.
 *
 * The step-2 skeleton assigned no TX pin at all, which made writing to the router
 * structurally impossible. That guarantee is gone by design — SPEC §5.1 requires a
 * transparent two-way pipe. GPIO17 reaches the router's console RX, so every byte
 * arriving from USB or TCP is a keystroke as far as the router is concerned, and SPEC §9
 * is explicit that writing to a bootloader console can brick the router.
 *
 * The remaining structural protections:
 *   - The router console is on UART1, never UART0, so the ROM bootloader's reset-time
 *     chatter is not injected into it (SPEC §2.5).
 *   - The IDF log goes to UART0 only, and ESP_CONSOLE_SECONDARY_NONE keeps it off the
 *     native USB port (sdkconfig.defaults), so Path A stays 8-bit clean.
 *   - The loopback self-test is still compiled out behind BRIDGE_ENABLE_LOOPBACK_TEST and
 *     must only be enabled with the router disconnected and GPIO17 jumpered to GPIO18.
 * ---------------------------------------------------------------------------
 */

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include "bridge.h"
#include "config.h"
#include "console_cmd.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_tcp.h"
#include "nvs_flash.h"
#include "usb_cdc.h"
#include "wifi.h"

static const char *TAG = "main";

#ifndef CONFIG_BRIDGE_UART_TX
#define CONFIG_BRIDGE_UART_TX 17
#endif
#ifndef CONFIG_BRIDGE_UART_RX
#define CONFIG_BRIDGE_UART_RX 18
#endif

/* Report what the silicon actually is, rather than what the silkscreen claims.
 * SPEC §8.2 is an acceptance criterion on this output. */
static void report_hardware(void)
{
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);

    ESP_LOGI(TAG, "chip: model=%d cores=%d revision=v%d.%d", (int)chip.model, chip.cores,
             chip.revision / 100, chip.revision % 100);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "flash: %lu bytes (%lu MB)", (unsigned long)flash_size,
                 (unsigned long)(flash_size / (1024 * 1024)));
    } else {
        ESP_LOGE(TAG, "flash: size query failed");
    }

    if (esp_psram_is_initialized()) {
        const size_t psram = esp_psram_get_size();
        ESP_LOGI(TAG, "psram: initialised, %u bytes (%u MB)", (unsigned)psram,
                 (unsigned)(psram / (1024 * 1024)));
    } else {
        ESP_LOGE(TAG, "psram: NOT initialised — check CONFIG_SPIRAM_MODE_OCT vs the "
                      "fitted part");
    }
}

/*
 * Classify what, if anything, is driving a pin — without driving it ourselves.
 *
 * Sample once with an internal pull-down and once with a pull-up. An external driver
 * (a router TX output idling high, even through a 470 Ohm series resistor) easily
 * overpowers the ~45 kOhm internal pull, so it reads the same both ways. A pin with
 * nothing attached follows whichever pull is enabled.
 *
 * Kept from the step-2 skeleton because it is what finally located the fault documented
 * in hardware.md §4.5, and it costs 80 ms at boot.
 */
static const char *classify_pin(int pin)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(20));
    const int with_pulldown = gpio_get_level((gpio_num_t)pin);

    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(20));
    const int with_pullup = gpio_get_level((gpio_num_t)pin);

    io.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io);

    if (with_pulldown == 1 && with_pullup == 1) {
        return "driven HIGH — a UART output idling high, i.e. connected and alive";
    }
    if (with_pulldown == 0 && with_pullup == 0) {
        return "driven LOW — line held low (break condition, or wired to GND)";
    }
    return "FLOATING — nothing is driving this pin";
}

/*
 * Link check, run before the UART driver claims either pin.
 *
 * The historical failure mode is worth restating on every boot: the harness was landed on
 * the devkit pins silkscreened "TX"/"RX", which are UART0 = GPIO43/44, not GPIO17/18. A
 * router wired there is invisible here, is fed this very boot log, and fights the FT232R
 * for GPIO44. See hardware.md §4.5.
 */
static void check_router_link(void)
{
    const char *rx_state = classify_pin(CONFIG_BRIDGE_UART_RX);
    const char *tx_state = classify_pin(CONFIG_BRIDGE_UART_TX);

    ESP_LOGI(TAG, "pin check GPIO%d (expected: router TX -> our RX): %s",
             CONFIG_BRIDGE_UART_RX, rx_state);
    ESP_LOGI(TAG, "pin check GPIO%d (expected: our TX -> router RX): %s",
             CONFIG_BRIDGE_UART_TX, tx_state);

    if (rx_state[0] == 'F') {
        ESP_LOGW(TAG, "GPIO%d is floating — no live router output is reaching it. If the "
                      "wires are on the pins silkscreened 'TX'/'RX', those are UART0 "
                      "(GPIO43/44), not the router link. See hardware.md §4.5.",
                 CONFIG_BRIDGE_UART_RX);
    }
}

#ifdef BRIDGE_ENABLE_LOOPBACK_TEST
/*
 * SPEC §10.2: write all 256 byte values and read them back, proving the driver path is
 * 8-bit clean before any of it is pointed at a real router.
 *
 * DANGEROUS with the router attached: the pattern contains CR and LF, so on a live
 * console it types junk and presses Enter. Only build this with a GPIO17<->GPIO18 jumper
 * and the router physically disconnected.
 */
static void uart1_loopback_test(void)
{
    uint8_t tx[256];
    uint8_t rx[256];

    for (size_t i = 0; i < sizeof(tx); i++) {
        tx[i] = (uint8_t)i;
    }
    memset(rx, 0, sizeof(rx));

    uart_flush_input(BRIDGE_UART_PORT);

    const int written = bridge_write(tx, sizeof(tx));
    if (written != (int)sizeof(tx)) {
        ESP_LOGE(TAG, "loopback: short write (%d of %u)", written, (unsigned)sizeof(tx));
        return;
    }

    const int n = uart_read_bytes(BRIDGE_UART_PORT, rx, sizeof(rx), pdMS_TO_TICKS(500));
    if (n != (int)sizeof(rx)) {
        ESP_LOGW(TAG, "loopback: read %d of %u bytes — is GPIO%d jumpered to GPIO%d?", n,
                 (unsigned)sizeof(rx), CONFIG_BRIDGE_UART_TX, CONFIG_BRIDGE_UART_RX);
        return;
    }

    if (memcmp(tx, rx, sizeof(tx)) != 0) {
        for (size_t i = 0; i < sizeof(tx); i++) {
            if (tx[i] != rx[i]) {
                ESP_LOGE(TAG, "loopback: MISMATCH at byte %u: sent 0x%02x got 0x%02x",
                         (unsigned)i, tx[i], rx[i]);
                break;
            }
        }
        return;
    }

    ESP_LOGI(TAG, "loopback: PASS — all 256 byte values survived the round trip");
}
#endif /* BRIDGE_ENABLE_LOOPBACK_TEST */

void app_main(void)
{
    ESP_LOGI(TAG, "router console bridge starting");
    report_hardware();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* A partition-table change or a full NVS. Erasing loses saved settings, which is
         * better than refusing to boot; the console can set them again. */
        ESP_LOGW(TAG, "nvs needs erasing (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(config_init());

    check_router_link();

    ESP_ERROR_CHECK(bridge_init());

    /* Non-fatal: a failed scrollback allocation must not cost the console (SPEC §5.6 is a
     * convenience feature under the interactive-only scope of §1.1). */
    if (bridge_ringlog_init(config_get()->ringlog_bytes) != ESP_OK) {
        ESP_LOGW(TAG, "continuing without scrollback");
    }

#ifdef BRIDGE_ENABLE_LOOPBACK_TEST
    ESP_LOGW(TAG, "loopback test enabled — the router MUST be disconnected");
    uart1_loopback_test();
#endif

    ESP_ERROR_CHECK(usb_cdc_start());

    /* Wi-Fi and TCP are best-effort: SPEC §5.4 requires that losing Path B never disturbs
     * Path A, and that starts with not aborting the boot when Wi-Fi will not come up. */
    if (wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "wifi failed to start; Path A only");
    } else if (net_tcp_start() != ESP_OK) {
        ESP_LOGE(TAG, "tcp server failed to start; Path A only");
    }

    ESP_ERROR_CHECK(console_cmd_start());

    ESP_LOGI(TAG, "ready. Path A: /dev/cu.usbmodem*  Path B: nc <ip> %d",
             TCP_CONSOLE_PORT);
    ESP_LOGI(TAG, "free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
}
