/*
 * UART1 <-> sinks fan-out (SPEC §4, §5.1, §5.2).
 *
 * One reader task owns UART1 RX and copies every byte to every registered sink. Sinks are
 * decoupled by a per-sink stream buffer, which is what makes SPEC §5.3's "a full CDC TX
 * buffer must never stall UART1 RX" structurally true rather than a matter of care: the
 * fan-out only ever does a zero-timeout send, and a sink that cannot keep up loses bytes
 * and increments its own drop counter.
 *
 * The transport is 8-bit clean (SPEC §5.1). Nothing in this module inspects, translates
 * or escapes a byte. Telnet IAC handling, if enabled, lives in net_tcp.c where it belongs
 * to one sink rather than to the pipe.
 */
#ifndef BRIDGE_H
#define BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

/* SPEC §2.4 / §2.5: UART1 on the safe pins, router console link. */
#define BRIDGE_UART_PORT UART_NUM_1

typedef struct bridge_sink bridge_sink_t;

typedef struct {
    uint64_t router_to_host; /* bytes read from UART1 */
    uint64_t host_to_router; /* bytes written to UART1 */
    uint32_t uart_overflow;  /* UART_FIFO_OVF + UART_BUFFER_FULL events */
    uint32_t uart_parity_err;
    uint32_t uart_frame_err;
} bridge_stats_t;

/* Brings up UART1 from the persisted line settings and starts the reader task.
 * Assigns BOTH pins: unlike the step-2 skeleton this is a bidirectional bridge, so the
 * TX pin is live from here on and anything written reaches the router's console. */
esp_err_t bridge_init(void);

/*
 * Registers a sink. `capacity` is the stream buffer size in bytes; size it to the burst
 * the sink must absorb while its own task is descheduled.
 *
 * Returns NULL if memory is short. The caller's task then reads with bridge_sink_read().
 */
bridge_sink_t *bridge_sink_register(const char *name, size_t capacity);
void bridge_sink_unregister(bridge_sink_t *sink);

/* Blocks up to `ticks` for at least one byte. Returns bytes read, 0 on timeout. */
size_t bridge_sink_read(bridge_sink_t *sink, uint8_t *buf, size_t len, TickType_t ticks);

/* Bytes this sink lost because its buffer was full when the fan-out ran. */
uint32_t bridge_sink_drops(const bridge_sink_t *sink);
const char *bridge_sink_name(const bridge_sink_t *sink);

/*
 * Writes to the router. Serialised by a mutex, so concurrent writers from USB and TCP
 * interleave at whole-call granularity rather than mid-byte (SPEC §4).
 *
 * DANGER: this is the router's console input. SPEC §9 — a destructive command typed here
 * is transmitted faithfully, and writing to a bootloader console can brick the router.
 */
int bridge_write(const uint8_t *data, size_t len);

/* SPEC §5.2: a UART break, needed to interrupt some bootloaders. */
esp_err_t bridge_send_break(void);

/* Applies new line settings to the live UART. config_set_* persists them; this makes
 * them take effect without a reboot. */
esp_err_t bridge_apply_line_settings(void);

void bridge_get_stats(bridge_stats_t *out);

/* --- Scrollback ring-log, SPEC §5.6 ------------------------------------------------ */

/* Allocated in PSRAM. Returns ESP_ERR_NO_MEM and leaves the log disabled if PSRAM is
 * unavailable; the bridge itself keeps working. */
esp_err_t bridge_ringlog_init(size_t capacity);

/* Copies at most `len` bytes of the most recent output into `buf`, oldest first.
 * Returns the number copied. */
size_t bridge_ringlog_dump(uint8_t *buf, size_t len);
size_t bridge_ringlog_used(void);
void bridge_ringlog_clear(void);

#endif /* BRIDGE_H */
