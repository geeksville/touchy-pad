// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 20.2 / 24 — LVGL widget → Action wiring.
// See widget_actions.h for the design notes.

#include "widget_actions.h"

#include "host_api.h"
#include "macros.h"
#include "screens.h"
#include "widgets.pb.h"

#include "esp_log.h"
#include "lvgl.h"

#include <cstdio>
#include <cstring>
#include <new>

static const char *TAG = "screens.actions";

namespace {

// Per-widget heap state. `actions` points into the active decoded Screen
// (owned by g_active_screen in screens.cpp), so it is valid for the
// widget's lifetime — the screen object is only replaced after the LVGL
// screen carrying these widgets has been swapped out and deleted, which
// fires LV_EVENT_DELETE → widget_delete_cb first.
struct ActionSlot {
    char widget_id[32];
    const touchy_Action *actions;
    pb_size_t            actions_count;
    widget_value_fn      set_value;
};

void widget_event_cb(lv_event_t *e)
{
    auto *slot = static_cast<ActionSlot *>(lv_event_get_user_data(e));
    if (!slot || slot->actions_count == 0) return;
    lv_event_code_t code = lv_event_get_code(e);
    auto *obj = static_cast<lv_obj_t *>(lv_event_get_target(e));

    for (pb_size_t i = 0; i < slot->actions_count; i++) {
        const touchy_Action &act = slot->actions[i];
        switch (act.which_kind) {
        case touchy_Action_macro_tag:
            macros_run(&act.kind.macro);
            break;
        case touchy_Action_host_tag: {
            touchy_LvEvent evt = touchy_LvEvent_init_zero;
            evt.code = (uint32_t)code;
            snprintf(evt.user_data, sizeof(evt.user_data), "%s", slot->widget_id);
            evt.host_code = act.kind.host.code;
            slot->set_value(obj, &evt);
            host_api_post_event(&evt);
            break;
        }
        case touchy_Action_device_tag: {
            const touchy_ActionDevice &dev = act.kind.device;
            switch (dev.which_kind) {
            case touchy_ActionDevice_switch_screen_tag: {
                const touchy_ActionSwitchScreen &ss = dev.kind.switch_screen;
                // ActionSwitchScreen.Behavior values match the int code
                // expected by screens_switch() 1:1.
                //
                // CRITICAL: defer to `lv_async_call`. We're being called
                // from the middle of LVGL's event dispatch on a widget
                // whose owning screen would be torn down by
                // `screens_switch()` (it calls `lv_obj_delete(old_scr)`
                // on the current screen). Deleting LVGL objects while
                // their event chain is still being walked is a
                // use-after-free — LVGL's standard remedy is to defer
                // such "destroy myself" actions via `lv_async_call`,
                // which runs the callback at the start of the next
                // `lv_timer_handler` tick, after every in-flight event
                // has fully unwound.
                struct SwitchReq {
                    int  behavior;
                    char path[96];   // matches widgets.options ActionSwitchScreen.path
                };
                auto *req = new (std::nothrow) SwitchReq{};
                if (!req) {
                    ESP_LOGE(TAG, "OOM scheduling switch_screen");
                    break;
                }
                req->behavior = (int)ss.behavior;
                snprintf(req->path, sizeof(req->path), "%s", ss.path);
                lv_async_call(
                    [](void *p) {
                        auto *r = static_cast<SwitchReq *>(p);
                        bool ok = screens_switch(
                            r->behavior, r->path[0] ? r->path : nullptr);
                        if (!ok) {
                            ESP_LOGW(TAG,
                                     "switch_screen failed "
                                     "(behavior=%d path='%s')",
                                     r->behavior, r->path);
                        }
                        delete r;
                    },
                    req);
                break;
            }
            default:
                ESP_LOGD(TAG, "widget '%s' has unknown device action %u",
                         slot->widget_id, (unsigned)dev.which_kind);
                break;
            }
            break;
        }
        default:
            ESP_LOGD(TAG, "widget '%s' has unknown action kind %u",
                     slot->widget_id, (unsigned)act.which_kind);
            break;
        }
    }
}

void widget_delete_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    auto *slot = static_cast<ActionSlot *>(lv_event_get_user_data(e));
    delete slot;
}

}  // namespace

void widget_value_none(lv_obj_t *, touchy_LvEvent *) {}

void widget_value_slider(lv_obj_t *obj, touchy_LvEvent *evt)
{
    evt->which_state = touchy_LvEvent_value_tag;
    evt->state.value = lv_slider_get_value(obj);
}

void widget_value_switch(lv_obj_t *obj, touchy_LvEvent *evt)
{
    evt->which_state = touchy_LvEvent_checked_tag;
    evt->state.checked = lv_obj_has_state(obj, LV_STATE_CHECKED);
}

void widget_attach_actions(lv_obj_t *obj,
                           const char *widget_id,
                           const touchy_Action *actions,
                           pb_size_t actions_count,
                           lv_event_code_t code,
                           widget_value_fn set_value)
{
    if (actions_count == 0) return;
    auto *slot = new (std::nothrow) ActionSlot{};
    if (!slot) return;
    // Always NUL-terminate; widget_id is at most 31 chars + NUL by virtue
    // of the proto cap, but we copy via snprintf to keep gcc's
    // stringop-truncation checker quiet.
    snprintf(slot->widget_id, sizeof(slot->widget_id), "%s", widget_id);
    slot->actions       = actions;
    slot->actions_count = actions_count;
    slot->set_value     = set_value;
    lv_obj_add_event_cb(obj, widget_event_cb, code, slot);
    lv_obj_add_event_cb(obj, widget_delete_cb, LV_EVENT_DELETE, slot);
}
