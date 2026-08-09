/*
 * ESP32-S3 Router Serial Console Bridge — step 2 skeleton (SPEC §10.2).
 *
 * This is NOT the bridge yet. It exists to prove three things before any of the real
 * functionality is written:
 *
 *   1. The toolchain builds and the image boots.
 *   2. PSRAM initialises, at what size and in what mode.
 *   3. The UART1 driver path works against the real router.
 *
 * ---------------------------------------------------------------------------
 * SAFETY: this firmware is READ-ONLY on UART1 by default.
 *
 * GPIO17 (our TX) is wired to the router's console RX. Anything written there is
 * keystrokes as far as the router is concerned. SPEC §9 is explicit that writing to a
 * bootloader console can brick the router, and a byte pattern sweeping 0x00-0xFF
 * contains CR and LF — i.e. it would type junk into a live shell and press Enter.
 * So nothing is ever transmitted unless BRIDGE_ENABLE_LOOPBACK_TEST is defined, and
 * that must only be done with the router physically disconnected.
 *
 * That guarantee covers UART1 only, and it is void if the router is miswired to the
 * devkit pins silkscreened "TX"/"RX". Those are UART0 = GPIO43/44, which this firmware
 * and the ROM bootloader both transmit on unconditionally. See hardware.md §4.5.
 * ---------------------------------------------------------------------------
 *
 * All log output goes to UART0 (the "COM" USB-C port). Nothing is written to USB
 * Serial/JTAG — that is Path A, reserved for router data.
 */

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bridge";

/* Router link. Set from platformio.ini build_flags; the defaults here keep the file
 * self-contained if it is ever built outside PlatformIO. GPIO17/18 are in the
 * "safe for use" set for the N16R8 module (SPEC §2.3). */
#ifndef CONFIG_BRIDGE_UART_TX
#define CONFIG_BRIDGE_UART_TX 17
#endif
#ifndef CONFIG_BRIDGE_UART_RX
#define CONFIG_BRIDGE_UART_RX 18
#endif

#define BRIDGE_UART_PORT UART_NUM_1

/* SPEC §5.2: large enough to absorb a full boot-log burst without overflow. */
#define BRIDGE_UART_RX_BUF 8192
#define BRIDGE_UART_TX_BUF 4096

/* Candidate console rates, in the order SPEC §2.4 says they occur in the wild. */
static const int kCandidateBauds[] = {115200, 57600, 38400, 9600};

/* How long to listen at each candidate rate while probing. */
#define BAUD_PROBE_MS 2500

/* Report what the silicon actually is, rather than what the silkscreen claims. */
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

    /* Acceptance criterion §8.2. If this reports less than 8 MB, or reports nothing,
     * the fitted part is not the octal 8 MB the "R8" marking claims and
     * CONFIG_SPIRAM_MODE_OCT in sdkconfig.defaults must be revisited. */
    if (esp_psram_is_initialized()) {
        size_t psram = esp_psram_get_size();
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
 * This is what distinguishes "the router is simply idle" from "that wire isn't
 * connected to what we think it is".
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
 * Classify every pin that is safe to touch on this module, not just GPIO17/18.
 *
 * Written to resolve the hardware.md §4.5 contradiction — the router's J1 pads measured
 * 3.3 V while GPIO17 and GPIO18 both read FLOATING — and it did, by elimination. No pin
 * in the safe set was driven, which ruled out a mislabelled GPIO and pointed at the two
 * pins this scan cannot cover.
 *
 * Excluded deliberately: 0/3/45/46 (strapping), 19/20 (native USB, Path A),
 * 26-32 (in-package flash), 33-37 (octal PSRAM), 43/44 (UART0 — this log), 48 (LED).
 * See wiring.md "Pin budget on this module".
 *
 * GPIO43/44 cannot be sampled here: they carry the log this function reports through,
 * and reconfiguring them as plain inputs would cut the diagnostic mid-sentence. That
 * blind spot is exactly where the fault was, so the all-float branch below names it
 * explicitly rather than leaving the reader to work it out.
 */
static const int kSafePins[] = {1,  2,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13,
                                14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 47};

static void scan_all_safe_pins(void)
{
    ESP_LOGI(TAG, "--- full pin scan: which pin is the router actually driving? ---");

    int driven_high = 0;
    bool rx_driven = false;
    for (size_t i = 0; i < sizeof(kSafePins) / sizeof(kSafePins[0]); i++) {
        const int pin = kSafePins[i];
        const char *state = classify_pin(pin);

        /* Only the interesting ones deserve a line each; a wall of FLOATING helps nobody. */
        if (state[0] != 'F') {
            ESP_LOGW(TAG, "GPIO%-2d: %s", pin, state);
            driven_high++;
            rx_driven |= (pin == CONFIG_BRIDGE_UART_RX);
        }
    }

    if (driven_high == 0) {
        ESP_LOGE(TAG, "scan: ALL %u safe pins float — nothing reaches the die here.",
                 (unsigned)(sizeof(kSafePins) / sizeof(kSafePins[0])));
        /* The historical root cause. Worth spelling out every time, because the symptom
         * is indistinguishable from a genuinely disconnected wire. */
        ESP_LOGE(TAG, "scan: CHECK THE PINS SILKSCREENED 'TX' AND 'RX' FIRST. On this "
                      "devkit those are UART0 = GPIO43/44, NOT GPIO%d/%d. A router wired "
                      "there is invisible to this scan, feeds the router's RX with this "
                      "very boot log, and fights the FT232R for GPIO44. Move the wires "
                      "to the pins numbered 17 and 18 on the opposite header.",
                 CONFIG_BRIDGE_UART_TX, CONFIG_BRIDGE_UART_RX);
        ESP_LOGE(TAG, "scan: if the wires are already on GPIO%d/%d, then the break is "
                      "between the router pad and the die, or GND is not shared.",
                 CONFIG_BRIDGE_UART_TX, CONFIG_BRIDGE_UART_RX);
    } else if (rx_driven) {
        /* The healthy state is two driven pins: the router's TX on our RX, and the
         * router's own pull-up on its RX showing through on our TX. */
        ESP_LOGI(TAG, "scan: %d pin(s) driven, including GPIO%d — the router link is "
                      "connected as configured.",
                 driven_high, CONFIG_BRIDGE_UART_RX);
    } else {
        ESP_LOGW(TAG, "scan: %d pin(s) driven, but NOT GPIO%d. The wire is on a different "
                      "pin than the silkscreen claims — repoint CONFIG_BRIDGE_UART_RX in "
                      "platformio.ini at the driven pin listed above.",
                 driven_high, CONFIG_BRIDGE_UART_RX);
    }
}

/*
 * Bring UART1 up for receive only.
 *
 * No TX pin is assigned at all. That is stronger than merely never calling
 * uart_write_bytes(): an unassigned TX cannot drive the router's console RX even
 * transiently, and if the wiring turns out to be swapped it cannot contend with the
 * router's TX output driver either.
 */
static esp_err_t uart1_init_rx_only(int baud, int rx_pin)
{
    const uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        /* Router console headers carry no RTS/CTS (SPEC §5.2). */
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(BRIDGE_UART_PORT, BRIDGE_UART_RX_BUF,
                                            BRIDGE_UART_TX_BUF, 0, NULL, 0),
                        TAG, "uart_driver_install");
    ESP_RETURN_ON_ERROR(uart_param_config(BRIDGE_UART_PORT, &cfg), TAG,
                        "uart_param_config");
    ESP_RETURN_ON_ERROR(uart_set_pin(BRIDGE_UART_PORT, UART_PIN_NO_CHANGE, rx_pin,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin");

    ESP_LOGI(TAG, "uart1: %d baud 8N1, rx=GPIO%d, TX UNASSIGNED (receive-only)", baud,
             rx_pin);
    return ESP_OK;
}

/* Print a buffer as an escaped one-liner so control bytes stay visible in the UART0
 * log without corrupting it. */
static void log_readable(const uint8_t *buf, size_t len)
{
    char out[256];
    size_t o = 0;

    for (size_t i = 0; i < len && o + 5 < sizeof(out); i++) {
        const uint8_t c = buf[i];
        if (c == '\n') {
            o += (size_t)snprintf(out + o, sizeof(out) - o, "\\n");
        } else if (c == '\r') {
            o += (size_t)snprintf(out + o, sizeof(out) - o, "\\r");
        } else if (isprint(c)) {
            out[o++] = (char)c;
            out[o] = '\0';
        } else {
            o += (size_t)snprintf(out + o, sizeof(out) - o, "\\x%02x", c);
        }
    }
    ESP_LOGI(TAG, "rx: %s", out);
}

/*
 * Listen at each candidate baud and score how much of the traffic looks like text.
 * A correct baud yields mostly printable ASCII; a wrong one yields high-bit garbage.
 * Purely passive — answers the "what baud is this console?" question from SPEC §2.4
 * without transmitting anything to the router.
 *
 * Returns the best-scoring baud, or 0 if the router sent nothing at any rate.
 */
static int probe_router_baud(int rx_pin)
{
    uint8_t buf[512];
    int best_baud = 0;
    int best_score = 0;

    ESP_LOGI(TAG, "--- probing with rx=GPIO%d ---", rx_pin);
    ESP_ERROR_CHECK(uart_set_pin(BRIDGE_UART_PORT, UART_PIN_NO_CHANGE, rx_pin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    for (size_t i = 0; i < sizeof(kCandidateBauds) / sizeof(kCandidateBauds[0]); i++) {
        const int baud = kCandidateBauds[i];

        ESP_ERROR_CHECK(uart_set_baudrate(BRIDGE_UART_PORT, (uint32_t)baud));
        ESP_ERROR_CHECK(uart_flush_input(BRIDGE_UART_PORT));

        size_t total = 0;
        size_t printable = 0;
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(BAUD_PROBE_MS);

        while (xTaskGetTickCount() < deadline) {
            const int n = uart_read_bytes(BRIDGE_UART_PORT, buf, sizeof(buf),
                                          pdMS_TO_TICKS(200));
            if (n <= 0) {
                continue;
            }
            if (total == 0) {
                log_readable(buf, (size_t)n);
            }
            total += (size_t)n;
            for (int k = 0; k < n; k++) {
                if (isprint(buf[k]) || buf[k] == '\r' || buf[k] == '\n' ||
                    buf[k] == '\t') {
                    printable++;
                }
            }
        }

        const int pct = total ? (int)((printable * 100) / total) : 0;
        ESP_LOGI(TAG, "probe %6d baud: %u bytes, %d%% printable", baud, (unsigned)total,
                 pct);

        /* Require a meaningful sample; a handful of bytes proves nothing. */
        if (total >= 16 && pct > best_score) {
            best_score = pct;
            best_baud = baud;
        }
    }

    if (best_baud == 0) {
        ESP_LOGW(TAG, "no traffic on GPIO%d at any candidate baud", rx_pin);
    } else {
        ESP_LOGI(TAG, "GPIO%d: best guess %d baud (%d%% printable)", rx_pin, best_baud,
                 best_score);
    }
    return best_baud;
}

#ifdef BRIDGE_ENABLE_LOOPBACK_TEST
/*
 * Write all 256 byte values and read them back, proving the driver path is 8-bit clean.
 * DANGEROUS with the router attached — see the safety note at the top of this file.
 * Only build this with a GPIO17<->GPIO18 jumper and the router disconnected.
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

    const int written = uart_write_bytes(BRIDGE_UART_PORT, tx, sizeof(tx));
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
    ESP_LOGI(TAG, "router console bridge — step 2 skeleton (UART1 read-only)");
    report_hardware();

    /* Do this before the UART claims either pin. */
    ESP_LOGI(TAG, "pin check GPIO%d (expected: router TX -> our RX): %s",
             CONFIG_BRIDGE_UART_RX, classify_pin(CONFIG_BRIDGE_UART_RX));
    ESP_LOGI(TAG, "pin check GPIO%d (expected: our TX -> router RX): %s",
             CONFIG_BRIDGE_UART_TX, classify_pin(CONFIG_BRIDGE_UART_TX));

    scan_all_safe_pins();

    if (uart1_init_rx_only(kCandidateBauds[0], CONFIG_BRIDGE_UART_RX) != ESP_OK) {
        ESP_LOGE(TAG, "uart1 init failed; stopping");
        return;
    }

#ifdef BRIDGE_ENABLE_LOOPBACK_TEST
    ESP_LOGW(TAG, "loopback test enabled — the router MUST be disconnected");
    uart1_loopback_test();
#endif

    /* Nominal orientation first. */
    int rx_pin = CONFIG_BRIDGE_UART_RX;
    int baud = probe_router_baud(rx_pin);

    /* Nothing there? Try the other pin. If TX and RX were swapped at the header, the
     * router's output is arriving on GPIO17 instead. Safe to test because no TX pin is
     * assigned, so we cannot contend with the router's output driver. */
    if (baud == 0) {
        rx_pin = CONFIG_BRIDGE_UART_TX;
        baud = probe_router_baud(rx_pin);
        if (baud != 0) {
            ESP_LOGW(TAG, "TX/RX ARE SWAPPED: the router's TX is on GPIO%d, not GPIO%d. "
                          "Swap the two signal wires, or swap the pin numbers in "
                          "platformio.ini.",
                     rx_pin, CONFIG_BRIDGE_UART_RX);
        }
    }

    if (baud != 0) {
        ESP_ERROR_CHECK(uart_set_baudrate(BRIDGE_UART_PORT, (uint32_t)baud));
        ESP_ERROR_CHECK(uart_set_pin(BRIDGE_UART_PORT, UART_PIN_NO_CHANGE, rx_pin,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    } else {
        ESP_LOGW(TAG, "no traffic on either pin. Check the pin-check lines above: if "
                      "both read FLOATING, neither wire reaches a live router output — "
                      "and re-read the pin scan's advice about the pins silkscreened "
                      "'TX'/'RX'. If GPIO%d reads 'driven HIGH', the link is fine and "
                      "the router is simply idle — power-cycle it to force a boot log.",
                 CONFIG_BRIDGE_UART_RX);
        /* Keep listening on the nominal pin anyway. */
        ESP_ERROR_CHECK(uart_set_pin(BRIDGE_UART_PORT, UART_PIN_NO_CHANGE,
                                     CONFIG_BRIDGE_UART_RX, UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE));
    }

    ESP_LOGI(TAG, "free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "streaming router output below (power-cycle the router to capture a "
                  "boot log)");

    /* Passive tap: echo whatever the router says onto the UART0 diagnostic log. */
    uint8_t buf[256];
    for (;;) {
        const int n = uart_read_bytes(BRIDGE_UART_PORT, buf, sizeof(buf),
                                      pdMS_TO_TICKS(100));
        if (n > 0) {
            log_readable(buf, (size_t)n);
        }
    }
}
