// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared LVGL display driver for WS2812B LED-matrix boards (Stages LB1 /
// LB2). Used by jc_esp32p4_m3 and feather_esp32_s3; compiled into each
// board component via its CMakeLists.txt SRCS list.
//
// There is no LCD: the LVGL display is a small (e.g. 32×8) RGB565 canvas
// whose flush callback converts each rendered pixel to gamma-corrected
// RGB888 and pushes it into a single WS2812B LEDPanel. LVGL renders the
// whole frame each time (FULL render mode) — the surface is tiny, so a
// single full-frame buffer and one led_strip refresh per frame is simplest
// and avoids partial-area bookkeeping.
//
// Board-specific pins / geometry come from the board's own `board_pins.h`
// (BOARD_LED_PANEL_GPIO / _W / _H), pulled in via the board component's
// PRIV_INCLUDE_DIRS ".". The LEDPanel + serpentine map live alongside in
// led_panel.{h,cpp}.

#include "display.h"
#include "led_display.h"
#include "board_pins.h"
#include "led_panel.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include <cmath>

static const char *TAG = "display";

static LEDPanel     *s_panel      = nullptr;
static lv_display_t *s_disp       = nullptr;
// Desired brightness (0..255). Remembered even if set before the panel is
// up (backlight_init() runs before display_init()).
static uint8_t       s_brightness = 255;

// Gamma-correction LUT (gamma ≈ 2.2). WS2812B LEDs are perceptually very
// non-linear at low duty; without this, mid-tones look washed out and dim
// colours barely differ. Built once on first display_init().
static uint8_t s_gamma[256];

static void build_gamma_lut(void)
{
    for (int i = 0; i < 256; ++i) {
        s_gamma[i] = (uint8_t)lroundf(powf(i / 255.0f, 2.2f) * 255.0f);
    }
}

// LVGL flush: px_map is a full-frame RGB565 (little-endian) buffer covering
// `area` (== the whole display in FULL render mode). Convert + stage every
// pixel, present once, then acknowledge.
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (s_panel) {
        const auto *px = reinterpret_cast<const uint16_t *>(px_map);
        const int aw = area->x2 - area->x1 + 1;
        for (int y = area->y1; y <= area->y2; ++y) {
            for (int x = area->x1; x <= area->x2; ++x) {
                const uint16_t c = px[(y - area->y1) * aw + (x - area->x1)];
                // RGB565 → RGB888 (expand to full 8-bit range), then gamma.
                const uint8_t r5 = (c >> 11) & 0x1F;
                const uint8_t g6 = (c >> 5) & 0x3F;
                const uint8_t b5 = c & 0x1F;
                const uint8_t r8 = (uint8_t)((r5 * 255 + 15) / 31);
                const uint8_t g8 = (uint8_t)((g6 * 255 + 31) / 63);
                const uint8_t b8 = (uint8_t)((b5 * 255 + 15) / 31);
                s_panel->set_pixel(x, y, s_gamma[r8], s_gamma[g8], s_gamma[b8]);
            }
        }
        s_panel->flush();
    }
    lv_display_flush_ready(disp);
}

extern "C" lv_display_t *display_init(void)
{
    constexpr int W = BOARD_LED_PANEL_W;
    constexpr int H = BOARD_LED_PANEL_H;

    build_gamma_lut();

    s_panel = new LEDPanel(BOARD_LED_PANEL_GPIO, W, H, s_brightness);
    if (!s_panel->begin()) {
        ESP_LOGE(TAG, "LED panel bring-up failed; continuing headless");
        delete s_panel;
        s_panel = nullptr;
    }

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 4;
    port_cfg.task_stack      = 8 * 1024;
    port_cfg.timer_period_ms = 5;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const size_t buf_bytes = (size_t)W * H * sizeof(uint16_t);
    auto *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_DEFAULT);
    if (!buf) {
        ESP_LOGE(TAG, "failed to allocate %u-byte draw buffer",
                 (unsigned)buf_bytes);
        return nullptr;
    }

    lvgl_port_lock(0);
    s_disp = lv_display_create(W, H);
    if (s_disp) {
        lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(s_disp, buf, nullptr, buf_bytes,
                               LV_DISPLAY_RENDER_MODE_FULL);
        lv_display_set_flush_cb(s_disp, flush_cb);
    } else {
        ESP_LOGE(TAG, "lv_display_create failed");
    }
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LED-matrix LVGL display initialised at %dx%d", W, H);
    return s_disp;
}

void led_display_set_brightness(uint8_t level_0_100)
{
    if (level_0_100 > 100) level_0_100 = 100;
    s_brightness = (uint8_t)((uint16_t)level_0_100 * 255 / 100);

    if (s_panel) {
        s_panel->set_brightness(s_brightness);
        // Force a repaint so the new brightness is applied to the currently
        // displayed frame (brightness is baked in at pixel-staging time).
        lvgl_port_lock(0);
        lv_obj_t *scr = lv_screen_active();
        if (scr) {
            lv_obj_invalidate(scr);
        }
        lvgl_port_unlock();
    }
}
