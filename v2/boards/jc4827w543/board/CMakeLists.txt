# SPDX-License-Identifier: Apache-2.0
#
# `board` component for the JC4827W543 4.3-inch ESP32-S3 board.

idf_component_register(
    SRCS
        "board.cpp"
        "display.cpp"
        "nv3041a.c"
        "touch.cpp"
    INCLUDE_DIRS "../../../main"   # exports board.h / display.h / touch.h
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_spi
        esp_lcd
        esp_lvgl_port
        esp_psram
        lvgl__lvgl
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=missing-field-initializers)
