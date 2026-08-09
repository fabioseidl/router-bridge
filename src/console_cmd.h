/*
 * Out-of-band control channel (SPEC §5.5).
 *
 * On UART0 — the "COM" USB-C port — deliberately, so the data path stays transparent.
 * SPEC §5.1 forbids an in-band escape sequence: any byte reserved for "attention" would
 * corrupt a binary transfer, so configuration gets its own wire instead.
 */
#ifndef BRIDGE_CONSOLE_CMD_H
#define BRIDGE_CONSOLE_CMD_H

#include "esp_err.h"

esp_err_t console_cmd_start(void);

#endif /* BRIDGE_CONSOLE_CMD_H */
