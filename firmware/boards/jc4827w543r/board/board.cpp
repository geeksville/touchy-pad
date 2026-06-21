// SPDX-License-Identifier: GPL-3.0-or-later
//
// Board bring-up for the JC4827W543R. This board has no IO-expander. The
// display backlight is driven from a real GPIO and is configured in display.cpp;
// the XPT2046 touch SPI bus is initialised in touch.cpp.

#include "board.h"           // public API (main/board.h)
#include "tc_tag.h"
#include "board_pins.h"      // private pin map
#include "platform.h"        // capability descriptor
#include "backlight_pwm.h"   // shared LEDC backlight driver (Stage 94)

#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = TOUCHY_TAG("board");

extern "C" i2c_master_bus_handle_t board_get_i2c_bus(void)
{
    return nullptr;
}

extern "C" void board_init(void)
{
    // Stage 94 — set up the LEDC PWM backlight (left off; backlight_init()
    // turns it on at the persisted brightness).
    backlight_pwm_init();
    ESP_LOGI(TAG, "Initialising JC4827W543R board");
}

// Resistive XPT2046 single-touch panel on a native-USB ESP32-S3.
extern "C" const struct Platform *platform_get(void)
{
    static const struct Platform p = {
        .is_multitouch = false,
        .has_usb = true,
    };
    return &p;
}
