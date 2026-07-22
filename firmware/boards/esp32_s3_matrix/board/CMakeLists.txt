# SPDX-License-Identifier: GPL-3.0-or-later
#
# `board` component for the Waveshare ESP32-S3-Matrix LED-matrix board
# (Stage matrix1). Implements main/board.h, main/display.h, main/touch.h.
# The LVGL display + brightness seam are the shared WS2812B driver in
# boards/common/leds/ (panel GPIO / geometry come from the persisted
# BoardConfig preference — Stage lb6 — not board_pins.h).

idf_component_register(
    SRCS
        "board.cpp"
        "../../../boards/common/leds/led_display.cpp"
        "touch.cpp"
        "../../../boards/common/leds/led_panel.cpp"
    PRIV_INCLUDE_DIRS "." "../../../boards/common/leds"
    REQUIRES
        main
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
