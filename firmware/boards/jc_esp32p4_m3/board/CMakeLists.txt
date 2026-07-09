# SPDX-License-Identifier: GPL-3.0-or-later
#
# `board` component for the Guition JC-ESP32P4-M3 LED-matrix board (Stage
# LB1). Implements main/board.h, main/display.h, main/touch.h. The LVGL
# display is backed by a WS2812B LEDPanel (boards/common/led_panel.cpp).

idf_component_register(
    SRCS
        "board.cpp"
        "display.cpp"
        "touch.cpp"
        "../../common/led_panel.cpp"
    INCLUDE_DIRS "../../../main"   # exports board.h / display.h / touch.h to deps
    PRIV_INCLUDE_DIRS "." "../../common"   # local board_pins.h / led_panel.h
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
