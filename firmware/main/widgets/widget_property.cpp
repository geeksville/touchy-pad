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

}  // namespace

bool widget_property_set(const touchy_SetPropertyCmd &cmd)
{
    const std::string ident = ident_of(cmd);
    if (ident.empty()) {
        ESP_LOGW(TAG, "set_property: no property name/id supplied");
        return false;
    }

    const bool remove = (cmd.which_value == 0);
    bool ok = true;

    lvgl_port_lock(0);

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
        }
    }

    lvgl_port_unlock();
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
