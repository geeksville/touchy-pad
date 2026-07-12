# SPDX-License-Identifier: GPL-3.0-or-later
#
# `board` component for the Adafruit ESP32-S3 Feather LED-matrix board
# (Stage LB2). Implements main/board.h, main/display.h, main/touch.h. The
# LVGL display + brightness seam are the shared WS2812B driver in
# main/leds/led_display.cpp (board_pins.h supplies the GPIO / geometry).

idf_component_register(
    SRCS
        "board.cpp"
        "../../../main/leds/led_display.cpp"
        "touch.cpp"
        "../../../main/leds/led_panel.cpp"
    INCLUDE_DIRS "../../../main"   # exports board.h / display.h / touch.h to deps
    PRIV_INCLUDE_DIRS "." "../../../main/leds"   # local board_pins.h / led_panel.h / led_display.h
    REQUIRES
        driver
        esp_hw_support
        esp_lcd
        esp_lvgl_port
        esp_psram
        lvgl__lvgl
)

# led_strip's config structs use designated initialisers that leave some
# fields unset, which trips -Werror=missing-field-initializers under IDF 6.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=missing-field-initializers)
