// SPDX-License-Identifier: Apache-2.0
//
// Touchy-Pad host-uploaded screen/component registry — see screens.h.

#include "screens.h"

#include "esp_log.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

#include <cstring>
#include <string>

static const char *TAG = "screens";

// True after screens_init() has run lv_xml_init() so we don't double-init.
static bool s_inited = false;

// Case-insensitive endswith.
static bool ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s);
    size_t lx = strlen(suffix);
    if (lx > ls) return false;
    return strcasecmp(s + ls - lx, suffix) == 0;
}

void screens_init(void)
{
    if (s_inited) return;
    // lv_xml_* APIs touch LVGL internals; grab the LVGL port lock first.
    lvgl_port_lock(0);
    lv_xml_init();
    lvgl_port_unlock();
    s_inited = true;
    ESP_LOGI(TAG, "lv_xml_init done");
}

bool screens_register_from_file(const char *path)
{
    if (!path || !*path) return false;
    if (!ends_with(path, ".xml")) {
        // Non-XML assets (fonts, images, …) are reachable via "F:" paths
        // and don't need a separate registration step — the XML referencing
        // them resolves the path at parse time.
        ESP_LOGI(TAG, "skipping non-xml registration for %s", path);
        return true;
    }

    if (!s_inited) screens_init();

    // LVGL's parser expects "<letter>:..." paths. Our Fs::begin() registers
    // the 'F' letter at /littlefs/from_host/, so we just prepend it here.
    std::string lv_path = std::string("F:") + path;

    lvgl_port_lock(0);
    lv_result_t r = lv_xml_component_register_from_file(lv_path.c_str());
    lvgl_port_unlock();

    if (r != LV_RESULT_OK) {
        ESP_LOGE(TAG, "lv_xml_component_register_from_file(%s) failed: %d",
                 lv_path.c_str(), (int)r);
        return false;
    }
    ESP_LOGI(TAG, "registered xml component from %s", lv_path.c_str());
    return true;
}

bool screens_load(const char *name)
{
    if (!name || !*name) return false;
    if (!s_inited) screens_init();

    bool ok = false;
    lvgl_port_lock(0);

    // LVGL 9.3's `lv_xml_create` does NOT create a screen when parent is NULL
    // (it logs "no parent object available for view" and bails). Instead we
    // create a fresh screen ourselves and instantiate the registered
    // component as its child, then activate the screen.
    lv_obj_t *scr = lv_obj_create(NULL); // FIXME - I think this is wrong
    if (scr) {
        lv_obj_t *content = (lv_obj_t *)lv_xml_create(scr, name, NULL);
        if (content) {
            // Make the component fill the screen so XML layouts that
            // expect to size against their parent see the full display.
            lv_obj_set_size(content, lv_pct(100), lv_pct(100));
            lv_screen_load(scr);
            ok = true;
        } else {
            lv_obj_delete(scr);
        }
    }

    lvgl_port_unlock();

    if (ok) {
        ESP_LOGI(TAG, "loaded screen '%s'", name);
    } else {
        ESP_LOGE(TAG, "lv_xml_create('%s') returned NULL — registered?", name);
    }
    return ok;
}
