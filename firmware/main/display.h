// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Brings up the parallel RGB LCD panel, the LVGL port task, and registers
// the panel as an LVGL display driver. Returns the LVGL display handle.
//
// Caller must hold no LVGL lock when calling this.
lv_disp_t *display_init(void);

#ifdef __cplusplus
}
#endif
