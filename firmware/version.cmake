# SPDX-License-Identifier: GPL-3.0-or-later
#
# Firmware versioning — included by firmware/CMakeLists.txt before project().
#
# Reads the repo-root VERSION file (line 1 = semver base, line 2 = build
# number) and git state, then sets:
#
#   FW_VER_STR    "x.y.z" on a clean tree, "x.y.z.devN+hash" when dirty
#   FW_BUILD_NUM  integer build number from VERSION line 2
#   PROJECT_VER   set to FW_VER_STR so esp_app_get_description()->version
#                 carries the right string in the on-device app descriptor
#
# Also calls configure_file() to emit firmware/build/version.h, which
# firmware/main/host_api.cpp includes for FIRMWARE_VERSION_STR and
# FIRMWARE_BUILD_NUMBER.  The configure_file(COPYONLY) stamp ensures cmake
# re-runs configuration automatically whenever VERSION changes.

set(_ver_file "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")

# ---- Track VERSION as a CMake dependency ---------------------------------
# configure_file COPYONLY registers _ver_file as an input; cmake will
# re-run configuration (and regenerate version.h) whenever it changes.
configure_file("${_ver_file}" "${CMAKE_BINARY_DIR}/_version.stamp" COPYONLY)

# ---- Parse VERSION -------------------------------------------------------
file(READ "${_ver_file}" _ver_raw)
string(STRIP "${_ver_raw}" _ver_raw)
# Tolerates both LF and CRLF line endings.
string(REGEX MATCH
    "^([0-9]+\\.[0-9]+\\.[0-9]+)[^\n\r]*[\r\n]+([0-9]+)"
    _ "${_ver_raw}")
set(FW_VER_BASE  "${CMAKE_MATCH_1}")
set(FW_BUILD_NUM "${CMAKE_MATCH_2}")

if(NOT FW_VER_BASE OR NOT FW_BUILD_NUM)
    message(FATAL_ERROR
        "Could not parse VERSION file '${_ver_file}'.\n"
        "Expected two lines: semver (e.g. 0.1.0) then build number (e.g. 1).")
endif()

# ---- Git state -----------------------------------------------------------
# git -C <dir> searches upward for .git so it finds the repo root even
# though we're pointing it at firmware/.
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
                status --porcelain
        OUTPUT_VARIABLE _git_dirty
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
                rev-parse --short HEAD
        OUTPUT_VARIABLE _git_hash
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
                rev-list --count HEAD
        OUTPUT_VARIABLE _git_count
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
else()
    set(_git_dirty "")
    set(_git_hash  "unknown")
    set(_git_count "0")
endif()

# ---- Build full version string -------------------------------------------
# Clean tree  →  "x.y.z"
# Dirty tree  →  "x.y.z.devN+hash"  (PEP 440 local-version convention)
if(_git_dirty)
    set(FW_VER_STR "${FW_VER_BASE}.dev${_git_count}+${_git_hash}")
else()
    set(FW_VER_STR "${FW_VER_BASE}")
endif()

set(PROJECT_VER "${FW_VER_STR}")
message(STATUS "touchy-pad: version ${FW_VER_STR}  (build ${FW_BUILD_NUM})")

# ---- Emit version.h into the build directory -----------------------------
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/main/version.h.in"
    "${CMAKE_BINARY_DIR}/version.h"
    @ONLY
)
