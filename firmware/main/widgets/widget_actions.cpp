// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 20.2 / 24 — LVGL widget → Action wiring.
// See widget_actions.h for the design notes.

#include "widget_actions.h"
#include "tc_tag.h"

#include "fs/fs.h"
#include "host_api.h"
#include "lv_throttled.h"
#include "macros.h"
#include "screens.h"
#include "widget_builders.h"
#include "widgets.pb.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

static const char *TAG = TOUCHY_TAG("screens.actions");

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
        widget_run_action(slot->actions[i], obj, slot->widget_id, code,
                          slot->set_value);
    }
}

void widget_delete_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    auto *slot = static_cast<ActionSlot *>(lv_event_get_user_data(e));
    delete slot;
}

}  // namespace

void widget_run_action(const touchy_Action &act,
                       lv_obj_t *obj,
                       const char *widget_id,
                       lv_event_code_t code,
                       widget_value_fn set_value)
{
    switch (act.which_kind) {
    case touchy_Action_macro_tag:
        macros_run(&act.kind.macro);
        break;
    case touchy_Action_host_tag: {
        touchy_LvEvent evt = touchy_LvEvent_init_zero;
        evt.code = (uint32_t)code;
        snprintf(evt.user_data, sizeof(evt.user_data), "%s", widget_id);
        evt.host_code = act.kind.host.code;
        set_value(obj, &evt);
        host_api_post_event(&evt);
        break;
    }
    case touchy_Action_device_tag: {
        const touchy_ActionDevice &dev = act.kind.device;
        switch (dev.which_kind) {
        case touchy_ActionDevice_change_widget_ref_tag: {
            const touchy_ActionChangeWidgetRef &cw = dev.kind.change_widget_ref;
            // Resolve target path. For BY_PATH (0) we honour the
            // configured `path` verbatim. For NEXT (1) / PREVIOUS
            // (2) we treat `path` as a directory and enumerate its
            // `*.pb` entries; the current ref's path determines
            // where in the list we step.
            std::string target_path;
            const int   behavior   = (int)cw.behavior;
            const char *target_id  = cw.target_id;
            const char *dir_or_pth = cw.path;
            if (!target_id[0]) {
                ESP_LOGW(TAG, "change_widget_ref: missing target_id");
                break;
            }
            // Stage 86 — IMAGE_BUTTON_RELEASED (3) / IMAGE_BUTTON_PRESSED
            // (4): repoint an ImageButton's image slot in place by id,
            // rather than retargeting a WidgetRef by path. Deferred via
            // lv_throttled_post for the same reason as the ref rebuild
            // below (the apply touches the LVGL tree; throttling keeps a
            // burst of repaints from starving the host_api task).
            if (behavior == 3 || behavior == 4) {
                if (!dir_or_pth[0]) {
                    ESP_LOGW(TAG,
                             "change_widget_ref(IMAGE_BUTTON): empty path");
                    break;
                }
                std::string tid(target_id);
                std::string tpath(dir_or_pth);
                bool pressed = (behavior == 4);
                lv_throttled_post([tid, tpath, pressed]() {
                    bool ok = widget_image_button_set_slot(
                        tid.c_str(), pressed, tpath.c_str());
                    if (!ok) {
                        ESP_LOGE(TAG,
                                 "set_slot failed (id='%s' %s path='%s')",
                                 tid.c_str(), pressed ? "pressed" : "released",
                                 tpath.c_str());
                    }
                });
                break;
            }
            if (behavior == 0) {  // BY_PATH
                if (!dir_or_pth[0]) {
                    ESP_LOGW(TAG, "change_widget_ref(BY_PATH): empty path");
                    break;
                }
                target_path = dir_or_pth;
            } else if (behavior == 1 || behavior == 2) {  // NEXT / PREVIOUS
                const char *dir = dir_or_pth[0] ? dir_or_pth : "F:host/w/";
                std::string                 rest;
                Fs *fs = fs_resolve(dir, &rest);
                if (!fs) {
                    ESP_LOGW(TAG,
                             "change_widget_ref: bad drive in '%s'", dir);
                    break;
                }
                std::vector<std::string> files;
                // Drive prefix string for stitching paths back together.
                char         drive_letter = (dir[0] & ~0x20);
                std::string  prefix(1, drive_letter);
                prefix += ":";
                if (!rest.empty() && rest.back() != '/') rest += '/';
                fs->list(rest, [&](const std::string &name, bool is_dir) {
                    if (is_dir) return true;
                    if (name.size() < 4) return true;
                    if (name.compare(name.size() - 3, 3, ".pb") != 0) return true;
                    files.push_back(prefix + rest + name);
                    return true;
                });
                if (files.empty()) {
                    ESP_LOGW(TAG,
                             "change_widget_ref: no *.pb files in '%s'", dir);
                    break;
                }
                std::sort(files.begin(), files.end());
                const char *current = widget_refs_current_path(target_id);
                int idx = 0;
                if (current) {
                    auto it = std::find(files.begin(), files.end(),
                                        std::string(current));
                    if (it == files.end()) {
                        ESP_LOGW(TAG,
                                 "change_widget_ref: current path '%s' "
                                 "not in '%s'; defaulting to first entry",
                                 current, dir);
                    } else {
                        idx = (int)(it - files.begin());
                    }
                }
                int step = (behavior == 1) ? 1 : -1;
                int n    = (int)files.size();
                int next = ((idx + step) % n + n) % n;
                target_path = files[next];
            } else {
                ESP_LOGW(TAG,
                         "change_widget_ref: unknown behavior %d",
                         behavior);
                break;
            }

            // Defer via lv_throttled_post — `widget_refs_change` deletes
            // LVGL objects in the same event chain that is dispatching
            // this callback, which is unsafe inline. Throttled (rather
            // than a raw lv_async_call) so a burst of change_widget_ref
            // actions can't drain back-to-back under one continuous LVGL
            // port-lock hold and starve the host_api task — see
            // lv_throttled.h. Both call sites of widget_run_action run
            // under the LVGL port lock (event callback during
            // lv_timer_handler; widget_run_actions takes it explicitly),
            // so posting here is safe.
            std::string tid(target_id);
            std::string tpath(target_path);
            lv_throttled_post([tid, tpath]() {
                bool ok = widget_refs_change(tid.c_str(), tpath.c_str());
                if (!ok) {
                    ESP_LOGE(TAG,
                             "change_widget_ref failed "
                             "(target_id='%s' path='%s')",
                             tid.c_str(), tpath.c_str());
                } else {
                    ESP_LOGI(TAG,
                             "change_widget_ref succeeded "
                             "(target_id='%s' path='%s')",
                             tid.c_str(), tpath.c_str());
                }
            });
            break;
        }
        default:
            ESP_LOGW(TAG, "widget '%s' has unknown device action %u",
                     widget_id, (unsigned)dev.which_kind);
            break;
        }
        break;
    }
    default:
        ESP_LOGW(TAG, "widget '%s' has unknown action kind %u",
                 widget_id, (unsigned)act.which_kind);
        break;
    }
}

void widget_run_actions(const touchy_Action *actions, pb_size_t actions_count)
{
    if (!actions || actions_count == 0) return;
    // Run on the host_api task but touch LVGL state (lv_async_call,
    // widget_refs lookups) under the LVGL lock, mirroring screens_load().
    lvgl_port_lock(0);
    for (pb_size_t i = 0; i < actions_count; i++) {
        widget_run_action(actions[i], nullptr, "", LV_EVENT_CLICKED,
                          widget_value_none);
    }
    lvgl_port_unlock();
}

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
