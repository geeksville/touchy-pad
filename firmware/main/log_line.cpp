// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 18 — shared device log sink. See log_line.h.

#include "log_line.h"

#include "esp_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

static const char *TAG = "logw";

namespace {

// All live LogLine instances. Mutated only from the LVGL task (widget
// construction/destruction and `log_line_post` are all expected to run
// under the LVGL port lock), so no extra synchronisation is needed.
std::vector<LogLine *> &registry()
{
    static auto *v = new std::vector<LogLine *>();
    return *v;
}

// Cached most-recent line. `log_line_post` updates this; freshly
// constructed widgets pick it up so they don't start blank.
char s_last_line[128] = "";

}  // namespace

extern "C" void log_line_post(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_last_line, sizeof(s_last_line), fmt, args);
    va_end(args);

    ESP_LOGI(TAG, "%s", s_last_line);

    for (LogLine *w : registry()) {
        w->_update(s_last_line);
    }
}

LogLine::LogLine(lv_obj_t *parent)
{
    _container = lv_obj_create(parent);
    // Sensible defaults; host DSL can override via Style / Rect.
    lv_obj_set_style_bg_color(_container, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_container, 0, 0);
    lv_obj_set_style_pad_all(_container, 4, 0);
    lv_obj_set_scrollbar_mode(_container, LV_SCROLLBAR_MODE_OFF);
    // Default to full-width by 30 px tall; the layout pass will resize.
    lv_obj_set_size(_container, LV_PCT(100), 30);

    _label = lv_label_create(_container);
    lv_obj_set_style_text_color(_label, lv_color_hex(0x00FF88), 0);
    lv_label_set_long_mode(_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(_label, LV_PCT(100));
    lv_label_set_text(_label, s_last_line[0] ? s_last_line : "Ready");

    registry().push_back(this);
    lv_obj_add_event_cb(_container, _deleteCb, LV_EVENT_DELETE, this);
}

void LogLine::_update(const char *line)
{
    if (_label && line) {
        lv_label_set_text(_label, line);
    }
}

void LogLine::_deleteCb(lv_event_t *e)
{
    auto *self = static_cast<LogLine *>(lv_event_get_user_data(e));
    auto &reg = registry();
    for (auto it = reg.begin(); it != reg.end(); ++it) {
        if (*it == self) { reg.erase(it); break; }
    }
    delete self;
}
