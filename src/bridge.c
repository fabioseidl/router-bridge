#include "bridge.h"

#include <string.h>

#include "config.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bridge";

#ifndef CONFIG_BRIDGE_UART_TX
#define CONFIG_BRIDGE_UART_TX 17
#endif
#ifndef CONFIG_BRIDGE_UART_RX
#define CONFIG_BRIDGE_UART_RX 18
#endif

/* SPEC §5.2: large enough to absorb a full boot-log burst without overflow. */
#define UART_RX_BUF 16384
#define UART_TX_BUF 4096
#define UART_EVENT_QUEUE_LEN 32

/* One read syscall per wakeup; sized to the driver's typical delivery, not to the buffer. */
#define READ_CHUNK 512

#define MAX_SINKS 8

struct bridge_sink {
    char name[16];
    StreamBufferHandle_t sb;
    uint32_t drops;
    bool in_use;
};

static struct bridge_sink s_sinks[MAX_SINKS];
static SemaphoreHandle_t s_sinks_lock; /* guards the registry, not the stream buffers */
static SemaphoreHandle_t s_tx_lock;
static QueueHandle_t s_uart_events;
static bridge_stats_t s_stats;

/* --- Ring-log (SPEC §5.6) ----------------------------------------------------------- */

static uint8_t *s_log;
static size_t s_log_cap;
static size_t s_log_head; /* next write position */
static size_t s_log_used;
static SemaphoreHandle_t s_log_lock;

esp_err_t bridge_ringlog_init(size_t capacity)
{
    if (capacity == 0) {
        return ESP_OK; /* explicitly disabled */
    }

    /* SPEC §5.6 says PSRAM. Deliberately not falling back to internal RAM: 64 KB of
     * scrollback is a convenience feature and must never compete with Wi-Fi/lwIP for the
     * DMA-capable heap that config reserves in sdkconfig.defaults. */
    s_log = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM);
    if (s_log == NULL) {
        ESP_LOGE(TAG, "ringlog: %u bytes of PSRAM unavailable; scrollback disabled",
                 (unsigned)capacity);
        return ESP_ERR_NO_MEM;
    }

    s_log_lock = xSemaphoreCreateMutex();
    if (s_log_lock == NULL) {
        heap_caps_free(s_log);
        s_log = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_log_cap = capacity;
    s_log_head = 0;
    s_log_used = 0;
    ESP_LOGI(TAG, "ringlog: %u bytes in PSRAM", (unsigned)capacity);
    return ESP_OK;
}

static void ringlog_write(const uint8_t *data, size_t len)
{
    if (s_log == NULL) {
        return;
    }

    /* A burst larger than the whole log can only leave its tail. Skip the prefix rather
     * than wrapping over ourselves repeatedly. */
    if (len >= s_log_cap) {
        data += len - s_log_cap;
        len = s_log_cap;
    }

    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    const size_t to_end = s_log_cap - s_log_head;
    if (len <= to_end) {
        memcpy(s_log + s_log_head, data, len);
    } else {
        memcpy(s_log + s_log_head, data, to_end);
        memcpy(s_log, data + to_end, len - to_end);
    }
    s_log_head = (s_log_head + len) % s_log_cap;
    s_log_used = (s_log_used + len > s_log_cap) ? s_log_cap : s_log_used + len;
    xSemaphoreGive(s_log_lock);
}

size_t bridge_ringlog_dump(uint8_t *buf, size_t len)
{
    if (s_log == NULL) {
        return 0;
    }

    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    const size_t n = (len < s_log_used) ? len : s_log_used;
    /* Oldest first. The oldest byte sits `used` behind head, modulo capacity; when the
     * caller asks for less than we hold, give them the most recent n bytes. */
    const size_t start = (s_log_head + s_log_cap - n) % s_log_cap;
    const size_t to_end = s_log_cap - start;
    if (n <= to_end) {
        memcpy(buf, s_log + start, n);
    } else {
        memcpy(buf, s_log + start, to_end);
        memcpy(buf + to_end, s_log, n - to_end);
    }
    xSemaphoreGive(s_log_lock);
    return n;
}

size_t bridge_ringlog_used(void)
{
    return s_log_used;
}

void bridge_ringlog_clear(void)
{
    if (s_log == NULL) {
        return;
    }
    xSemaphoreTake(s_log_lock, portMAX_DELAY);
    s_log_head = 0;
    s_log_used = 0;
    xSemaphoreGive(s_log_lock);
}

/* --- Sinks -------------------------------------------------------------------------- */

bridge_sink_t *bridge_sink_register(const char *name, size_t capacity)
{
    xSemaphoreTake(s_sinks_lock, portMAX_DELAY);

    bridge_sink_t *sink = NULL;
    for (int i = 0; i < MAX_SINKS; i++) {
        if (!s_sinks[i].in_use) {
            sink = &s_sinks[i];
            break;
        }
    }
    if (sink == NULL) {
        xSemaphoreGive(s_sinks_lock);
        ESP_LOGE(TAG, "sink '%s': registry full (%d)", name, MAX_SINKS);
        return NULL;
    }

    /* Trigger level 1: wake the reader as soon as a single byte lands. SPEC §1.1 —
     * interactive latency beats batching. */
    sink->sb = xStreamBufferCreate(capacity, 1);
    if (sink->sb == NULL) {
        xSemaphoreGive(s_sinks_lock);
        ESP_LOGE(TAG, "sink '%s': no memory for %u bytes", name, (unsigned)capacity);
        return NULL;
    }

    strlcpy(sink->name, name, sizeof(sink->name));
    sink->drops = 0;
    sink->in_use = true;

    xSemaphoreGive(s_sinks_lock);
    ESP_LOGI(TAG, "sink '%s' registered (%u bytes)", name, (unsigned)capacity);
    return sink;
}

void bridge_sink_unregister(bridge_sink_t *sink)
{
    if (sink == NULL) {
        return;
    }
    xSemaphoreTake(s_sinks_lock, portMAX_DELAY);
    sink->in_use = false;
    StreamBufferHandle_t sb = sink->sb;
    sink->sb = NULL;
    xSemaphoreGive(s_sinks_lock);

    /* Deleted outside the lock: the fan-out only touches in_use sinks, and it holds the
     * same lock while deciding. */
    if (sb != NULL) {
        vStreamBufferDelete(sb);
    }
}

size_t bridge_sink_read(bridge_sink_t *sink, uint8_t *buf, size_t len, TickType_t ticks)
{
    if (sink == NULL || sink->sb == NULL) {
        return 0;
    }
    return xStreamBufferReceive(sink->sb, buf, len, ticks);
}

uint32_t bridge_sink_drops(const bridge_sink_t *sink)
{
    return sink ? sink->drops : 0;
}

const char *bridge_sink_name(const bridge_sink_t *sink)
{
    return sink ? sink->name : "?";
}

/*
 * Copy one chunk of router output to every sink.
 *
 * Zero timeout throughout. This is the guarantee that a wedged USB host or a stalled TCP
 * client cannot backpressure UART1 into an overflow (SPEC §5.3).
 */
static void fan_out(const uint8_t *data, size_t len)
{
    ringlog_write(data, len);

    xSemaphoreTake(s_sinks_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_SINKS; i++) {
        struct bridge_sink *s = &s_sinks[i];
        if (!s->in_use || s->sb == NULL) {
            continue;
        }
        const size_t sent = xStreamBufferSend(s->sb, data, len, 0);
        if (sent < len) {
            s->drops += (uint32_t)(len - sent);
        }
    }
    xSemaphoreGive(s_sinks_lock);
}

/*
 * UART1 reader.
 *
 * Event-driven rather than a bare uart_read_bytes loop, because SPEC §5.2 requires
 * overflows to be counted and never dropped silently — and FIFO overflow is only visible
 * as a driver event.
 */
static void uart_rx_task(void *arg)
{
    (void)arg;
    uint8_t *buf = malloc(READ_CHUNK);
    if (buf == NULL) {
        ESP_LOGE(TAG, "rx task: out of memory");
        vTaskDelete(NULL);
        return;
    }

    uart_event_t ev;
    for (;;) {
        if (xQueueReceive(s_uart_events, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (ev.type) {
        case UART_DATA: {
            size_t remaining = ev.size;
            while (remaining > 0) {
                const size_t want = (remaining < READ_CHUNK) ? remaining : READ_CHUNK;
                const int n = uart_read_bytes(BRIDGE_UART_PORT, buf, want, 0);
                if (n <= 0) {
                    break;
                }
                s_stats.router_to_host += (uint64_t)n;
                fan_out(buf, (size_t)n);
                remaining -= (size_t)n;
            }
            break;
        }

        case UART_FIFO_OVF:
            /* The hardware FIFO overran before the driver drained it. */
            s_stats.uart_overflow++;
            ESP_LOGW(TAG, "UART1 FIFO overflow (total %lu)",
                     (unsigned long)s_stats.uart_overflow);
            uart_flush_input(BRIDGE_UART_PORT);
            xQueueReset(s_uart_events);
            break;

        case UART_BUFFER_FULL:
            /* The driver's ring buffer filled because we did not read fast enough. */
            s_stats.uart_overflow++;
            ESP_LOGW(TAG, "UART1 ring buffer full (total %lu)",
                     (unsigned long)s_stats.uart_overflow);
            uart_flush_input(BRIDGE_UART_PORT);
            xQueueReset(s_uart_events);
            break;

        case UART_PARITY_ERR:
            s_stats.uart_parity_err++;
            break;

        case UART_FRAME_ERR:
            /* Usually a baud mismatch rather than noise. */
            s_stats.uart_frame_err++;
            break;

        default:
            break;
        }
    }
}

/* --- Line settings ------------------------------------------------------------------ */

static uart_word_length_t word_length(uint8_t data_bits)
{
    switch (data_bits) {
    case 5:
        return UART_DATA_5_BITS;
    case 6:
        return UART_DATA_6_BITS;
    case 7:
        return UART_DATA_7_BITS;
    default:
        return UART_DATA_8_BITS;
    }
}

static uart_parity_t parity_mode(char parity)
{
    switch (parity) {
    case 'E':
        return UART_PARITY_EVEN;
    case 'O':
        return UART_PARITY_ODD;
    default:
        return UART_PARITY_DISABLE;
    }
}

esp_err_t bridge_apply_line_settings(void)
{
    const bridge_config_t *cfg = config_get();

    ESP_RETURN_ON_ERROR(uart_set_baudrate(BRIDGE_UART_PORT, cfg->baud), TAG,
                        "uart_set_baudrate");
    ESP_RETURN_ON_ERROR(uart_set_word_length(BRIDGE_UART_PORT, word_length(cfg->data_bits)),
                        TAG, "uart_set_word_length");
    ESP_RETURN_ON_ERROR(uart_set_parity(BRIDGE_UART_PORT, parity_mode(cfg->parity)), TAG,
                        "uart_set_parity");
    ESP_RETURN_ON_ERROR(uart_set_stop_bits(BRIDGE_UART_PORT, cfg->stop_bits == 2
                                                                 ? UART_STOP_BITS_2
                                                                 : UART_STOP_BITS_1),
                        TAG, "uart_set_stop_bits");

    ESP_LOGI(TAG, "uart1: %lu baud %d%c%d", (unsigned long)cfg->baud, cfg->data_bits,
             cfg->parity, cfg->stop_bits);
    return ESP_OK;
}

/* --- TX ----------------------------------------------------------------------------- */

int bridge_write(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return 0;
    }

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    const int n = uart_write_bytes(BRIDGE_UART_PORT, data, len);
    if (n > 0) {
        s_stats.host_to_router += (uint64_t)n;
    }
    xSemaphoreGive(s_tx_lock);
    return n;
}

esp_err_t bridge_send_break(void)
{
    /* uart_write_bytes_with_break sends the break after the payload, so a zero-length
     * payload would be a no-op. One NUL is the conventional carrier; the break itself is
     * what the bootloader detects. Length is in bit periods. */
    const uint8_t nul = 0;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    const int n = uart_write_bytes_with_break(BRIDGE_UART_PORT, &nul, 1, 20);
    xSemaphoreGive(s_tx_lock);

    return (n == 1) ? ESP_OK : ESP_FAIL;
}

void bridge_get_stats(bridge_stats_t *out)
{
    if (out != NULL) {
        *out = s_stats;
    }
}

/* --- Init --------------------------------------------------------------------------- */

esp_err_t bridge_init(void)
{
    s_sinks_lock = xSemaphoreCreateMutex();
    s_tx_lock = xSemaphoreCreateMutex();
    if (s_sinks_lock == NULL || s_tx_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const bridge_config_t *cfg = config_get();
    const uart_config_t uc = {
        .baud_rate = (int)cfg->baud,
        .data_bits = word_length(cfg->data_bits),
        .parity = parity_mode(cfg->parity),
        .stop_bits = (cfg->stop_bits == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
        /* SPEC §5.2: router console headers carry no RTS/CTS. */
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(BRIDGE_UART_PORT, UART_RX_BUF, UART_TX_BUF,
                                            UART_EVENT_QUEUE_LEN, &s_uart_events, 0),
                        TAG, "uart_driver_install");
    ESP_RETURN_ON_ERROR(uart_param_config(BRIDGE_UART_PORT, &uc), TAG, "uart_param_config");
    ESP_RETURN_ON_ERROR(uart_set_pin(BRIDGE_UART_PORT, CONFIG_BRIDGE_UART_TX,
                                     CONFIG_BRIDGE_UART_RX, UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin");

    ESP_LOGW(TAG, "uart1 LIVE: tx=GPIO%d rx=GPIO%d, %lu baud %d%c%d — the TX pin now "
                  "reaches the router's console input",
             CONFIG_BRIDGE_UART_TX, CONFIG_BRIDGE_UART_RX, (unsigned long)cfg->baud,
             cfg->data_bits, cfg->parity, cfg->stop_bits);

    /* Priority above the sink tasks: draining UART1 promptly is what prevents the
     * overflow that acceptance criterion §8.9 measures. */
    if (xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 12, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
