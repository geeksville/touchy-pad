// SPDX-License-Identifier: GPL-3.0-or-later
//
// Board bring-up for the Guition JC-ESP32P4-M3 LED-matrix board (Stage
// LB1). Display-less / touch-less: there is no shared I2C bus and no touch
// controller. The LED panel itself is created by display_init(); board_init
// only handles the (currently minimal) board-wide setup.

#include "board.h"
#include "board_pins.h"
#include "led_display.h"
#include "platform.h"

#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = TOUCHY_TAG("board");

i2c_master_bus_handle_t board_get_i2c_bus(void) { return nullptr; }

extern "C" void board_init(void)
{
#ifdef BOARD_C6_RESET_GPIO
    // Hold the ESP32-C6 WiFi companion in reset (WiFi unused in LB1).
    gpio_set_direction(BOARD_C6_RESET_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_C6_RESET_GPIO, 0);
#endif
    ESP_LOGI(TAG, "Board init done (ESP32-P4, LED matrix)");
}

// Stage 94 backlight API. This board has no LCD backlight; "brightness"
// maps to the whole LED matrix's global brightness scalar. Forward to the
// display driver, which owns the panel.
void backlight_set(uint8_t level)
{
    led_display_set_brightness(level);
}

// Stage LB1 — strong override of the weak platform_is_touchable() default:
// this board has no touch panel, so it selects the touch-less built-in
// default screen and tells the host to hide touch-only UI.
extern "C" bool platform_is_touchable(void) { return false; }

extern "C" const struct Platform *platform_get(void)
{
    static const struct Platform p = { .is_multitouch = false, .has_usb = true };
    return &p;
}
