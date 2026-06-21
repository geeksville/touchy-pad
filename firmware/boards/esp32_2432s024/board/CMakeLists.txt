# SPDX-License-Identifier: GPL-3.0-or-later
#
# `board` component for the ESP32-2432S024 ("Cheap Yellow Display", 2.4"
# CYD2USB) — classic ESP32, ILI9341 SPI display + XPT2046 SPI touch, no PSRAM.
#
# The C++ implementation is shared across the whole CYD family and lives in
# boards/cyd_common/; only board_pins.h is board-specific. The shared sources
# are referenced by relative path so each variant compiles them against its
# own pin map (PRIV_INCLUDE_DIRS "." puts this dir's board_pins.h first). The
# ILI9341 vs ST7789 panel driver is selected at compile time from board_pins.h
# (BOARD_LCD_CONTROLLER_ILI9341).

idf_component_register(
    SRCS
        "../../cyd_common/board.cpp"
        "../../cyd_common/display.cpp"
        "../../cyd_common/touch.cpp"
        "../../common/backlight_pwm.cpp"
    INCLUDE_DIRS "../../../main"   # exports board.h / display.h / touch.h / platform.h
    PRIV_INCLUDE_DIRS "." "../../common"
    REQUIRES
        driver
        esp_driver_spi
        esp_driver_ledc
        esp_lcd
        esp_lvgl_port
        lvgl__lvgl
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=missing-field-initializers)
