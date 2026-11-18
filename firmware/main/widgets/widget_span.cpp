// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage lb24 — rich text via LVGL span groups.
// See widget_span.h and docs/plans/span-rich-text.md.

#include "widget_span.h"
#include "tc_tag.h"
#include "widget_styles.h"   // color_from_u32 / font_for_size

#include "esp_log.h"

#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

static const char *TAG = TOUCHY_TAG("widgets.span");

#if LV_USE_SPAN

namespace {

// Distinguishes our `lv_obj_t` user_data from any other widget's (a label's
// Stage-lb19 scroll-speed context, for instance). Only ever read on an object
// that `lv_obj_check_type()` has already confirmed is a spangroup, and only this
// module ever creates spangroups — the tag is belt and braces.
constexpr uint32_t kCtxMagic = 0x5453504Eu;  // "TSPN"

struct SpanGroupCtx {
    uint32_t    magic = kCtxMagic;
    // Last markup parsed into this group, so a repeated identical `text`
    // override can skip the span churn (a chatty host re-sends the same string
    // whenever its own state changes).
    std::string last_markup;
};

void ctx_delete_cb(lv_event_t *e)
{
    delete static_cast<SpanGroupCtx *>(lv_event_get_user_data(e));
}

SpanGroupCtx *ctx_of(lv_obj_t *obj)
{
    auto *ctx = static_cast<SpanGroupCtx *>(lv_obj_get_user_data(obj));
    return (ctx && ctx->magic == kCtxMagic) ? ctx : nullptr;
}

// True for a spangroup this module created — i.e. one for which a `text`
// property override means "parse this markup into the span list".
bool is_markup_group(lv_obj_t *obj)
{
    if (!obj || !lv_obj_check_type(obj, &lv_spangroup_class)) return false;
    return ctx_of(obj) != nullptr;
}

// One parsed run: literal text plus the colour it runs in (if any).
struct SpanSpec {
    std::string text;
    bool        has_color = false;
    uint32_t    color     = 0;
    bool        has_font  = false;
    int32_t     font_size = 0;
};

// `#RRGGBB ` opener test: exactly six hex digits followed by a space.
bool is_opener(const char *p)
{
    for (int i = 0; i < 6; ++i) {
        const char c = p[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return p[6] == ' ';
}

uint32_t hex6_to_u32(const char *p)
{
    uint32_t v = 0;
    for (int i = 0; i < 6; ++i) {
        const char c = p[i];
        const uint32_t d = (c >= '0' && c <= '9') ? (uint32_t)(c - '0')
                           : (c >= 'a' && c <= 'f') ? (uint32_t)(c - 'a' + 10)
                                                    : (uint32_t)(c - 'A' + 10);
        v = (v << 4) | d;
    }
    return v;
}

// Parse `#RRGGBB text#` markup into runs, following LVGL's label-recolour
// semantics (minus the state-carrying defect): a run opens at `#` + six hex
// digits + a space (the space is consumed) and closes at the next `#`; an
// unmatched `#` is literal; an empty run contributes nothing; an unterminated
// run keeps its colour to the end of the string. Text outside a run gets no
// colour, so it inherits the group's own text colour.
void parse_markup(const char *markup, std::vector<SpanSpec> &out)
{
    auto flush = [&out](const char *begin, const char *end, bool colored, uint32_t color) {
        if (end <= begin) return;
        SpanSpec s;
        s.text.assign(begin, (size_t)(end - begin));
        s.has_color = colored;
        s.color     = color;
        out.push_back(std::move(s));
    };

    const char *seg   = markup;
    bool        colored = false;
    uint32_t    color   = 0;
    for (const char *p = markup; *p; ++p) {
        if (*p != '#') continue;
        if (!colored && is_opener(p + 1)) {
            // Opener. `p += 7` lands on the consumed space; the loop's ++p then
            // resumes at the first character of the run's text.
            flush(seg, p, false, 0);
            color   = hex6_to_u32(p + 1);
            colored = true;
            p += 7;
            seg = p + 1;
        } else if (colored) {
            // Closer — or a `#` that is not a valid opener while inside a run.
            flush(seg, p, true, color);
            colored = false;
            seg     = p + 1;
        }
    }
    flush(seg, markup + strlen(markup), colored, color);
}

// Replace `group`'s span list with `specs`. `lv_span_set_text()` copies, so the
// specs may be locals. Refreshes once at the end: `lv_span_set_text()` does not
// invalidate on its own.
void apply_specs(lv_obj_t *group, const std::vector<SpanSpec> &specs)
{
    // Drop the previous runs first, or every update would append a paragraph.
    while (lv_span_t *old = lv_spangroup_get_child(group, 0)) {
        lv_spangroup_delete_span(group, old);
    }
    for (const SpanSpec &spec : specs) {
        if (spec.text.empty()) continue;
        lv_span_t *sp = lv_spangroup_add_span(group);
        if (!sp) break;
        lv_span_set_text(sp, spec.text.c_str());
        // `lv_span_get_style()` is the span's built-in style: mutating it keeps
        // the per-span font / colour without duplicating a foreign style object.
        lv_style_t *st = lv_span_get_style(sp);
        if (spec.has_color) lv_style_set_text_color(st, color_from_u32(spec.color));
        if (spec.has_font) {
            if (const lv_font_t *font = font_for_size(spec.font_size))
                lv_style_set_text_font(st, font);
        }
    }
    lv_spangroup_refresh(group);
}

// Common wrapper setup for every span group we build: not an opaque theme box,
// fully opaque text, and carrying the "a `text` override means markup" marker.
void prepare_group(lv_obj_t *group)
{
    lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_opa(group, LV_OPA_COVER, 0);
    auto *ctx = new (std::nothrow) SpanGroupCtx{};
    if (!ctx) return;
    lv_obj_set_user_data(group, ctx);
    lv_obj_add_event_cb(group, ctx_delete_cb, LV_EVENT_DELETE, ctx);
}

// Shared with `build_label()`'s behaviour: AUTO leaves LVGL's default alone.
void apply_text_align(lv_obj_t *obj, touchy_TextAlign align)
{
    if (align == touchy_TextAlign_AUTO) return;
    static const lv_text_align_t map[] = {
        LV_TEXT_ALIGN_AUTO,
        LV_TEXT_ALIGN_LEFT,
        LV_TEXT_ALIGN_CENTER,
        LV_TEXT_ALIGN_RIGHT,
    };
    const int idx = (int)align;
    if (idx >= 0 && idx < 4) lv_obj_set_style_text_align(obj, map[idx], 0);
}

}  // namespace

void span_group_set_markup(lv_obj_t *group, const char *markup)
{
    if (!group || !markup) return;
    std::vector<SpanSpec> specs;
    parse_markup(markup, specs);
    apply_specs(group, specs);
    if (SpanGroupCtx *ctx = ctx_of(group)) ctx->last_markup = markup;
}

lv_obj_t *build_span_group(lv_obj_t *parent, const touchy_Widget &w)
{
    const touchy_SpanGroup &sg = w.kind.span_group;
    lv_obj_t *group = lv_spangroup_create(parent);
    if (!group) return nullptr;
    prepare_group(group);

    std::vector<SpanSpec> specs;
    specs.reserve(sg.spans_count);
    for (pb_size_t i = 0; i < sg.spans_count; ++i) {
        const touchy_Span &s = sg.spans[i];
        if (s.text[0] == '\0') continue;
        SpanSpec spec;
        spec.text      = s.text;
        spec.has_color = s.has_text_color;
        spec.color     = s.text_color;
        spec.has_font  = s.has_font_size;
        spec.font_size = s.font_size;
        specs.push_back(std::move(spec));
    }
    apply_specs(group, specs);

    // No `lv_spangroup_set_mode()` call on purpose: it is deprecated in LVGL
    // 9.5 (the mode follows the object's size) and would stomp the size that
    // `apply_rect()` sets right after we return.
    lv_spangroup_set_overflow(group,
        sg.overflow == touchy_SpanOverflow_SPAN_OVERFLOW_ELLIPSIS
            ? LV_SPAN_OVERFLOW_ELLIPSIS : LV_SPAN_OVERFLOW_CLIP);
    // LVGL wants `< 0` for "unlimited"; the wire spells that 0.
    lv_spangroup_set_max_lines(group, sg.max_lines > 0 ? sg.max_lines : -1);
    apply_text_align(group, sg.text_align);
    lv_spangroup_refresh(group);

    ESP_LOGI(TAG, "build_span_group id='%s' spans=%u overflow=%d max_lines=%ld "
                  "text_align=%d first='%.40s'",
             w.id, (unsigned)specs.size(), (int)sg.overflow, (long)sg.max_lines,
             (int)sg.text_align, specs.empty() ? "" : specs[0].text.c_str());
    return group;
}

lv_obj_t *build_recolor_span_group(lv_obj_t *parent, const touchy_Widget &w)
{
    const touchy_LongMode lm = w.kind.label.long_mode;
    // A span group cannot marquee, so the two scrolling modes keep the plain
    // label path — where the LVGL parser defect cannot bite either, because a
    // scrolling label lays its text out as a single unwrapped line.
    if (lm == touchy_LongMode_LONG_MODE_SCROLL ||
        lm == touchy_LongMode_LONG_MODE_SCROLL_CIRCULAR) {
        return nullptr;
    }

    lv_obj_t *group = lv_spangroup_create(parent);
    if (!group) return nullptr;
    prepare_group(group);
    span_group_set_markup(group, w.kind.label.text);
    // The object's own height bounds the text exactly as it does for a label;
    // DOTS adds the ellipsis, WRAP / CLIP just clip.
    lv_spangroup_set_overflow(group, lm == touchy_LongMode_LONG_MODE_DOTS
                                          ? LV_SPAN_OVERFLOW_ELLIPSIS
                                          : LV_SPAN_OVERFLOW_CLIP);
    lv_spangroup_set_max_lines(group, -1);
    apply_text_align(group, w.kind.label.text_align);
    lv_spangroup_refresh(group);

    ESP_LOGI(TAG, "build_recolor_span_group id='%s' text='%.40s' long_mode=%d "
                  "text_align=%d",
             w.id, w.kind.label.text, (int)lm, (int)w.kind.label.text_align);
    return group;
}

bool span_group_apply_text_override(lv_obj_t *obj,
                                    const touchy_SetPropertyCmd &cmd)
{
    if (!is_markup_group(obj)) return false;
    if (cmd.which_property != touchy_SetPropertyCmd_property_name_tag) return false;
    if (strcmp(cmd.property.property_name, "text") != 0) return false;
    if (cmd.which_value != touchy_SetPropertyCmd_string_value_tag) return false;

    SpanGroupCtx *ctx = ctx_of(obj);
    if (ctx && ctx->last_markup == cmd.value.string_value) {
        // Identical markup: skip the span churn. (The caller still invalidates a
        // "changed" widget, which is a no-op for an unchanged drawing.)
        ESP_LOGD(TAG, "text override unchanged for markup spangroup, skipped");
        return true;
    }
    span_group_set_markup(obj, cmd.value.string_value);
    ESP_LOGI(TAG, "span group text override: '%.40s'", cmd.value.string_value);
    return true;
}

#else   // !LV_USE_SPAN

// LVGL's span widget is compiled out. The explicit `span_group` kind cannot be
// rendered at all, and `Label.recolor` stays on LVGL's own parser (which
// `build_label()` falls back to) — including its wrapped-run defect. Only
// reachable on a hand-rolled sdkconfig: every shipped board builds with
// CONFIG_LV_USE_SPAN=y (firmware/sdkconfig.defaults now states it explicitly).
lv_obj_t *build_span_group(lv_obj_t *parent, const touchy_Widget &w)
{
    (void)parent;
    ESP_LOGW(TAG, "widget '%s' is a span_group but LV_USE_SPAN is off — skipped",
             w.id);
    return nullptr;
}

lv_obj_t *build_recolor_span_group(lv_obj_t *parent, const touchy_Widget &w)
{
    (void)parent;
    (void)w;
    return nullptr;
}

void span_group_set_markup(lv_obj_t *group, const char *markup)
{
    (void)group;
    (void)markup;
}

bool span_group_apply_text_override(lv_obj_t *obj, const touchy_SetPropertyCmd &cmd)
{
    (void)obj;
    (void)cmd;
    return false;
}

#endif  // LV_USE_SPAN
