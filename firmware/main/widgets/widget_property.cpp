// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage lb12 — runtime widget property overrides. See widget_property.h.

#include "widget_property.h"
#include "tc_tag.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"

#include <string>
#include <vector>

static const char *TAG = TOUCHY_TAG("widgets.property");

namespace {

// One remembered override, keyed by (widget_id, ident). `cmd` keeps the
// full value variant so it can be re-applied on every rebuild.
struct Override {
    std::string           widget_id;
    std::string           ident;  // property name, or "#<id>" for numeric
    touchy_SetPropertyCmd cmd;
};

struct IdObj {
    std::string id;
    lv_obj_t   *obj;
};

// All accessed only under the LVGL port lock (see header).
std::vector<Override> s_overrides;   // sticky, session-scoped
std::vector<IdObj>    s_pending;      // id→obj for the screen being built
std::vector<IdObj>    s_active;       // id→obj for the live screen

std::string ident_of(const touchy_SetPropertyCmd &cmd)
{
    if (cmd.which_property == touchy_SetPropertyCmd_property_name_tag) {
        return std::string(cmd.property.property_name);
    }
    if (cmd.which_property == touchy_SetPropertyCmd_property_id_tag) {
        return "#" + std::to_string(cmd.property.property_id);
    }
    return std::string();
}

lv_obj_t *find_active(const char *id)
{
    for (auto &e : s_active) {
        if (e.id == id) return e.obj;
    }
    return nullptr;
}

#if LV_USE_OBJ_PROPERTY
// Apply one override's value to a live object. Returns false if the
// property can't be resolved / set. An unset value is a no-op here (the
// removal only affects future rebuilds).
bool apply_to_obj(lv_obj_t *obj, const touchy_SetPropertyCmd &cmd)
{
    if (cmd.which_value == 0) return true;

    lv_prop_id_t pid = LV_PROPERTY_ID_INVALID;
    if (cmd.which_property == touchy_SetPropertyCmd_property_name_tag) {
        pid = lv_obj_property_get_id(obj, cmd.property.property_name);
    } else if (cmd.which_property == touchy_SetPropertyCmd_property_id_tag) {
        pid = (lv_prop_id_t)cmd.property.property_id;
    }
    if (pid == LV_PROPERTY_ID_INVALID) {
        ESP_LOGW(TAG, "unknown property '%s' on widget '%s'",
                 cmd.which_property == touchy_SetPropertyCmd_property_name_tag
                     ? cmd.property.property_name : "(numeric)",
                 cmd.widget_id);
        return false;
    }

    lv_property_t prop;
    lv_memzero(&prop, sizeof(prop));
    prop.id = pid;
    switch (cmd.which_value) {
    case touchy_SetPropertyCmd_bool_value_tag:
        prop.num = cmd.value.bool_value ? 1 : 0;
        break;
    case touchy_SetPropertyCmd_int_value_tag:
        prop.num = cmd.value.int_value;
        break;
    case touchy_SetPropertyCmd_string_value_tag:
        prop.ptr = cmd.value.string_value;
        break;
    case touchy_SetPropertyCmd_color_value_tag:
        prop.color = lv_color_hex(cmd.value.color_value);
        break;
    case touchy_SetPropertyCmd_point_value_tag:
        prop.point.x = cmd.value.point_value.x;
        prop.point.y = cmd.value.point_value.y;
        break;
    default:
        return false;
    }

    if (lv_obj_set_property(obj, &prop) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "lv_obj_set_property failed (widget '%s')", cmd.widget_id);
        return false;
    }
    ESP_LOGD(TAG, "applied property '%s' on widget '%s'", ident_of(cmd).c_str(),
             cmd.widget_id);
    return true;
}
#else   // LV_USE_OBJ_PROPERTY
bool apply_to_obj(lv_obj_t *, const touchy_SetPropertyCmd &)
{
    ESP_LOGW(TAG, "LV_USE_OBJ_PROPERTY disabled — set_property ignored");
    return false;
}
#endif  // LV_USE_OBJ_PROPERTY

// Stage lb16 — arrange for the pending repaint to happen *now* instead of at
// the next LVGL timer slice. Rationale is in widget_property_set_batch();
// called under the LVGL port lock. Safe to call when nothing is dirty.
void kick_repaint_locked()
{
    // The default display is the only one this device has. Marking the
    // refresh timer ready makes it due immediately (an invalidation already
    // unpaused it — lv_inv_area -> LV_EVENT_REFR_REQUEST -> lv_timer_resume —
    // but resume alone does not make it due, and a paused timer is skipped
    // entirely by lv_timer_handler()).
    lv_timer_t *refr = lv_display_get_refr_timer(nullptr);
    if (!refr) return;
    lv_timer_resume(refr);
    lv_timer_ready(refr);
}

// Apply ONE entry (no locking — caller holds the LVGL port lock). Sets
// *touched_live when the override landed on a widget that is currently on
// screen, i.e. when the visible tree changed and needs repainting.
bool set_one_locked(const touchy_SetPropertyCmd &cmd, bool *touched_live)
{
    const std::string ident = ident_of(cmd);
    if (ident.empty()) {
        ESP_LOGW(TAG, "set_property: no property name/id supplied");
        return false;
    }

    const bool remove = (cmd.which_value == 0);
    bool ok = true;

    auto it = s_overrides.begin();
    for (; it != s_overrides.end(); ++it) {
        if (it->widget_id == cmd.widget_id && it->ident == ident) break;
    }

    if (remove) {
        if (it != s_overrides.end()) {
            s_overrides.erase(it);
            ESP_LOGI(TAG, "removed override %s.%s", cmd.widget_id, ident.c_str());
        }
    } else {
        if (it != s_overrides.end()) {
            it->cmd = cmd;
        } else {
            s_overrides.push_back(Override{ std::string(cmd.widget_id), ident, cmd });
        }
        lv_obj_t *obj = find_active(cmd.widget_id);
        if (obj) {
            ok = apply_to_obj(obj, cmd);
            if (ok) {
                // Belt and braces: LVGL's own property setters invalidate,
                // but marking the widget dirty here guarantees a repaint even
                // for a property whose setter forgets to (and it is what
                // unpauses the refresh timer — see kick_repaint_locked()).
                lv_obj_invalidate(obj);
                if (touched_live) *touched_live = true;
            }
        }
    }

    return ok;
}

}  // namespace

bool widget_property_set_batch(const touchy_SetPropertiesCmd &cmds)
{
    // googly-vr / stage lb14 — one RPC carries N overrides; apply them all
    // under a single lock acquisition so a whole animation frame lands
    // atomically. A failing entry logs + flips the result but never
    // aborts the remaining entries.
    bool ok           = true;
    bool touched_live = false;  // did an entry change a widget on screen?
    lvgl_port_lock(0);
    for (pb_size_t i = 0; i < cmds.props_count; i++) {
        ok = set_one_locked(cmds.props[i], &touched_live) && ok;
    }
    // Stage lb16 — the widgets this batch changed are dirty now, so make the
    // repaint happen immediately instead of at the next LVGL timer slice.
    //
    // Why the extra poke is needed (googly-vr sent ~10 fps and got ~2 fps): the
    // display refresh timer pauses itself at the top of every pass
    // (lv_display_refr_timer -> lv_timer_pause), and on an interrupt-driven
    // touch panel (this board's GT911 has an INT line, so lvgl_port_add_touch
    // puts the indev in LV_INDEV_MODE_EVENT) LVGL also leaves the indev read
    // timer paused — so while the panel is idle *every* timer is paused and
    // lv_timer_handler() reports LV_NO_TIMER_READY. esp_lvgl_port then sleeps
    // its full task_max_sleep_ms (500 ms, ESP_LVGL_PORT_INIT_CONFIG) before
    // looking again. An invalidation does resume the refresh timer
    // (lv_inv_area -> LV_EVENT_REFR_REQUEST -> lv_timer_resume), but
    // lv_timer_resume() only notifies the port via
    // lv_timer_handler_set_resume_cb() — which esp_lvgl_port never registers
    // (touchy-pad registers one of its own at display bring-up, see
    // display.cpp::post_init(), which already bounds any invalidation to the
    // 33 ms refresh period).
    //
    // This call goes further and makes the frame *immediate* rather than
    // within-a-refresh-period: `lv_timer_ready` makes the pass due now instead
    // of `period` ms after the last one, and the explicit wake keeps this path
    // working on its own rather than depending on that global registration.
    if (touched_live) kick_repaint_locked();
    lvgl_port_unlock();
    // Outside the lock (it is just an event-group set, and the LVGL task must
    // be free to take the port lock the moment it wakes). Nothing to do for a
    // batch that changed nothing visible — e.g. one that only staged overrides
    // for a screen that isn't loaded.
    if (touched_live) lvgl_port_task_wake(LVGL_PORT_EVENT_USER, nullptr);
    return ok;
}

void widget_property_build_reset()
{
    s_pending.clear();
}

void widget_property_register(const char *id, lv_obj_t *obj)
{
    if (!id || id[0] == '\0' || !obj) return;
    s_pending.push_back(IdObj{ std::string(id), obj });
    // Re-apply any sticky overrides that target this widget.
    for (auto &ov : s_overrides) {
        if (ov.widget_id == id) apply_to_obj(obj, ov.cmd);
    }
}

void widget_property_build_commit()
{
    s_active = std::move(s_pending);
    s_pending.clear();
}
