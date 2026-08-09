#include "usb_cdc.h"

#include "bridge.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "usb";

/* Enough to hold a boot-log burst while macOS is not reading. Larger than the driver's
 * own TX ring, which is the point: the deep buffer lives on our side where a full one
 * costs a counted drop rather than a stalled UART reader. */
#define USB_SINK_BYTES 8192
#define USB_DRIVER_TX_BUF 2048
#define USB_DRIVER_RX_BUF 1024
#define USB_CHUNK 256

/*
 * How long to wait for room in the driver's TX ring before giving up on a chunk.
 *
 * Not zero: with no wait at all, a host that is merely scheduling slowly would shed bytes
 * constantly. Not long either — SPEC §5.3 forbids blocking the fan-out, and this task is
 * a sink reader, so a long block here just backs up into our own stream buffer and shows
 * as a drop one layer up. 20 ms rides out normal host scheduling jitter and nothing more.
 */
#define USB_TX_WAIT_MS 20

static bridge_sink_t *s_sink;
static uint32_t s_tx_drops;

uint32_t usb_cdc_tx_drops(void)
{
    /* The sink's own drops (fan-out could not enqueue) plus ours (driver would not take
     * it). Both mean the same thing to a user: bytes the Mac never saw. */
    return s_tx_drops + bridge_sink_drops(s_sink);
}

/* Router -> Mac. */
static void usb_tx_task(void *arg)
{
    (void)arg;
    uint8_t buf[USB_CHUNK];

    for (;;) {
        const size_t n = bridge_sink_read(s_sink, buf, sizeof(buf), portMAX_DELAY);
        if (n == 0) {
            continue;
        }

        size_t off = 0;
        while (off < n) {
            const int w = usb_serial_jtag_write_bytes(buf + off, n - off,
                                                      pdMS_TO_TICKS(USB_TX_WAIT_MS));
            if (w <= 0) {
                /* No host, or a host that has stopped reading. Drop the rest of this
                 * chunk and move on — SPEC §5.3: never block, never stall UART1. */
                s_tx_drops += (uint32_t)(n - off);
                break;
            }
            off += (size_t)w;
        }
    }
}

/* Mac -> router. */
static void usb_rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[USB_CHUNK];

    for (;;) {
        const int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), portMAX_DELAY);
        if (n > 0) {
            /* Straight through. No echo, no line buffering, no CR/LF translation —
             * SPEC §5.1, and the router's shell does its own echo. */
            bridge_write(buf, (size_t)n);
        }
    }
}

esp_err_t usb_cdc_start(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = USB_DRIVER_TX_BUF,
        .rx_buffer_size = USB_DRIVER_RX_BUF,
    };
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&cfg), TAG,
                        "usb_serial_jtag_driver_install");

    s_sink = bridge_sink_register("usb", USB_SINK_BYTES);
    if (s_sink == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(usb_tx_task, "usb_tx", 3072, NULL, 10, NULL) != pdPASS ||
        xTaskCreate(usb_rx_task, "usb_rx", 3072, NULL, 10, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Path A up: USB Serial/JTAG <-> UART1");
    return ESP_OK;
}
