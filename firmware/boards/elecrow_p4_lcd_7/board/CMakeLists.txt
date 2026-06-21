# SPDX-License-Identifier: GPL-3.0-or-later
#
# `board` component for Elecrow CrowPanel Advanced 7" ESP32-P4.
# Implements main/board.h, main/display.h, main/touch.h.

idf_component_register(
    SRCS
        "board.cpp"
        "display.cpp"
        "touch.cpp"
        "../../common/backlight_pwm.cpp"
    INCLUDE_DIRS "../../../main"   # exports board.h / display.h / touch.h to deps
    PRIV_INCLUDE_DIRS "." "../../common"   # local board_pins.h
    REQUIRES
        driver
        esp_driver_ledc
        esp_hw_support
        esp_lcd
        esp_lvgl_port
        esp_psram
        lvgl__lvgl
)

# esp_lcd_touch_gt911 uses designated initialisers that leave some fields
# unset, which trips -Werror=missing-field-initializers under IDF 6.0.1.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=missing-field-initializers)
