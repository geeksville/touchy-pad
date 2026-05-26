// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 59 — declarative LVGL animations driven by `Widget.animations[]`.
// See widget_animations.h for the rationale.

#include "widget_animations.h"

#include "widget_styles.h"   // lv_prop_from_proto / lv_path_from_proto
#include "widgets.pb.h"

#include "esp_log.h"
#include "lvgl.h"

#include <new>
#include <vector>

static const char *TAG = "widgets.anim";

namespace {

// Per-track context the LVGL animator hands back to our exec callback.
// We can't squeeze the style property into `lv_anim_t::var` (it already
// stores the lv_obj_t*), so we allocate one of these per track and pass
// it as `var`. The exec callback dereferences `ctx->obj` to find the
// target widget and `ctx->prop` to know which style property to set.
struct AnimCtx {
    lv_obj_t        *obj;
    lv_style_prop_t  prop;
};

// Per-widget bag of `AnimCtx*` so the LV_EVENT_DELETE callback knows
// which contexts to free when the widget goes away.
struct WidgetAnimations {
    std::vector<AnimCtx *> ctxs;
};

// LVGL animation exec callback. Sets one local style property on the
// target widget every frame. Works for any integer-valued style prop
// (X/Y/WIDTH/HEIGHT, OPA, etc.) because `lv_style_value_t::num` covers
// all of them.
void anim_style_exec_cb(void *var, int32_t v)
{
    auto *ctx = static_cast<AnimCtx *>(var);
    if (!ctx || !ctx->obj) return;
    lv_style_value_t sv = { .num = v };
    lv_obj_set_local_style_prop(ctx->obj, ctx->prop, sv, LV_PART_MAIN);
}

void widget_animations_delete_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    auto *wa = static_cast<WidgetAnimations *>(lv_event_get_user_data(e));
    if (!wa) return;
    // Cancel any still-running animations bound to these contexts before
    // freeing them, otherwise the next animator tick will dereference
    // freed memory.
    for (AnimCtx *ctx : wa->ctxs) {
        lv_anim_delete(ctx, anim_style_exec_cb);
        delete ctx;
    }
    delete wa;
}

}  // namespace

void apply_animations(lv_obj_t *obj, const touchy_Widget &w)
{
    if (w.animations_count == 0 || w.animations == nullptr) return;

    auto *wa = new (std::nothrow) WidgetAnimations{};
    if (!wa) return;

    for (pb_size_t i = 0; i < w.animations_count; ++i) {
        const touchy_Animation &anim = w.animations[i];
        if (anim.tracks_count == 0 || anim.tracks == nullptr) continue;

        // `repeat_count == 0` on the wire → infinite (matches LVGL's
        // LV_ANIM_REPEAT_INFINITE sentinel).
        uint32_t repeat = (anim.repeat_count == 0)
                              ? LV_ANIM_REPEAT_INFINITE
                              : anim.repeat_count;
        // 0 → forward duration (LVGL defaults reverse to 0 = disabled,
        // which is not what we want when `reverse` is set).
        uint32_t rev_dur = anim.reverse_duration_ms
                               ? anim.reverse_duration_ms
                               : anim.duration_ms;

        for (pb_size_t j = 0; j < anim.tracks_count; ++j) {
            const touchy_AnimTrack &track = anim.tracks[j];
            lv_style_prop_t prop = lv_prop_from_proto(track.prop);
            if (prop == LV_STYLE_PROP_INV) {
                ESP_LOGW(TAG, "skipping track with unsupported prop %d",
                         (int)track.prop);
                continue;
            }

            auto *ctx = new (std::nothrow) AnimCtx{ obj, prop };
            if (!ctx) continue;
            wa->ctxs.push_back(ctx);

            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, ctx);
            lv_anim_set_exec_cb(&a, anim_style_exec_cb);
            lv_anim_set_values(&a, track.start, track.end);
            lv_anim_set_duration(&a, anim.duration_ms);
            lv_anim_set_path_cb(&a, lv_path_from_proto(anim.path));
            lv_anim_set_repeat_count(&a, repeat);
            lv_anim_set_repeat_delay(&a, anim.repeat_delay_ms);
            if (anim.start_delay_ms > 0)
                lv_anim_set_delay(&a, anim.start_delay_ms);
            if (anim.reverse) {
                lv_anim_set_reverse_duration(&a, rev_dur);
                lv_anim_set_reverse_delay(&a, anim.reverse_delay_ms);
            }
            lv_anim_start(&a);
        }
    }

    if (wa->ctxs.empty()) {
        delete wa;
        return;
    }
    lv_obj_add_event_cb(obj, widget_animations_delete_cb, LV_EVENT_DELETE, wa);
    ESP_LOGI(TAG, "apply_animations id='%s' anims=%u ctxs=%zu",
             w.id, (unsigned)w.animations_count, wa->ctxs.size());
}
