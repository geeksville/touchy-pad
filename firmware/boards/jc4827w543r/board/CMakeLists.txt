# SPDX-License-Identifier: GPL-3.0-or-later
#
# `board` component for the JC4827W543R 4.3-inch ESP32-S3 board.

idf_component_register(
    SRCS
        "board.cpp"
        "../../jc4827w543/board/display.cpp"
        "../../jc4827w543/board/nv3041a.c"
        "touch.cpp"
        "../../common/backlight_pwm.cpp"
    INCLUDE_DIRS "../../../main"   # exports board.h / display.h / touch.h
    PRIV_INCLUDE_DIRS "." "../../common"
    REQUIRES
        driver
        esp_driver_spi
        esp_driver_ledc
        esp_lcd
        esp_lvgl_port
        esp_psram
        lvgl__lvgl
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=missing-field-initializers)
