# PlatformIO's espidf layout puts the main component in src/, not main/.
#
# REQUIRES names the *component*, which is not the same as the include path.
# In ESP-IDF 5.5.3 driver/uart.h is owned by esp_driver_uart, not by the legacy
# `driver` component (which now only carries i2c, touch_sensor, twai and deprecated
# shims). See docs/hardware.md §2.3.
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_uart esp_driver_gpio esp_psram
)
