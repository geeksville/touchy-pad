# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS
        "main.cpp"
        "board.cpp"
        "display.cpp"
        "touch.cpp"
        "usb_hid.cpp"
        "trackpad_widget.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_lcd
        esp_timer
        esp_psram
)

# The esp_lcd_touch_gt911 component's `ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG()`
# macro initialises only some fields of `esp_lcd_panel_io_i2c_config_t`, which
# trips `-Werror=missing-field-initializers` under IDF 6.0.1.  Demote it to a
# warning for our component only.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=missing-field-initializers)
