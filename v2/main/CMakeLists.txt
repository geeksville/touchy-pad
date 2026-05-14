# SPDX-License-Identifier: Apache-2.0
#
# `main` is board-agnostic: it pulls in the app logic only. The currently
# selected board provides a `board` component (under boards/<board>/) that
# implements board.h / display.h / touch.h. We REQUIRE it here so the
# board's CMakeLists is parsed and its public headers are on our include
# path.

idf_component_register(
    SRCS
        "main.cpp"
        "usb_hid.cpp"
        "trackpad_widget.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        board
        esp_lcd
        esp_timer
)
