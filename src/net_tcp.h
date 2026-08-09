/*
 * Path B — the console over a raw TCP socket (SPEC §5.4).
 *
 * Raw bytes, not telnet option negotiation. A stock `telnet` client will mostly work, but
 * `nc <ip> 23` is the documented client because it does not inject IAC sequences into a
 * transparent pipe.
 */
#ifndef BRIDGE_NET_TCP_H
#define BRIDGE_NET_TCP_H

#include <stdint.h>

#include "esp_err.h"

/* SPEC §5.4: default port 23. */
#define TCP_CONSOLE_PORT 23

/* SPEC §5.4: "at least 2 simultaneous clients". */
#define TCP_MAX_CLIENTS 4

esp_err_t net_tcp_start(void);

uint8_t net_tcp_client_count(void);
uint32_t net_tcp_tx_drops(void);

#endif /* BRIDGE_NET_TCP_H */
