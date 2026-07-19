// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared LVGL display driver for WS2812B LED-matrix boards (Stages LB1 /
// LB2). Used by jc_esp32p4_m3 and esp32-s3-devkitc-1; compiled into each
// board component via its CMakeLists.txt SRCS list.
//
// There is no LCD: the LVGL display is a small (e.g. 32×8) RGB565 canvas
// whose flush callback converts each rendered pixel to gamma-corrected
// RGB888 and pushes it into a WS2812B LEDChain. LVGL renders the whole
// frame each time (FULL render mode) — the surface is tiny, so a single
// full-frame buffer and one led_strip refresh per frame is simplest and
// avoids partial-area bookkeeping.
//
// Board-specific pins come from the board's own `board_pins.h`, but the LED
// panel-chain geometry (per-panel size + wiring, data GPIO, tiling axis) is
// a runtime setting read from the persisted BoardConfig (Stage lb6/lb10)
// via Prefs — a fresh, unconfigured board brings up no LED display. The
// LEDChain + LEDPanel tiles + serpentine map live alongside in
// led_panel.{h,cpp}.

#include "display.h"
#include "led_display.h"
#include "led_panel.h"
#include "tc_tag.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include <cmath>

static const char *TAG = TOUCHY_TAG("display");

static LEDChain     *s_chain      = nullptr;
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
    if (s_chain) {
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
                s_chain->set_pixel(x, y, s_gamma[r8], s_gamma[g8], s_gamma[b8]);
            }
        }
        s_chain->flush();
    }
    lv_display_flush_ready(disp);
}

// Stage lb7 — the LED-matrix board's Display subclass. Both LED boards
// (esp32_s3_devkitc_1, jc_esp32p4_m3) share this driver, so display_create()
// is defined here once.
namespace {
class LEDMatrixDisplay : public Display {
protected:
    lv_display_t *hw_init() override;
};
}  // namespace

Display *display_create(void)
{
    return new LEDMatrixDisplay();
}

lv_display_t *LEDMatrixDisplay::hw_init(void)
{
    // Stage lb6/lb10 — panel geometry comes from the persisted BoardConfig,
    // not compile-time macros. A device that has never been programmed has
    // no chain and comes up headless (host sees display 0×0) until the user
    // pushes a config (e.g. `touchy pref from-template led-32x8`).
    LedChainDesc chain;
    if (!led_chain_config(&chain)) {
        ESP_LOGW(TAG, "No LED panel configured (board_config empty) — "
                       "running headless; push a config with "
                       "'touchy pref from-template <name>'");
        return nullptr;
    }

    // Tile the panels into one logical surface (Stage lb10). Horizontal
    // tiling (tile_by_row == false) lays panels left-to-right in chain
    // order; vertical tiling stacks them top-to-bottom.
    int total_w = 0, total_h = 0;
    for (int i = 0; i < chain.panel_count; ++i) {
        const LedPanelDesc &p = chain.panels[i];
        if (p.width <= 0 || p.height <= 0) {
            ESP_LOGE(TAG, "Invalid LED panel geometry %dx%d — running headless",
                     p.width, p.height);
            return nullptr;
        }
        if (chain.tile_by_row) {
            total_h += p.height;
            if (p.width > total_w) total_w = p.width;
        } else {
            total_w += p.width;
            if (p.height > total_h) total_h = p.height;
        }
    }

    // Guard against a bogus template so a bad config can't OOM the draw
    // buffer or trip lv_display_create.
    if (total_w <= 0 || total_h <= 0 || (long)total_w * total_h > 4096) {
        ESP_LOGE(TAG, "Invalid LED surface geometry %dx%d — running headless",
                 total_w, total_h);
        return nullptr;
    }

    build_gamma_lut();

    s_chain = new LEDChain(chain.gpio, total_w, total_h, s_brightness);
    int off = 0;  // running offset along the tiling axis
    for (int i = 0; i < chain.panel_count; ++i) {
        const LedPanelDesc &p = chain.panels[i];
        LedWiring wiring;
        wiring.rows_snaked  = p.rows_snaked;
        wiring.cols_snaked  = p.cols_snaked;
        wiring.row_major    = p.row_major;
        wiring.cols_flipped = p.cols_flipped;
        wiring.rows_flipped = p.rows_flipped;
        const int x_off = chain.tile_by_row ? 0 : off;
        const int y_off = chain.tile_by_row ? off : 0;
        s_chain->add_tile(p.width, p.height, x_off, y_off, wiring);
        off += chain.tile_by_row ? p.height : p.width;
    }

    if (!s_chain->begin()) {
        ESP_LOGE(TAG, "LED panel bring-up failed; continuing headless");
        delete s_chain;
        s_chain = nullptr;
    }

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 4;
    port_cfg.task_stack      = 8 * 1024;
    port_cfg.timer_period_ms = 5;
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const size_t buf_bytes = (size_t)total_w * total_h * sizeof(uint16_t);
    auto *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_DEFAULT);
    if (!buf) {
        ESP_LOGE(TAG, "failed to allocate %u-byte draw buffer",
                 (unsigned)buf_bytes);
        return nullptr;
    }

    lvgl_port_lock(0);
    s_disp = lv_display_create(total_w, total_h);
    if (s_disp) {
        lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(s_disp, buf, nullptr, buf_bytes,
                               LV_DISPLAY_RENDER_MODE_FULL);
        lv_display_set_flush_cb(s_disp, flush_cb);
    } else {
        ESP_LOGE(TAG, "lv_display_create failed");
    }
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LED-matrix LVGL display initialised at %dx%d (%d tiles)",
             total_w, total_h, chain.panel_count);
    return s_disp;
}

void led_display_set_brightness(uint8_t level_0_100)
{
    if (level_0_100 > 100) level_0_100 = 100;
    s_brightness = (uint8_t)((uint16_t)level_0_100 * 255 / 100);

    if (s_chain) {
        s_chain->set_brightness(s_brightness);
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
