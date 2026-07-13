# SPDX-License-Identifier: GPL-3.0-or-later
#
# `board` component for the Adafruit ESP32-S3 Feather LED-matrix board
# (Stage LB2). Implements main/board.h, main/display.h, main/touch.h. The
# LVGL display + brightness seam are the shared WS2812B driver in
# main/leds/led_display.cpp (panel GPIO / geometry come from the persisted
# BoardConfig preference — Stage lb6 — not board_pins.h). The LED shared
# sources now live in firmware/boards/common/leds/ (moved from main/leds/
# in Stage lb6 to silence IDF's cross-component source ownership warning).

idf_component_register(
    SRCS
        "board.cpp"
        "../../../boards/common/leds/led_display.cpp"
        "touch.cpp"
        "../../../boards/common/leds/led_panel.cpp"
    PRIV_INCLUDE_DIRS "." "../../../boards/common/leds"   # local board_pins.h / led_panel.h / led_display.h
    REQUIRES
        main   # seam headers board.h / display.h / touch.h live in main
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
