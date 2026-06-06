// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 24 — live FPS readout widget. See fps_widget.h.

#include "fps_widget.h"
#include "tc_tag.h"

#include "esp_log.h"
#include "lvgl.h"

#include <cstdio>

static const char *TAG = TOUCHY_TAG("fps");

namespace {

// Refresh period for the visible label. Half a second feels responsive
// without making the readout unreadable.
constexpr uint32_t kTickPeriodMs = 500;

// One sampling counter per FpsWidget. LVGL's refresh-ready event fires
// on every display flush; multiplying the count by (1000 / period_ms)
// gives FPS at the chosen update cadence.
void refr_event_cb(lv_event_t *e)
{
    auto *counter = static_cast<uint32_t *>(lv_event_get_user_data(e));
    if (counter) ++*counter;
}

}  // namespace

FpsWidget::FpsWidget(lv_obj_t *parent)
{
    // A bare label is enough — the host can attach styles via the
    // Widget's styles[] just like any other widget. We don't use an
    // outer container because the label is naturally focusable-free
    // and inherits the parent's grid/flex cell.
    _label = lv_label_create(parent);
    _container = _label;
    lv_label_set_text(_label, "FPS: --");
    lv_obj_set_style_text_opa(_label, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(_label, LV_OPA_TRANSP, 0);

    // The counter must outlive _both_ the display event (refr_event_cb
    // dereferences it on every flush) and the timer (its callback
    // reads + resets it). Both hooks + the counter are owned by a
    // DeleteCtx held in the widget's LV_EVENT_DELETE handler.
    auto *counter = new uint32_t(0);
    lv_display_t *disp = lv_display_get_default();
    if (disp) {
        lv_display_add_event_cb(disp, refr_event_cb,
                                LV_EVENT_REFR_READY, counter);
    }
    _counter = counter;
    _timer = lv_timer_create(_timerCb, kTickPeriodMs, this);
    auto *ctx = new DeleteCtx{counter, _timer, disp};
    lv_obj_add_event_cb(_container, _deleteCb, LV_EVENT_DELETE, ctx);
    ESP_LOGI(TAG, "fps widget created");
}

void FpsWidget::_tick()
{
    if (!_counter) return;
    uint32_t frames = *_counter;
    *_counter = 0;
    // frames per kTickPeriodMs → frames per second.
    uint32_t fps = (frames * 1000u) / kTickPeriodMs;
    char buf[24];
    snprintf(buf, sizeof(buf), "FPS: %lu", (unsigned long)fps);
    lv_label_set_text(_label, buf);
}

void FpsWidget::_timerCb(lv_timer_t *t)
{
    auto *self = static_cast<FpsWidget *>(lv_timer_get_user_data(t));
    if (self) self->_tick();
}

void FpsWidget::_deleteCb(lv_event_t *e)
{
    auto *ctx = static_cast<DeleteCtx *>(lv_event_get_user_data(e));
    if (!ctx) return;
    if (ctx->timer) lv_timer_delete(ctx->timer);
    if (ctx->disp) {
        // Walk the display event list and remove our entry. LVGL 9
        // expects us to pass the same callback + user_data we
        // registered with.
        lv_display_remove_event_cb_with_user_data(ctx->disp, refr_event_cb,
                                                  ctx->counter);
    }
    delete ctx->counter;
    delete ctx;
}
