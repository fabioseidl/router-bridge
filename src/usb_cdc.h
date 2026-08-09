/*
 * Path A — the MacBook's console over the ESP32-S3's native USB (SPEC §5.3).
 *
 * USB Serial/JTAG, not TinyUSB. SPEC §5.3 settles this: the controller is in the silicon,
 * needs no IDF Component Manager dependency under PlatformIO, and enumerates on macOS as
 * a CDC device with no driver install.
 *
 * The port appears as /dev/cu.usbmodem* and is opened with screen/picocom — never with
 * `pio device monitor`, which is UART0 and carries diagnostics only.
 */
#ifndef BRIDGE_USB_CDC_H
#define BRIDGE_USB_CDC_H

#include <stdint.h>

#include "esp_err.h"

esp_err_t usb_cdc_start(void);

/* Bytes dropped because the host was not draining the CDC endpoint. */
uint32_t usb_cdc_tx_drops(void);

#endif /* BRIDGE_USB_CDC_H */
