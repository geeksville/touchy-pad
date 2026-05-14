# SPDX-License-Identifier: Apache-2.0
#
# `board` component for the Waveshare ESP32-S3-Touch-LCD-7B.
# Implements main/board.h, main/display.h, main/touch.h.
#
# include_dirs is "." so this component's `board_pins.h` is visible to its
# own .cpp files, AND the common API headers from main/ (board.h, display.h,
# touch.h) become visible to anything that REQUIRES this component (i.e.
# main itself) via the explicit PUB include below.

idf_component_register(
    SRCS
        "board.cpp"
        "display.cpp"
        "touch.cpp"
    INCLUDE_DIRS "../../../main"   # exports board.h / display.h / touch.h to deps
    PRIV_INCLUDE_DIRS "."       # local board_pins.h, internal helpers
    REQUIRES
        driver
        esp_lcd
        esp_lvgl_port
        esp_psram
        lvgl__lvgl
)

# The esp_lcd_touch_gt911 component's `ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG()`
# macro initialises only some fields of `esp_lcd_panel_io_i2c_config_t`,
# which trips `-Werror=missing-field-initializers` under IDF 6.0.1. Demote
# that to a warning for this component only.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=missing-field-initializers)
