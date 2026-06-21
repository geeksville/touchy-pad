// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared board bring-up for the "Cheap Yellow Display" (CYD2USB) family of
// classic-ESP32 boards (esp32_2432s028rv3, esp32_2432s024, ...).
//
// These boards have no I2C-attached peripherals on the touch path (the XPT2046
// touch panel rides its own SPI bus, set up in touch.cpp), so board_init()
// only quiesces the active-low RGB status LED and brings up the shared LEDC
// PWM backlight (boards/common/backlight_pwm) on BOARD_BL_GPIO.
//
// All board-specific values (pins, controller, orientation) live in each
// board's private board_pins.h, so this single translation unit serves every
// CYD variant.

#include "board.h"           // public API (main/board.h)
#include "tc_tag.h"
#include "board_pins.h"      // private pin map (per-board)
#include "platform.h"        // capability descriptor
#include "backlight_pwm.h"   // shared LEDC backlight driver (Stage 94)

#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = TOUCHY_TAG("board");

// No I2C touch on these boards; the GT911-style shared bus does not exist here.
i2c_master_bus_handle_t board_get_i2c_bus(void)
{
    return nullptr;
}

extern "C" void board_init(void)
{
    // Stage 94 — set up the LEDC PWM backlight (left off; backlight_init()
    // turns it on at the persisted brightness).
    backlight_pwm_init();

    // The RGB LED is common-anode (active-low). Drive all three high so it
    // starts off rather than glowing a random colour at boot.
    const gpio_num_t led_pins[] = {
        BOARD_LED_GPIO_R, BOARD_LED_GPIO_G, BOARD_LED_GPIO_B};
    uint64_t mask = 0;
    for (gpio_num_t p : led_pins) mask |= 1ULL << p;

    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    for (gpio_num_t p : led_pins) gpio_set_level(p, 1);  // off (active-low)

    ESP_LOGI(TAG, "CYD board init done (no I2C bus; RGB LED off)");
}

// Resistive XPT2046 single-touch panel on a classic ESP32 with no native USB.
extern "C" const struct Platform *platform_get(void)
{
    static const struct Platform p = {
        .is_multitouch = false,
        .has_usb = false,
    };
    return &p;
}
