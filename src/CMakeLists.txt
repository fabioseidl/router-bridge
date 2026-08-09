# PlatformIO's espidf layout puts the main component in src/, not main/.
#
# REQUIRES names the *component*, which is not the same as the include path.
# In ESP-IDF 5.5.3 driver/uart.h is owned by esp_driver_uart and
# driver/usb_serial_jtag.h by esp_driver_usb_serial_jtag, not by the legacy `driver`
# component (which now only carries i2c, touch_sensor, twai and deprecated shims).
# See docs/hardware.md §2.3 — SPEC §0 states the opposite and is wrong for 5.5.3.
idf_component_register(
    SRCS
        "main.c"
        "config.c"
        "bridge.c"
        "usb_cdc.c"
        "wifi.c"
        "net_tcp.c"
        "console_cmd.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_uart
        esp_driver_gpio
        esp_driver_usb_serial_jtag
        esp_psram
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        nvs_flash
        console
        lwip
)
