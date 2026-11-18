// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage lb12 — runtime widget property overrides. See widget_property.h.

#include "widget_property.h"
#include "tc_tag.h"
#include "widget_span.h"     // Stage lb24 — markup-backed span groups take `text`
#include "widget_styles.h"   // Stage lb26 — font_for_size() for font_size_value

#include "esp_log.h"
#include "esp_lvgl_port.h"

#include <algorithm>
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

// Stage lb21 (fork-review fix) — drop a widget from the registry when LVGL
// destroys it.
//
// `widget_refs_change()` tears an in-place WidgetRef subtree down and rebuilds
// it, so `s_active` used to keep `lv_obj_t*` pointers to objects that no longer
// existed: a later set_property could write through freed memory, and the ids
// of the rebuilt widgets never left `s_pending`.
// `lv_obj_tree.c::obj_delete_core()` sends LV_EVENT_DELETE to the object and
// then recurses into every child, so hooking this keeps the registry truthful
// for *every* teardown path — full screen reload, in-place ref swap, plain
// widget delete — without having to replicate the Stage-85 descendant walk in
// a second place.
//
// Match by pointer, not by id: a rebuild normally creates a widget carrying the
// same `Widget.id` as the one being deleted, and the fresh object must survive.
void delete_cb(lv_event_t *e)
{
    lv_obj_t *obj = static_cast<lv_obj_t *>(lv_event_get_target(e));
    if (!obj) return;
    auto drop = [obj](std::vector<IdObj> &v) {
        v.erase(std::remove_if(v.begin(), v.end(),
                               [obj](const IdObj &entry) { return entry.obj == obj; }),
                v.end());
    };
    drop(s_active);
    drop(s_pending);
}

#if LV_USE_OBJ_PROPERTY
// Resolve an entry's property ident to an LVGL property id (name → the
// widget's own property, else a style property; numeric → as-is).
lv_prop_id_t resolve_prop_id(lv_obj_t *obj, const touchy_SetPropertyCmd &cmd)
{
    if (cmd.which_property == touchy_SetPropertyCmd_property_name_tag) {
        return lv_obj_property_get_id(obj, cmd.property.property_name);
    }
    if (cmd.which_property == touchy_SetPropertyCmd_property_id_tag) {
        return (lv_prop_id_t)cmd.property.property_id;
    }
    return LV_PROPERTY_ID_INVALID;
}

// Does this ident name the text-font style property? Compare the *index*, not
// the whole id: a name resolves to LV_PROPERTY_STYLE_TEXT_FONT (style-prop
// index OR'd with LVGL's type bits) while a host that used a raw
// `property_id` sends the bare LV_STYLE_TEXT_FONT.
bool is_text_font_prop(lv_prop_id_t pid)
{
    return LV_PROPERTY_ID_INDEX(pid) == (uint32_t)LV_STYLE_TEXT_FONT;
}

// Stage lb26 — apply a `font_size_value` entry (the pixel size is carried
// instead of the `const lv_font_t *` the value really is, because a pointer
// cannot travel on the wire).
//
// This must NOT go through the generic lv_obj_set_property() below: LVGL would
// hand the integer to lv_obj_set_local_style_prop() as the style value, and the
// lv_property_t union overlaps `num` with `ptr`/`style` — i.e. the device would
// treat `30` as a font pointer and crash in the draw path.
bool apply_font_size(lv_obj_t *obj, const touchy_SetPropertyCmd &cmd, lv_prop_id_t pid)
{
    if (!is_text_font_prop(pid)) {
        ESP_LOGW(TAG, "font_size_value needs the 'text_font' property ('%s' on '%s')",
                 ident_of(cmd).c_str(), cmd.widget_id);
        return false;
    }

    const int32_t px = (int32_t)cmd.value.font_size_value;
    if (px == touchy_FontSize_FONT_SIZE_DEFAULT) {
        // FONT_SIZE_DEFAULT: drop the local font so the widget inherits its
        // style stack (and ultimately the theme) again — the runtime mirror of
        // an unset `Style.font_size`.
        const bool dropped = lv_obj_remove_local_style_prop(obj, LV_STYLE_TEXT_FONT, 0);
        ESP_LOGI(TAG, "font_size=%d on '%s' (%s)", (int)px, cmd.widget_id,
                 dropped ? "dropped the local font" : "no local font to drop");
        return true;
    }

    const lv_font_t *font = font_for_size(px);
    if (!font) return false;   // font_for_size() logged the WARN

    // Selector 0 == LV_PART_MAIN | LV_STATE_DEFAULT, matching how the generic
    // style-property path applies an override (it leaves `selector` unset).
    lv_obj_set_style_text_font(obj, font, 0);
    ESP_LOGI(TAG, "font_size=%d on '%s'", (int)px, cmd.widget_id);
    return true;
}

// Apply one override's value to a live object. Returns false if the
// property can't be resolved / set. An unset value is a no-op here (the
// removal only affects future rebuilds).
bool apply_to_obj(lv_obj_t *obj, const touchy_SetPropertyCmd &cmd)
{
    if (cmd.which_value == 0) return true;

    // Stage lb24 — a markup-backed span group (an explicit `span_group` widget,
    // or the object that implements a `recolor` label) has no LVGL `text`
    // property, so a `text` write means "re-parse this markup into the span
    // list". That keeps a chatty host's live update at one RPC per frame, with
    // the same override semantics as a label's.
    if (span_group_apply_text_override(obj, cmd)) return true;

    const lv_prop_id_t pid = resolve_prop_id(obj, cmd);
    if (pid == LV_PROPERTY_ID_INVALID) {
        ESP_LOGW(TAG, "unknown property '%s' on widget '%s'",
                 cmd.which_property == touchy_SetPropertyCmd_property_name_tag
                     ? cmd.property.property_name : "(numeric)",
                 cmd.widget_id);
        return false;
    }

    // Stage lb26 — the typed font-size arm, and the reverse footgun: the
    // `text_font` style property's real value is a `const lv_font_t *`, so
    // *every* other value arm aimed at it (int / bool / colour / string /
    // point) would be handed to LVGL as that pointer.
    if (cmd.which_value == touchy_SetPropertyCmd_font_size_value_tag) {
        return apply_font_size(obj, cmd, pid);
    }
    if (is_text_font_prop(pid)) {
        ESP_LOGW(TAG,
                 "widget '%s': 'text_font' takes font_size_value (a pixel size) — "
                 "any other value would be read as a font pointer",
                 cmd.widget_id);
        return false;
    }

    lv_property_t prop;
    lv_memzero(&prop, sizeof(prop));
    prop.id = pid;
    // NB: `font_size_value` never reaches here — it is applied by
    // apply_font_size() above, because it carries a pixel size rather than a
    // value the generic property setter can consume.
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
    // Stage lb21 (fork-review fix) — track this object's destruction so the
    // registry can never hand a freed pointer back to set_property(). One
    // callback per identified widget; registering the same object twice (a
    // build path that both expands a ref and registers its inner widget) only
    // adds a redundant callback that finds nothing to erase.
    lv_obj_add_event_cb(obj, delete_cb, LV_EVENT_DELETE, nullptr);
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

void widget_property_build_merge()
{
    // Stage lb21 (fork-review fix) — promote the ids registered by a *partial*
    // rebuild (an in-place WidgetRef swap) without disturbing the widgets that
    // rebuild did not touch. `widget_property_build_commit()` cannot be used
    // here: it replaces the whole map with just the swapped subtree, which
    // would strand every override target outside it (the chrome's prev/next
    // buttons, sibling refs, ...).
    //
    // Same id ⇒ newer object: the swap deleted the previous object, so its
    // entry is stale even if the LV_EVENT_DELETE hook somehow did not run.
    // Last registration wins, matching "the widget the host can see now".
    for (auto &pending : s_pending) {
        for (size_t i = 0; i < s_active.size();) {
            if (s_active[i].id == pending.id) {
                s_active.erase(s_active.begin() + i);
            } else {
                i++;
            }
        }
        s_active.push_back(std::move(pending));
    }
    s_pending.clear();
}
