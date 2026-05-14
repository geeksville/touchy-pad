# SPDX-License-Identifier: Apache-2.0
# ESP-IDF top-level project file for touchy-pad v2.
#
# This project supports multiple hardware targets that share the same app
# code (`main/`). Each supported board lives under `boards/<name>/` as a
# self-contained ESP-IDF component that implements the board API declared in
# `main/board.h` / `main/display.h` / `main/touch.h`.
#
# Selecting a board:
#
#   idf.py -DBOARD=waveshare_s3_lcd_7b build              # one-shot
#   idf.py -DBOARD=jc4827w543 set-target esp32s3 # persists in CMake cache
#
# The default is `jc4827w543`. See `README.md` for the full list.

cmake_minimum_required(VERSION 3.16)

# ---- Board selection ---------------------------------------------------------

set(BOARD "jc4827w543" CACHE STRING
    "Target board (subdirectory name under boards/)")

set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD}")
if(NOT IS_DIRECTORY "${BOARD_DIR}")
    file(GLOB _avail RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/boards"
         "${CMAKE_CURRENT_SOURCE_DIR}/boards/*")
    message(FATAL_ERROR
        "Unknown BOARD='${BOARD}'.\n"
        "Available boards under boards/: ${_avail}\n"
        "Pass -DBOARD=<name> on the idf.py command line.")
endif()
message(STATUS "touchy-pad: building for BOARD=${BOARD}")

# ---- Inject the board component & layer its sdkconfig overlay --------------

# Each board provides a component literally named `board`. Only one is
# included via EXTRA_COMPONENT_DIRS, so there is no symbol clash.
set(EXTRA_COMPONENT_DIRS "${BOARD_DIR}")

# Common defaults first, then board-specific overrides last so the board wins.
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults"
    "${BOARD_DIR}/sdkconfig.defaults"
)

# Per-board sdkconfig so switching boards doesn't pollute the previous one.
set(SDKCONFIG "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.${BOARD}")

# Tell main/ what we picked, so it can `#ifdef` if it ever needs to.
add_compile_definitions(TOUCHY_BOARD_${BOARD}=1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(touchy_pad_v2)
