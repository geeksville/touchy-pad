// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage lb7 — Display base-class implementation + the board-agnostic
// HeadlessDisplay. Board-specific subclasses (LEDMatrixDisplay, each LCD
// board's own Display) live in their board components.

#include "display.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"

static const char *TAG = "display";

bool Display::init()
{
    m_disp = hw_init();
    if (!m_disp) {
        return false;
    }
    post_init();
    return true;
}

void Display::post_init()
{
// hmm doesnt work yet
#if 0
    // Dim blue background so a blank screen (or the area behind widgets)
    // clearly reads as "display is on". LVGL v9 has no lv_disp_set_bg_color;
    // style the display's active screen instead.
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_display_get_screen_active(m_disp);
    if (scr) {
        lv_obj_set_style_bg_color(scr, lv_color_hex(0x000020), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    }
    lvgl_port_unlock();
#endif
}

// ---------------------------------------------------------------------------
// HeadlessDisplay
// ---------------------------------------------------------------------------

// Resolution of the off-screen framebuffer. Kept small; nothing is shown.
#ifndef CONFIG_TOUCHY_HEADLESS_HRES
#define CONFIG_TOUCHY_HEADLESS_HRES 320
#endif
#ifndef CONFIG_TOUCHY_HEADLESS_VRES
#define CONFIG_TOUCHY_HEADLESS_VRES 240
#endif

// Bring LVGL up against an off-screen framebuffer with a no-op flush
// callback (Stage 64.4). Keeps the whole screen / host_api stack running
// while no real panel is present. Returns nullptr if the draw buffer could
// not be allocated.
lv_display_t *HeadlessDisplay::hw_init()
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 4;
    port_cfg.task_stack      = 8 * 1024;
    port_cfg.timer_period_ms = 5;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    constexpr int    W             = CONFIG_TOUCHY_HEADLESS_HRES;
    constexpr int    H             = CONFIG_TOUCHY_HEADLESS_VRES;
    constexpr size_t LINES_PER_BUF = 40;
    const size_t     buf_bytes     = (size_t)W * LINES_PER_BUF * sizeof(uint16_t);

    auto *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_DEFAULT);
    if (!buf) {
        ESP_LOGE(TAG, "headless: failed to allocate %u-byte draw buffer",
                 (unsigned)buf_bytes);
        return nullptr;
    }

    lvgl_port_lock(0);
    lv_display_t *disp = lv_display_create(W, H);
    if (disp) {
        lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(disp, buf, nullptr, buf_bytes,
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        // Discard every rendered slice; just acknowledge it immediately.
        lv_display_set_flush_cb(disp,
            [](lv_display_t *d, const lv_area_t *, uint8_t *) {
                lv_display_flush_ready(d);
            });
    } else {
        ESP_LOGE(TAG, "headless: lv_display_create failed");
    }
    lvgl_port_unlock();
    return disp;
}
