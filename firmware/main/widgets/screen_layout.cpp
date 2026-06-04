// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 15 / 20.1 / 24.1 / 24.2 — layout-widget configuration + child placement. See header.

#include "screen_layout.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "screens.layout";

bool widget_is_layout(const touchy_Widget &w)
{
    return w.which_kind == touchy_Widget_layout_absolute_tag ||
           w.which_kind == touchy_Widget_layout_flex_tag ||
           w.which_kind == touchy_Widget_layout_grid_tag;
}

void apply_rect(lv_obj_t *obj, const touchy_Widget &w, bool absolute_layout,
                const touchy_Widget *parent_layout)
{
    const bool has_rect = (w.which_placement == touchy_Widget_rect_tag);

    // Stage 72 — the "grow to fill" opt-in moved to `Widget.grow_x` /
    // `Widget.grow_y` (independent of the placement oneof). Under a flex
    // parent each axis splits into a main axis (a true flex-grow factor)
    // and a cross axis (LVGL has no STRETCH cross-align in v9, so a grow
    // there just means "fill the parent" → lv_pct(100)). Work out which
    // of grow_x / grow_y is the parent's main axis so we can apply each
    // correctly.
    bool parent_is_flex = false;
    bool parent_is_row = false;  // only meaningful when parent_is_flex
    if (!absolute_layout && parent_layout &&
        parent_layout->which_kind == touchy_Widget_layout_flex_tag) {
        parent_is_flex = true;
        switch (parent_layout->kind.layout_flex.flow) {
        case touchy_LayoutFlex_Flow_ROW:
        case touchy_LayoutFlex_Flow_ROW_WRAP:
        case touchy_LayoutFlex_Flow_ROW_REVERSE:
        case touchy_LayoutFlex_Flow_ROW_WRAP_REVERSE:
            parent_is_row = true;
            break;
        default:
            break;  // COLUMN family — main axis is the height (grow_y)
        }
    }

    // Cross-axis "fill the parent" requests (lv_pct(100)) derived from the
    // grow flags. For a ROW parent the cross axis is the height (grow_y);
    // for a COLUMN parent it is the width (grow_x). We deliberately do NOT
    // fill the cross axis of a ROW with grow_y unless asked: a pct(100)
    // height against a content-height row is a circular dependency that
    // collapses the row.
    const bool fill_w = parent_is_flex && !parent_is_row && w.grow_x > 0;
    const bool fill_h = parent_is_flex && parent_is_row && w.grow_y > 0;

    // Main-axis flex-grow factor (a ROW parent's main axis is the width →
    // grow_x; a COLUMN parent's is the height → grow_y). A positive factor
    // lets this child share the parent's free space — e.g. a spacer that
    // pushes its siblings to opposite edges, or a body that fills the area
    // below a content-sized chrome row.
    const int32_t main_grow =
        parent_is_flex ? (parent_is_row ? w.grow_x : w.grow_y) : 0;

    // Nothing to apply: no rect, not absolute, no cross-axis fill, and no
    // main-axis grow. (A bare layout child with no grow simply lets the
    // layout manager size it to content.)
    if (!has_rect && !absolute_layout && !fill_w && !fill_h && main_grow <= 0)
        return;

    const touchy_Rect empty_rect = touchy_Rect_init_zero;
    const auto &r = has_rect ? w.placement.rect : empty_rect;

    if (absolute_layout) {
        lv_obj_set_pos(obj, r.x, r.y);
    }

    int32_t w_ = r.w > 0 ? r.w
                 : (absolute_layout || fill_w ? lv_pct(100) : LV_SIZE_CONTENT);
    int32_t h_ = r.h > 0 ? r.h
                 : (absolute_layout || fill_h ? lv_pct(100) : LV_SIZE_CONTENT);
    lv_obj_set_size(obj, w_, h_);

    ESP_LOGD(TAG,
             "apply_rect id='%s' kind=%d has_rect=%d r=(%ld,%ld,%ld,%ld) "
             "grow=(%ld,%ld) abs=%d fill=(%d,%d) -> w=%s h=%s",
             w.id, (int)w.which_kind, (int)has_rect,
             (long)r.x, (long)r.y, (long)r.w, (long)r.h,
             (long)w.grow_x, (long)w.grow_y,
             (int)absolute_layout, (int)fill_w, (int)fill_h,
             w_ == LV_SIZE_CONTENT ? "CONTENT" : (w_ == lv_pct(100) ? "100%" : "px"),
             h_ == LV_SIZE_CONTENT ? "CONTENT" : (h_ == lv_pct(100) ? "100%" : "px"));

    // Stage 72 — main-axis flex-grow: apply the factor computed above so
    // this child shares the parent's free space along the main axis.
    if (main_grow > 0) {
        lv_obj_set_flex_grow(obj, (uint8_t)main_grow);
    }
}

// Place a widget inside a GRID-layout parent. The grid manager owns size
// and position, so we only forward the cell spec from the protobuf and ask
// LVGL to stretch the widget across its assigned track(s).
//
// Proto3 zero-default handling: in a proto3 binary the wire encoding for a
// scalar field whose value equals its default is omitted entirely. That
// means col=0, row=0, col_span=0, row_span=0 are all indistinguishable from
// "field not present". We therefore treat each value as its proto-comment
// specifies: col/row default to track 0 (top-left), col_span/row_span
// default to 1 (single track). Negative values are also clamped.
void apply_grid_cell(lv_obj_t *obj, const touchy_Widget &w)
{
    int32_t col = 0, row = 0, col_span = 1, row_span = 1;
    if (w.which_placement == touchy_Widget_cell_tag) {
        col      = w.placement.cell.col > 0 ? w.placement.cell.col : 0;
        row      = w.placement.cell.row > 0 ? w.placement.cell.row : 0;
        col_span = w.placement.cell.has_col_span ? w.placement.cell.col_span : 1;
        row_span = w.placement.cell.has_row_span ? w.placement.cell.row_span : 1;
    }
    // Stage 72 — `Widget.grow_x` / `grow_y` opt the child into filling its
    // track(s): grow > 0 → LV_GRID_ALIGN_STRETCH on that axis, otherwise
    // LV_GRID_ALIGN_CENTER so the widget sits content-sized at the centre
    // of its cell. (Grid tracks have no proportional per-child grow, so the
    // magnitude is ignored.)
    lv_grid_align_t col_align = w.grow_x > 0 ? LV_GRID_ALIGN_STRETCH : LV_GRID_ALIGN_CENTER;
    lv_grid_align_t row_align = w.grow_y > 0 ? LV_GRID_ALIGN_STRETCH : LV_GRID_ALIGN_CENTER;
    ESP_LOGD(TAG, "apply_grid_cell id='%s' col=%ld row=%ld col_span=%ld row_span=%ld "
             "grow=(%ld,%ld)",
             w.id, (long)col, (long)row, (long)col_span, (long)row_span,
             (long)w.grow_x, (long)w.grow_y);
    lv_obj_set_grid_cell(obj,
                         col_align, col, col_span,
                         row_align, row, row_span);
}

namespace {

// Maps LayoutFlex.Flow enum values to lv_flex_flow_t.
lv_flex_flow_t flex_flow_from_proto(touchy_LayoutFlex_Flow f)
{
    switch (f) {
    case touchy_LayoutFlex_Flow_ROW:                 return LV_FLEX_FLOW_ROW;
    case touchy_LayoutFlex_Flow_COLUMN:              return LV_FLEX_FLOW_COLUMN;
    case touchy_LayoutFlex_Flow_ROW_WRAP:            return LV_FLEX_FLOW_ROW_WRAP;
    case touchy_LayoutFlex_Flow_ROW_REVERSE:         return LV_FLEX_FLOW_ROW_REVERSE;
    case touchy_LayoutFlex_Flow_ROW_WRAP_REVERSE:    return LV_FLEX_FLOW_ROW_WRAP_REVERSE;
    case touchy_LayoutFlex_Flow_COLUMN_WRAP:         return LV_FLEX_FLOW_COLUMN_WRAP;
    case touchy_LayoutFlex_Flow_COLUMN_REVERSE:      return LV_FLEX_FLOW_COLUMN_REVERSE;
    case touchy_LayoutFlex_Flow_COLUMN_WRAP_REVERSE: return LV_FLEX_FLOW_COLUMN_WRAP_REVERSE;
    default:                                         return LV_FLEX_FLOW_ROW;
    }
}

}  // namespace

void apply_layout(lv_obj_t *parent, const touchy_Widget &w)
{
    switch (w.which_kind) {
    case touchy_Widget_layout_flex_tag: {
        const touchy_LayoutFlex &fl = w.kind.layout_flex;
        lv_obj_set_flex_flow(parent, flex_flow_from_proto(fl.flow));
        ESP_LOGD(TAG, "apply_layout flex id='%s' flow=%d gap=%ld",
                 w.id, (int)fl.flow, (long)fl.gap);
        if (fl.gap > 0) {
            lv_obj_set_style_pad_column(parent, fl.gap, 0);
            lv_obj_set_style_pad_row(parent, fl.gap, 0);
        }
        break;
    }
    case touchy_Widget_layout_grid_tag: {
        const touchy_LayoutGrid &g = w.kind.layout_grid;
        // Track templates. `cols` columns split the parent into equal
        // fractional units; `rows` does the same vertically when > 0,
        // otherwise we use a single content-sized row.
        //
        // Proto3 zero-default: `cols=0` is treated as "use 1 column".
        // `rows=0` deliberately means "content-sized single row" per the
        // proto comment; use rows ≥ 1 to get FR-sized rows.
        //
        // The descriptor arrays must outlive the call to
        // lv_obj_set_grid_dsc_array — LVGL only stores the pointer.
        // Stage 57: heap-allocate per obj and free on LV_EVENT_DELETE.
        // (Earlier code used `static` buffers, which corrupted any
        //  outer grid as soon as an inner widget_ref expanded into a
        //  sub-grid and overwrote the shared template.)
        int cols = g.cols > 0 ? (int)g.cols : 1;
        if (cols > 64) cols = 64;
        int rows = (int)g.rows;
        if (rows > 64) rows = 64;

        const size_t col_n = (size_t)cols + 1;
        const size_t row_n = (rows > 0) ? (size_t)rows + 1 : 2;
        int32_t *col_dsc = (int32_t *)heap_caps_malloc(
            sizeof(int32_t) * (col_n + row_n), MALLOC_CAP_DEFAULT);
        if (!col_dsc) {
            ESP_LOGE(TAG, "apply_layout: OOM allocating grid template");
            break;
        }
        int32_t *row_dsc = col_dsc + col_n;
        for (int i = 0; i < cols; i++) col_dsc[i] = LV_GRID_FR(1);
        col_dsc[cols] = LV_GRID_TEMPLATE_LAST;
        if (rows > 0) {
            for (int i = 0; i < rows; i++) row_dsc[i] = LV_GRID_FR(1);
            row_dsc[rows] = LV_GRID_TEMPLATE_LAST;
        } else {
            row_dsc[0] = LV_GRID_CONTENT;
            row_dsc[1] = LV_GRID_TEMPLATE_LAST;
        }
        lv_obj_set_grid_dsc_array(parent, col_dsc, row_dsc);
        // Free the joint allocation when the obj is destroyed. The
        // single `col_dsc` pointer owns both halves.
        lv_obj_add_event_cb(
            parent,
            [](lv_event_t *e) {
                int32_t *p = (int32_t *)lv_event_get_user_data(e);
                if (p) heap_caps_free(p);
            },
            LV_EVENT_DELETE,
            col_dsc);
        lv_obj_set_layout(parent, LV_LAYOUT_GRID);
        if (g.gap > 0) {
            lv_obj_set_style_pad_column(parent, g.gap, 0);
            lv_obj_set_style_pad_row(parent, g.gap, 0);
        }
        break;
    }
    case touchy_Widget_layout_absolute_tag:
    default:
        // No layout manager — children place themselves via lv_obj_set_pos.
        break;
    }
}
