/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include "sdkconfig.h"
#ifdef CONFIG_ARDUINO_RUNNING_CORE
#include <Arduino.h>
#endif
#include "esp_display_panel.hpp"
#include "lvgl.h"

// *INDENT-OFF*

/**
 * LVGL related parameters, can be adjusted by users
 */
#define LVGL_PORT_TICK_PERIOD_MS                (2)
// The period of the LVGL tick task, in milliseconds

/**
 *
 * LVGL buffer related parameters, can be adjusted by users:
 *
 *  (These parameters will be useless if the avoid tearing function is enabled)
 *
 *  - Memory type for buffer allocation:
 *      - MALLOC_CAP_SPIRAM: Allocate LVGL buffer in PSRAM
 *      - MALLOC_CAP_INTERNAL: Allocate LVGL buffer in SRAM
 *
 *      (The SRAM is faster than PSRAM, but the PSRAM has a larger capacity)
 *      (For SPI/QSPI LCD, it is recommended to allocate the buffer in SRAM,
 *       because the SPI DMA does not directly support PSRAM now)
 *
 *  - The size (in bytes) and number of buffers:
 *      - Lager buffer size can improve FPS, but it will occupy more memory.
 *        Maximum buffer size is `width * height`.
 *      - The number of buffers should be 1 or 2.
 */
#define LVGL_PORT_BUFFER_MALLOC_CAPS            (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
// #define LVGL_PORT_BUFFER_MALLOC_CAPS         (MALLOC_CAP_SPIRAM)
#define LVGL_PORT_BUFFER_SIZE_HEIGHT            (20)
#define LVGL_PORT_BUFFER_NUM                    (2)

/**
 * LVGL timer handle task related parameters, can be adjusted by users
 */
#define LVGL_PORT_TASK_MAX_DELAY_MS             (500)
#define LVGL_PORT_TASK_MIN_DELAY_MS             (2)
#define LVGL_PORT_TASK_STACK_SIZE               (6 * 1024)
#define LVGL_PORT_TASK_PRIORITY                 (2)
#ifdef ARDUINO_RUNNING_CORE
#define LVGL_PORT_TASK_CORE                     (ARDUINO_RUNNING_CORE)
#else
#define LVGL_PORT_TASK_CORE                     (0)
#endif

/**
 * Avoid tearing related configurations, can be adjusted by users.
 *
 *  (Currently, This function only supports RGB LCD and the version of LVGL must be >= 8.3.9)
 */
/**
 * Set the avoid tearing mode:
 *      - 0: Disable avoid tearing function
 *      - 1: LCD double-buffer & LVGL full-refresh
 *      - 2: LCD triple-buffer & LVGL full-refresh
 *      - 3: LCD double-buffer & LVGL direct-mode (recommended)
 */
#ifdef CONFIG_LVGL_PORT_AVOID_TEARING_MODE
#define LVGL_PORT_AVOID_TEARING_MODE            (CONFIG_LVGL_PORT_AVOID_TEARING_MODE)
#else
#define LVGL_PORT_AVOID_TEARING_MODE            (3)
#endif

#if LVGL_PORT_AVOID_TEARING_MODE != 0
/**
 * When avoid tearing is enabled, the LVGL software rotation `lv_disp_set_rotation()`
 * is not supported. But users can set the rotation degree(0/90/180/270) here,
 * but this function will reduce FPS.
 */
#ifdef CONFIG_LVGL_PORT_ROTATION_DEGREE
#define LVGL_PORT_ROTATION_DEGREE               (CONFIG_LVGL_PORT_ROTATION_DEGREE)
#else
#define LVGL_PORT_ROTATION_DEGREE               (0)
#endif

#define LVGL_PORT_AVOID_TEAR                    (1)
#if LVGL_PORT_AVOID_TEARING_MODE == 1
    #define LVGL_PORT_DISP_BUFFER_NUM           (2)
    #define LVGL_PORT_FULL_REFRESH              (1)
#elif LVGL_PORT_AVOID_TEARING_MODE == 2
    #define LVGL_PORT_DISP_BUFFER_NUM           (3)
    #define LVGL_PORT_FULL_REFRESH              (1)
#elif LVGL_PORT_AVOID_TEARING_MODE == 3
    #define LVGL_PORT_DISP_BUFFER_NUM           (2)
    #define LVGL_PORT_DIRECT_MODE               (1)
#else
    #error "Invalid avoid tearing mode, please set macro `LVGL_PORT_AVOID_TEARING_MODE` to one of `LVGL_PORT_AVOID_TEARING_MODE_*`"
#endif
#if (LVGL_PORT_ROTATION_DEGREE != 0) && (LVGL_PORT_ROTATION_DEGREE != 90) && \
    (LVGL_PORT_ROTATION_DEGREE != 180) && (LVGL_PORT_ROTATION_DEGREE != 270)
    #error "Invalid rotation degree, please set to 0, 90, 180 or 270"
#elif LVGL_PORT_ROTATION_DEGREE != 0
    #ifdef LVGL_PORT_DISP_BUFFER_NUM
        #undef LVGL_PORT_DISP_BUFFER_NUM
        #define LVGL_PORT_DISP_BUFFER_NUM       (3)
    #endif
#endif
#endif /* LVGL_PORT_AVOID_TEARING_MODE */

// *INDENT-ON*

#ifdef __cplusplus
extern "C" {
#endif

bool lvgl_port_init(esp_panel::drivers::LCD *lcd, esp_panel::drivers::Touch *tp);
bool lvgl_port_deinit(void);
bool lvgl_port_lock(int timeout_ms);
bool lvgl_port_unlock(void);

#ifdef __cplusplus
}
#endif
