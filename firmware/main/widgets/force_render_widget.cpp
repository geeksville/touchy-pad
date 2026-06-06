// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 24 — ForceRender checkbox. See force_render_widget.h.

#include "force_render_widget.h"
#include "tc_tag.h"

#include "esp_log.h"

static const char *TAG = TOUCHY_TAG("force_render");

namespace {

// Shared across every ForceRenderWidget on the current screen — a
// refcount of "how many checkboxes are currently checked". When it
// transitions 0→1 we spin up the redraw timer; when it falls back to
// 0 we delete it. Touched only from the LVGL task, so plain ints are
// safe.
int g_active = 0;
lv_timer_t *g_timer = nullptr;

// Fastest LVGL will let us schedule: a 1 ms period. The actual redraw
// cadence is bounded by `lv_timer_handler()` returning often enough
// (it is — the esp_lvgl_port task loops on it) and by the display
// flush time itself.
constexpr uint32_t kForceTickMs = 1;

void force_tick(lv_timer_t *)
{
    // Invalidate the current screen so LVGL has dirty regions to flush
    // on the next handler pass. This is the cheapest way to keep the
    // refresh pipeline busy from outside LVGL's normal change-tracking.
    lv_obj_t *scr = lv_screen_active();
    if (scr) lv_obj_invalidate(scr);
}

void retain_force()
{
    if (g_active++ == 0) {
        g_timer = lv_timer_create(force_tick, kForceTickMs, nullptr);
        ESP_LOGI(TAG, "force-render ON");
    }
}

void release_force()
{
    if (g_active <= 0) return;          // defensive: never go negative
    if (--g_active == 0 && g_timer) {
        lv_timer_delete(g_timer);
        g_timer = nullptr;
        ESP_LOGI(TAG, "force-render OFF");
    }
}

}  // namespace

ForceRenderWidget::ForceRenderWidget(lv_obj_t *parent)
{
    _checkbox = lv_checkbox_create(parent);
    lv_checkbox_set_text_static(_checkbox, "Force");
    // Starts unchecked. We *don't* touch the global state here even if
    // someone later adds a way to preset the checked flag — the user
    // intent is "force is off until I toggle this".
    lv_obj_remove_state(_checkbox, LV_STATE_CHECKED);
    lv_obj_add_event_cb(_checkbox, _valueChangedCb,
                        LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(_checkbox, _deleteCb, LV_EVENT_DELETE, this);
}

void ForceRenderWidget::_valueChangedCb(lv_event_t *e)
{
    auto *self = static_cast<ForceRenderWidget *>(lv_event_get_user_data(e));
    if (!self) return;
    bool checked = lv_obj_has_state(self->_checkbox, LV_STATE_CHECKED);
    if (checked) retain_force();
    else         release_force();
}

void ForceRenderWidget::_deleteCb(lv_event_t *e)
{
    // If the widget is destroyed (e.g. screen switch) while still
    // checked, drop the refcount it was holding so we don't leak a
    // permanent force-render timer across screens.
    auto *self = static_cast<ForceRenderWidget *>(lv_event_get_user_data(e));
    if (!self) return;
    if (lv_obj_has_state(self->_checkbox, LV_STATE_CHECKED)) {
        release_force();
    }
}
