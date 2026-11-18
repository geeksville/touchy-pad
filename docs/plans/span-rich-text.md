# Rich text with span groups — and `recolor` reimplemented on top of them

**Status: implemented — Stage lb24** (firmware build, host suite and simulator
validated; the on-hardware wrap check is the one thing still to run — see §10).
The **approach was decided** by the maintainer in response to this bug report
(§3): keep the `recolor` wire field and API byte-for-byte identical and make the
*firmware* render it with LVGL **span groups** instead of
`lv_label_set_recolor()`, so no existing host (voicebot in particular) has to
change a line. `Span` / `SpanGroup` are the new, recommended way to write
multi-colour text.

**Deviations from this plan as written** (details in
`docs/hardware/lightbar/design.md` → "stage lb24"):

* No `SpanMode`/mode field — LVGL 9.5 deprecated `lv_spangroup_set_mode()`, so
  the mode follows the object's size (as §4 noted, but the sketch's enum is gone).
* The demo page is registered in `build_user_pages()` rather than `pages/test.py`
  (so the `F:host/uscr/` page cycle is now span → test → trackpad, and the tests
  pinning the old order were updated deliberately).
* `span(color=…)` / `span(font_size=…)` use `None` (not `0`) for "inherit", so an
  explicit black run stays expressible. §6.1's signature sketches `= 0`.
* The sim's `text`-override removal is lossless *immediately* (it re-materialises
  from pristine bytes) where the device restores on the next rebuild — documented
  as a harmless sim advantage.
* Actual measured results: **324 passed / 2 skipped** (`just app-test`),
  `just app-lint` clean, `just firmware-build` green (44 % flash free), and an
  end-to-end sim run of the new page including a live markup override.

| | |
|---|---|
| **Stage name if implemented** | `lb24` — **collides** with `docs/plans/thought-bubbles.md`, which also claims `lb24`; whichever lands second becomes `lb25`. Last landed stage is `lb23`. |
| **Sides touched** | `proto/`, `firmware/main/`, `app/` (DSL + sim + tests + docs); `rust/` only through the regenerated prost bindings — **no Rust crate code change** |
| **Wire impact** | new `Span` / `SpanGroup` messages + `Widget.kind.span_group = 34`; **`Widget.Version` 30 → 31**. `SysBoardInfoResponse.ProtocolVersion` (14) and `PreferencesFile.Version` (9) unchanged |
| **Just recipes that validate it** | `just build-proto`, `just app-test`, `just app-lint`, `just firmware-build` (+ `just rust-gen-proto` for the committed prost file) |

## 1. The bug this fixes

Reported by **voicebot** against a `jc4827w543` (480×272 LCD, `Widget.Version`
30, protocol 14, LVGL 9.5.0). A `label` with `recolor = true` and a *wrapping*
long mode (`WRAP` / `DOTS`) mis-renders when an automatic line break falls
**inside** a `#RRGGBB …#` run. On the continuation line the run's colour is lost,
the *closing* `#` is re-read as an *opening* command, the next word is swallowed
as the colour parameter, and glyphs at the seam are drawn twice — so words
overlap, the colour run ends early, and the line overruns the box.

Nothing in touchy-pad draws recoloured text by hand: `build_label()`
(`firmware/main/widgets/widget_builders.cpp:291`) only calls
`lv_label_set_recolor(lbl, true)`. The defect is inside LVGL's label recolour
path, in two independent places, both of which reset the parser state that a
`#RRGGBB …#` run depends on:

* **Measurement** — `lv_text_get_next_line()` (`src/misc/lv_text.c`) creates
  `lv_text_cmd_state_t cmd_state = LV_TEXT_CMD_STATE_WAIT;` **per call**, and its
  caller passes `&txt[line_start]` per line. A line beginning inside a run starts
  in `WAIT`, so `lv_text_is_cmd()` treats the closing `#` as an opener: `#`,
  the following word and the next space are all measured at **zero width**, and
  word wrap therefore fits an extra word onto the line.
* **Drawing** — `lv_draw_label()` (`src/draw/lv_draw_label.c`) declares
  `recolor_cmd_state` / `is_first_space_after_cmd` *after* its "skip to the first
  visible line" loop, so any pass that starts at a line > 0
  (`LV_LABEL_LONG_TXT_HINT` fast path, a clipped/tiled draw) re-parses from
  mid-run: `head` is drawn in the base colour, `I ` is dropped, and the rest of
  the line shifts — composited with a pass that started at line 0 you get the two
  offset copies.

`lv_text_get_width()` has the same per-call reset, so centre/right aligned
recoloured wrapped labels are hit too. This looks like the same long-standing
defect as LVGL issues [#4599](https://github.com/lvgl/lvgl/issues/4599) (v8) and
[#8547](https://github.com/lvgl/lvgl/issues/8547) (v9.3, closed stale with the fix
PR unmerged). `recolor` was even removed in early v9 precisely because it was
"too limited … can be replaced by `lv_span`", and `lv_span`/`lv_spangroup` is
still LVGL's supported rich-text path today: each span is its own styled run, so
there is **no in-string command parsing to carry across lines** and this bug
class cannot occur.

Blast radius: **any** host that puts a multi-colour string in a fixed-width label
hits this as soon as a coloured run straddles a line break — i.e. every LCD
layout that wants multi-colour text. voicebot's default
`--display-mode multiline` is unusable for multi-colour quips because of it (its
LED marquee path uses `LONG_MODE_SCROLL_CIRCULAR` and never auto-wraps, so it is
unaffected — and that mode is deliberately left alone, see §3.3).

## 2. What the stack can express today (verified against the code)

* **The wire has exactly two text widgets**: `Label` (`proto/widgets.proto:614`,
  `text` / `text_align` / `long_mode` / `recolor` / `scroll_*`) and `Button`
  (`text` only). Multi-colour text is `Label.recolor` (Stage lb17) plus LVGL's
  `#RRGGBB text#` markup.
* **`recolor` is not reachable through the property system** — the label property
  table exposes only `text` / `long_mode` / `text_selection_*`
  (`lv_label.c`, `lv_label_properties[]`), so it needs a real proto field. That
  is why lb17 added one.
* **A live `text` override re-parses the markup** on the device: the override
  lands on `lv_obj_set_property(obj, {"text", …})` → `lv_label_set_text()` →
  `lv_label_refr_text()`. One RPC per new string (≤ 511 bytes,
  `SetPropertyCmd.string_value` cap, `proto/touchy.options`).
* **`LV_USE_SPAN` is already on** in every board's config
  (`CONFIG_LV_USE_SPAN=y`, `CONFIG_LV_SPAN_SNIPPET_STACK_SIZE=64` in the built
  `sdkconfig.h`), because the LVGL Kconfig defaults it to `y` unless
  `LV_CONF_MINIMAL`. **No new component, no new dependency, no image growth.**
* **The spangroup API in 9.5** (`src/widgets/span/lv_span.{h,c}` — there is no
  `lv_spangroup.h` in this version): `lv_spangroup_create()` /
  `lv_spangroup_add_span()` / `lv_span_set_text()` / `lv_span_get_style()` /
  `lv_spangroup_set_overflow()` / `lv_spangroup_set_max_lines()` /
  `lv_spangroup_set_span_style()` / `lv_spangroup_delete_span()` /
  `lv_spangroup_refresh()`.
  * **`lv_spangroup_set_mode()` is deprecated** ("set the width to
    `LV_SIZE_CONTENT` or a fixed value to control expanding/wrapping") — the mode
    is *implied by the object's size*, so there is no mode enum to put on the
    wire.
  * Per-span properties fall back to the group's own style
    (`lv_span_get_style_text_color/font/opa/decor(par, span)` all read the
    spangroup's `LV_PART_MAIN` style when the span's built-in style has no value)
    — so the existing `Style.text_color` on the widget is the *base* colour, and a
    span only overrides what it sets.
  * Layout/size: a fixed width ⇒ break + expand height; the draw loop stops at the
    object's bottom (`txt_pos.y + max_line_h + next_line_h - line_space >
    coords.y2 + 1`) and then honours `overflow` (CLIP / ELLIPSIS) and `max_lines`
    (`< 0` = unlimited, `lv_span.c:601`). That reproduces the label's
    WRAP / DOTS / CLIP behaviour *without* `lv_spangroup_set_mode()`.
  * `lv_span_set_text()` **copies** (mallocs) and does *not* invalidate —
    `lv_spangroup_refresh()` is the explicit "layout is stale" call.
* **Nothing in the firmware inspects the label class.**
  `grep -rn 'lv_label_class|lv_label_get_text|lv_obj_check_type' firmware/main/`
  is empty, so a widget that used to be a label being a spangroup instead is
  safe. Labels never emit action values either (`widget_value_*` covers
  slider/switch only).
* **Host/sim sides:** DSL `label(..., recolor=False)`
  (`app/src/touchy_pad/api/screens.py:763`); sim renders markup as Qt rich text
  (`sim/widgets.py::_recolor_to_html`, regex `#([0-9a-fA-F]{6}) (.*?)#`); the
  sim's proto-level override engine (`sim/device.py::_apply_override_to_widget`)
  honours `text` on button/label only, re-materialising the active screen from
  pristine bytes on every batch, so removals are lossless there.

## 3. The decision

### 3.1 Compat path — `recolor` keeps its API and changes its engine

`Label.recolor = true` means exactly what it means today: `Label.text` is LVGL
markup (`#RRGGBB text#`), and a runtime
`set_properties([build_property_override(id, "text", markup)])` replaces it.
**No host change, no new field, no new call.** What changes is the device:
`build_label()` stops calling `lv_label_set_recolor()`; when `recolor` is set it
builds an **LVGL spangroup** instead of a label, parses the markup into spans
itself (one span per segment, `text_color` set on the coloured ones), and a
runtime `text` override on that widget runs the same parser (§5.4). Text outside
a run inherits the spangroup's `Style.text_color`, i.e. exactly the old
semantics. voicebot ships a firmware update and is fixed.

This is strictly better than the bug report's interim "one recolour command per
word" mitigation: it costs the host nothing, drops the 8-bytes-per-word
overhead, and fixes measurement and centre/right alignment at the same time.

### 3.2 New path — explicit `Span` / `SpanGroup` (recommended)

New proto messages + a new `Widget.kind` for hosts that want real rich text:
per-run colours *and* fonts, `max_lines`, ellipsis, alignment — and, crucially,
**the same single-RPC live update**: a `text` override whose value is markup is
re-parsed into the span list. The "cheap update" story stays *one
`SetPropertiesCmd` per frame* (no `span0_text` / `span0_color` chatter, no
JSON-in-a-string), which is what a chatty host needs.

Documented as the recommended way from then on; `recolor` stays supported as the
compatibility spelling.

### 3.3 What deliberately does **not** change

* **`SCROLL` / `SCROLL_CIRCULAR` labels keep the label path.** A spangroup cannot
  marquee (no long-mode, no scroll animation), and a scrolling label lays its text
  out as **one unwrapped line**, so the line-start / last-line parser resets that
  cause this bug never happen there. Keeping `lv_label_set_recolor()` for those
  two modes preserves the LED marquee (`pages/trackpad.py`, voicebot's LED path,
  `docs/python-api.md` → "Scrolling speed") byte-for-byte, including Stage lb19
  `scroll_duration_ms` / `scroll_speed_px_s` and the `anim_duration` override.
  Documented as the one remaining use of LVGL's markup parser, pointing at this
  plan.
* **`recolor == false` is untouched**: markup still renders literally.
* **No other widget kind changes.** `Button` / `Checkbox` text stays plain.

## 4. Wire format (`proto/widgets.proto` + `proto/widgets.options`)

```proto
// One styled run of text inside a SpanGroup. `text` is LITERAL — the span
// carries its own style, so no in-string markup is parsed (that is the whole
// point: see docs/plans/span-rich-text.md).
message Span {
    string text = 1;
    // 0xRRGGBB. Unset = inherit the group widget's Style.text_color.
    optional uint32 text_color = 2;
    // Pixel size (same names/mapping as Style.font_size: nearest
    // lv_font_montserrat_<N> compiled in). Unset/0 = the group's font.
    optional int32  font_size = 3;
}

// How a SpanGroup that runs out of room behaves.
enum SpanOverflow {
    SPAN_OVERFLOW_CLIP     = 0;  // default (LVGL default)
    SPAN_OVERFLOW_ELLIPSIS = 1;  // "…" on the last line that fits
}

// A rich-text paragraph: N styled runs laid out as one wrapping block.
//
// There is deliberately NO `mode` field: LVGL deprecated
// lv_spangroup_set_mode() in 9.5 (the mode is implied by the object's size),
// so a SIZE_CONTENT width expands and a fixed width wraps. Size comes from
// the shared Widget placement (rect / grid cell / grow_*), exactly as it
// does for a label.
message SpanGroup {
    repeated Span spans = 1;      // FT_POINTER (see widgets.options)
    SpanOverflow overflow = 2;
    // Max lines to draw; 0 or negative = unlimited (LVGL wants < 0 for
    // unlimited, so the firmware translates 0 -> -1).
    int32 max_lines = 3;
    // Horizontal alignment of the text block; AUTO = LVGL default.
    TextAlign text_align = 4;
}
```

In `Widget`:

```proto
        SpanGroup span_group = 34;   // oneof kind — 34 is free (max used is 33)
```

and `Widget.Version.CURRENT` moves **30 → 31**, with a comment line
(`// LB24: SpanGroup; Label.recolor rendered via spans.`).

`proto/widgets.options` gains:

```
touchy.Span.text        max_size:128     # a run, not a paragraph
touchy.SpanGroup.spans  type:FT_POINTER
```

`Span.text` mirrors `Label.text`'s 128-byte cap (a *run* is short; the long
multi-colour string rides the 512-byte `SetPropertyCmd.string_value` markup
override — the live path every chatty host uses). `spans` is `FT_POINTER` like
every other repeated list, so the decoded struct stays a small header.

No `ProtocolVersion` bump: nothing in `touchy.proto` changes and no command is
added — the existing file-write / `SetPropertiesCmd` paths carry the new widget.
The `Widget.Version` bump does mean **old saved screens are rejected and
deleted** (house rule, `widgets.proto:1010`), so hosts must re-save; voicebot
re-uploads every run, so that is free for it.

## 5. Firmware

### 5.1 New module `firmware/main/widgets/widget_span.{h,cpp}`

One place owns "markup → spans" and "proto → spans", so the compat path and the
new path cannot drift:

```cpp
// Parse `#RRGGBB text#` markup into `group`'s span list, replacing whatever
// was there. Text outside a run gets no colour (inherits the group style).
// An aborted/unterminated `#` is kept literally, matching the sim.
void span_group_set_markup(lv_obj_t *group, const char *markup);

// Explicit path: build a SpanGroup widget from the proto.
lv_obj_t *build_span_group(lv_obj_t *parent, const touchy_Widget &w);

// Compat path: the spangroup that implements Label.recolor (markup text,
// text_align, wrap / ellipsis / clip in place of long_mode).
lv_obj_t *build_recolor_span_group(lv_obj_t *parent, const touchy_Widget &w);

// True when `obj` is one of ours, so the override path can route `text`.
bool span_group_apply_text_override(lv_obj_t *obj,
                                    const touchy_SetPropertyCmd &cmd);
```

Implementation notes:

* **Markup parser** — one pass, no regex, no buffer of its own:

  ```c
  seg_start = s; colored = false;
  for (p = s; *p; p++) {
      if (*p != '#') continue;
      if (!colored && is_hex6(p + 1) && p[7] == ' ') {      /* opener */
          flush(seg_start, p, colored); colored = true; p += 7; seg_start = p + 1;
      } else if (colored) {                                  /* closer */
          flush(seg_start, p, colored); colored = false; seg_start = p + 1;
      }
  }
  flush(seg_start, p, colored);
  ```

  `flush()` adds a span only when the segment is non-empty (so `#FF0000 one#`
  yields exactly one coloured span, and an empty run adds nothing); coloured
  segments get `lv_style_set_text_color(lv_span_get_style(span), hex)`. The
  opener rule (six hex digits + a consumed space) and "an unmatched `#` is
  literal" are LVGL's own semantics, so existing `recolor` strings parse
  identically — §9 pins this with a test table.
* **Objects**: `lv_spangroup_create()`, then per span `lv_spangroup_add_span()`
  → `lv_span_set_text()` → mutate `lv_span_get_style(span)` (documented as the
  span's built-in style; avoids `lv_style_t` lifetime puzzles that
  `lv_spangroup_set_span_style()` would introduce). Per-span `font_size` reuses
  `font_for_size()` — currently a file-static in `widget_styles.cpp`, so
  **export it via `widget_styles.h`** (unknown size ⇒ leave the group font, same
  as `Style.font_size` today).
* **Re-parse clears first**: `span_group_set_markup()` must drop the existing
  spans before adding the new ones, or every quip appends a paragraph. Walk the
  list with the public API —
  `while ((s = lv_spangroup_get_child(group, 0))) lv_spangroup_delete_span(group, s);`
  (`lv_spangroup_delete_span()` frees the text + resets the span's style; the
  header's "call `lv_observer_remove()` first" caveat only applies if a span was
  bound with `lv_spangroup_bind_span_text()`, which we never do). Then add the new
  spans and refresh once.
* **Wrapper base**: the base colour / `text_opa` come from the widget's `Style`
  (applied generically by `apply_styles()`), and — like `build_label` — set a
  *local* `bg_opa = LV_OPA_TRANSP` so a spangroup is not an opaque white box.
  Same caveat as `docs/plans/thought-bubbles.md`: a local style outranks host
  styles, so a bubble background still means composing a container + text
  (documented, not solved here).
* **Never call `lv_spangroup_set_mode()`** (deprecated, and it would stomp the
  size `apply_rect()` sets a moment later). Map instead:

  | `Label.long_mode` (compat path) | spangroup settings |
  |---|---|
  | `WRAP` (0) | overflow CLIP, no `max_lines` — height behaves exactly as the label's today (fixed box ⇒ clips; content box ⇒ expands) |
  | `DOTS` (1) | overflow **ELLIPSIS**, no `max_lines` — the draw loop already stops at the box bottom, so a fixed-height box ellipsises like the label |
  | `CLIP` (4) | overflow CLIP |
  | `SCROLL` / `SCROLL_CIRCULAR` | **not mapped** — keep `lv_label` + `lv_label_set_recolor()` (§3.3) |

  `max_lines` is only set when the explicit `SpanGroup.max_lines > 0`
  (translate the proto's `0` to LVGL's `-1`).
* **`overflow` / `max_lines` are set after the spans**, then one
  `lv_spangroup_refresh()` — `lv_span_set_text()` does not invalidate.
* **Guard with `#if LV_USE_SPAN`**: the two builders return `nullptr` when the
  feature is off and the label path takes over, plus add an explicit
  `CONFIG_LV_USE_SPAN=y` to `firmware/sdkconfig.defaults` (it is only a Kconfig
  *default* today) so a future `LV_CONF_MINIMAL`-ish change cannot silently drop
  the fix.

### 5.2 `widget_builders.cpp`

* `widget_build()` gains
  `case touchy_Widget_span_group_tag: return build_span_group(parent, w);`.
* `build_label()` starts with the compat redirect:

  ```cpp
  // Stage lb24 — LVGL's label recolour parser loses its state on a wrapped
  // line (line-start / last-line resets), so a run that straddles a break is
  // mis-measured and drawn twice. Render recolor with a span group instead;
  // the markup string and the wire format are unchanged.
  if (w.kind.label.recolor &&
      w.kind.label.long_mode != touchy_LongMode_LONG_MODE_SCROLL &&
      w.kind.label.long_mode != touchy_LongMode_LONG_MODE_SCROLL_CIRCULAR) {
      if (lv_obj_t *g = build_recolor_span_group(parent, w)) return g;
  }
  ```

  The existing `lv_label_set_recolor()` call stays, now reachable only for the
  two scroll modes, with a comment saying why that is safe (a scroll label lays
  out one unwrapped line) and pointing at this plan.

### 5.3 Marking a spangroup so a `text` override can find it

A spangroup has **no `text` property** (`lv_span_properties[]` exposes only
`align` / `overflow` / `indent` / `mode` / `max_lines`), so
`lv_obj_property_get_id(g, "text")` returns `LV_PROPERTY_ID_INVALID` and the
override path would log "unknown property" and fail — breaking the one API
voicebot depends on. Use the established Stage-lb19 pattern: a small heap
context stored with `lv_obj_set_user_data()`, freed from an `LV_EVENT_DELETE`
callback, holding

* a `magic` tag (so `span_group_apply_text_override()` can recognise our
  spangroups and nothing else), and
* the **last markup string** (so a repeated identical `text` override is a
  no-op — see §10 on alloc churn).

```cpp
struct SpanGroupCtx { uint32_t magic; std::string last_markup; };
```

### 5.4 `widgets/widget_property.cpp` — the live path

`apply_to_obj()` gains one routing call before it resolves the property id:

```cpp
// Stage lb24 — a markup-backed spangroup has no LVGL `text` property; a
// `text` write means "re-parse this markup into the span list".
if (cmd.which_value != 0 && span_group_apply_text_override(obj, cmd)) return true;
```

`span_group_apply_text_override()` returns `false` for any object that is not one
of ours or any property other than a string `text`, so nothing else changes.
Because it returns `true` on success, `set_one_locked()` invalidates the object
and sets `touched_live` — so the **Stage lb16 immediate repaint** (invalidate +
`lv_timer_resume/ready` + `lvgl_port_task_wake`) applies to span updates for
free. It also means the override is stored in the sticky table exactly as today.

Removal (`value` oneof unset) needs **no** new code and behaves exactly as it
does for a label today: `set_one_locked()`'s remove branch just drops the stored
override; the on-screen text is restored on the next rebuild (screen load /
`WidgetRef` swap), not immediately. Worth one sentence in `docs/python-api.md`
so nobody files it as a bug.

### 5.5 Build wiring

* `firmware/main/CMakeLists.txt`: add `"widgets/widget_span.cpp"` to `SRCS`
  (the list is explicit — there is no glob).
* `firmware/sdkconfig.defaults`: `CONFIG_LV_USE_SPAN=y` (defensive, §5.1).
* No new component, no `idf_component.yml` change, no `REQUIRES` change.

## 6. Host library (`app/`)

### 6.1 DSL — `app/src/touchy_pad/api/screens.py`

```python
class SpanOverflow(IntEnum):     # mirrors proto/touchy_pad._proto
    SPAN_OVERFLOW_CLIP     = 0
    SPAN_OVERFLOW_ELLIPSIS = 1

def span(text: str, color: int = 0, font_size: int = 0) -> _proto.Span:
    """One run of text for span_group(). `color=0` = inherit the group colour."""

def span_group(
    id: str,
    spans: Iterable[Span | _proto.Span | str] = (),
    *,
    overflow: int = SpanOverflow.SPAN_OVERFLOW_ELLIPSIS,
    max_lines: int = 0,                 # 0 = unlimited
    text_align: int = TextAlign.AUTO,
    rect=None, style=None, animations=None,
) -> _proto.Widget:
    """Multi-colour / multi-font text in one wrapping block (stage lb24).

    The recommended way to put several colours in one text layout: unlike
    `label(..., recolor=True)` the runs are real styled spans on the device,
    so a coloured run that crosses a line break cannot corrupt the line.
    Live updates use the normal property override:
    `build_property_override(id, "text", "#RRGGBB one# two")`.
    """
```

* A bare `str` in `spans` is accepted as an uncoloured run (sugar).
* `parse_recolor_markup(text) -> list[_proto.Span]` (new, in
  `api/screens.py` or a small `api/rich_text.py`) — the Python mirror of the
  firmware parser, used by (a) hosts that want to turn a markup string into
  explicit spans up front and (b) the simulator (§7). Same rules, same test
  table (§9).
* Validation, matching the existing house style (raise `ValueError` in the DSL):
  colour must be `0 <= c <= 0xFFFFFF`, `font_size >= 0`, `max_lines` any int.
* Re-export the new names from `touchy_pad.api.__init__`
  (`span`, `span_group`, `SpanOverflow`, `parse_recolor_markup`) alongside the
  existing widget helpers.
* `label(...)` is **unchanged** — same signature, same docs, no deprecation
  (its `recolor` doc-comment gains one sentence: on a wrapping long mode the
  device now renders it as a span group, `docs/plans/span-rich-text.md`).

### 6.2 Rust / other consumers

No `rust/` code change and no new Rust helper is in scope: the crate's public
surface is "send proto / set properties", and both work as-is (the regenerated
`touchy.rs` gains the bindings automatically through `just rust-gen-proto`,
which **must be run and committed** — CI fails on drift). A `span_group()`
builder for `touchy-demo` would be nice-to-have, not required. `CHANGELOG.md`
gets an entry (it is user-visible: multi-colour text renders correctly now).

## 7. Simulator (`app/src/touchy_pad/sim/`)

The sim cannot reproduce the LVGL bug (it renders Qt rich text), so its job here
is **fidelity to the new contract**, not regression detection:

* `sim/widgets.py::_build_leaf` gains a `span_group` branch: a `QLabel` with
  `RichText`, `setWordWrap(True)`, the group's `text_align`, per-span
  `<span style="color:#RRGGBB; font-size:Npt">` runs (reusing a new
  `spans_to_html()` next to `_recolor_to_html`), and `max_lines` honoured via
  `QLabel` eliding/`setMaximumHeight` best-effort.
* `_recolor_to_html` is **reimplemented on top of `parse_recolor_markup()`** so
  the sim's label preview and the device's parser share one grammar (today it is
  an independent regex).
* `sim/device.py::_apply_override_to_widget` accepts `text` on a `span_group`
  widget, replacing the widget's spans with `parse_recolor_markup(value)`
  (mirroring §5.4). Because the sim re-materialises the active screen from
  pristine bytes on every batch, removal is *lossless and immediate* there —
  a documented, harmless sim advantage over the device (where removal waits for
  a rebuild).
* `board_config`-style no-ops are not needed; nothing else in the sim changes.

## 8. File & component layout (the whole diff, by file)

| File | Change |
|---|---|
| `proto/widgets.proto` | `Span`, `SpanOverflow`, `SpanGroup`, `Widget.kind.span_group = 34`, `Widget.Version.CURRENT` 30→31 |
| `proto/widgets.options` | `touchy.Span.text max_size:128`, `touchy.SpanGroup.spans type:FT_POINTER` |
| `firmware/main/widgets/widget_span.{h,cpp}` | **new** — parser, both builders, the `text`-override hook, the ctx marker |
| `firmware/main/widgets/widget_styles.{h,cpp}` | export `font_for_size()` |
| `firmware/main/widgets/widget_builders.cpp` | `widget_build` dispatch case; `build_label` compat redirect + comment |
| `firmware/main/widgets/widget_property.cpp` | one routing call in `apply_to_obj()` |
| `firmware/main/CMakeLists.txt` | add `widgets/widget_span.cpp` |
| `firmware/sdkconfig.defaults` | `CONFIG_LV_USE_SPAN=y` |
| `app/src/touchy_pad/api/screens.py` | `SpanOverflow`, `span()`, `span_group()`, `parse_recolor_markup()`, doc-comment tweak on `label()` |
| `app/src/touchy_pad/api/__init__.py` | re-exports |
| `app/src/touchy_pad/sim/widgets.py` | `span_group` renderer, `spans_to_html()`, `_recolor_to_html` on the shared parser |
| `app/src/touchy_pad/sim/device.py` | `text` override on a span group |
| `app/src/touchy_pad/pages/span.py` (**new**) + `pages/__init__.py`, `pages/test.py` | demo/regression page |
| `app/tests/test_screens.py`, `test_span_markup.py` (**new**), `test_sim_recolor.py`, `test_set_properties.py` | see §10 |
| `docs/design.md` | `## Stage lb24: … — DONE` after the lb23 section |
| `docs/hardware/lightbar/design.md` | full `## stage lb24 …` section (house style of lb12/lb13/lb17/lb19) |
| `docs/python-api.md` | LB17 section rewritten ("no longer uses LVGL's parser"), new "Rich text with spans" section, `text`-override note |
| `llms.txt` | widget list (`span_group`, `span`), `Widget.Version` 31, the `#`/recolor gotcha, the signage recipe |
| `AGENTS.md` | Stage lb24 bullet + `Widget.Version.CURRENT == 31` |
| `CHANGELOG.md` | Unreleased entry |
| `docs/open-issues.md` | the upstream LVGL defect + repro + the touchy-pad workaround |
| `.clinerules/plans.md`, `.clinerules/memory-bank/activeContext.md` | list this plan |

## 9. Phased implementation

1. **Proto + bindings.** `Span` / `SpanOverflow` / `SpanGroup`,
   `Widget.kind.span_group`, `Widget.Version.CURRENT` 31, the two
   `widgets.options` lines → `just build-proto`; `just rust-gen-proto` and
   **commit** the regenerated `rust/touchy-pad/src/proto/touchy.rs` (CI
   drift-check). Sanity: `just app-test` still green (nothing consumes it yet).
2. **Firmware.** `widget_span.{h,cpp}`, export `font_for_size()`, the
   `widget_build` dispatch case, the `build_label` compat redirect, the
   `widget_property.cpp` routing call, CMake + sdkconfig → `just firmware-build`
   (default `jc4827w543`).
3. **Host DSL.** `parse_recolor_markup()`, `span()`, `span_group()`,
   `SpanOverflow`, re-exports → `just app-test` / `just app-lint`.
4. **Simulator.** `span_group` renderer, shared parser, `text`-override mirror.
5. **Demo / regression page.** `pages/span.py` + registration: one explicit
   span group (multi-colour, mixed font sizes, `max_lines=2` + ellipsis), one
   `label(recolor=True)` whose coloured run **straddles a wrap** (the voicebot
   regression), one plain label for comparison.
6. **Hardware validation** on `jc4827w543` (§10).
7. **Docs + memory bank.** `docs/design.md`, `docs/hardware/lightbar/design.md`,
   `docs/python-api.md`, `llms.txt`, `AGENTS.md`, `CHANGELOG.md`,
   `docs/open-issues.md`, the two `.clinerules/` files — then hand off for the
   human to commit (no commits from me).

## 10. Testing strategy

**Unit (host, `just app-test`).**

* `app/tests/test_span_markup.py` (**new**) — one table drives both the Python
  mirror and (as a comment/spec) the firmware parser:
  | input | spans (text, colour) |
  |---|---|
  | `hello` | `[("hello", None)]` |
  | `#FF0000 one#` | `[("one", 0xFF0000)]` |
  | `#FF0000 Danger!# watch out` | `[("Danger!", 0xFF0000), (" watch out", None)]` |
  | `#FF0000 one# #00FF00 two#` | `[("one", FF0000), (" ", None), ("two", 00FF00)]` |
  | `#A020F0 cocks head#I bet` (voicebot, closing `#` hugging the next word) | `[("cocks head", A020F0), ("I bet", None)]` |
  | `#nope` / trailing `#` / `#FF0000 no-space#` | literal, one uncoloured span |
  | `#FF0000 #` (empty run) | `[]` or one empty → **no** spans |
* `app/tests/test_screens.py` — `span_group()` round-trip (spans + per-span
  colour/font presence, `overflow`, `max_lines`, `text_align`), bare-`str` span
  sugar, `Widget.Version.CURRENT == 31`, DSL validation errors, and that
  `label(recolor=True)` still writes `Label.recolor` **unchanged** (the LB17
  tests must keep passing untouched — that *is* the no-host-change guarantee).
* `app/tests/test_sim_recolor.py` — extend: the shared parser cases, and a
  `span_group` render that produces the same HTML as the equivalent
  `label(recolor=True)` for a multi-run string.
* `app/tests/test_set_properties.py` — a `text` override onto a `span_group`
  widget replaces the spans (sim); removal restores the pristine spans (sim);
  a `text` override onto a normal label is unaffected.
* Regression: no change to any existing test's expectations for labels,
  scrolling labels, `scroll_speed_px_s`, or the sim's override subset.

**Hardware (no firmware unit tests in this project).** On `jc4827w543`, with the
demo page pushed via `just app-run -- …`:

* **The bug itself**: rect `8,8,464,256`, font 30, `LONG_MODE_DOTS`,
  `text_color=#FFFFFF`, and voicebot's exact string
  (`#FF0000 peeks from behind curtains# Oh, Mike's been "hiding" somewhere,
  avoiding his problems, as usual. #A020F0 cocks head#I bet he's in a cave,
  living off grubs and studying the stars.`). Acceptance: the `head.` line is one
  continuous violet run, no double-drawn glyphs, no line overrun, and the wrap
  point matches the sim. (This is the minimal repro from the bug report — it
  cannot be reproduced in the sim, which does no real glyph layout.)
* **Live path**: push `text` overrides in a loop (~2 Hz) with different strings
  and confirm no ghosting, no growth (watch `board-info` `free_heap_bytes`), and
  that a new frame lands immediately (Stage lb16 behaviour).
* **`max_lines` + ellipsis**: an explicit span group with `max_lines=2` in a
  short box shows exactly two lines with `…`.
* **Regression**: the LED marquee path (`pages/trackpad.py`, and voicebot's LED
  mode) still scrolls and still colours — it is the untouched `lv_label` path.
* **Cross-check DOTS/CLIP**: a `LONG_MODE_DOTS` recolor label in a fixed-height
  box ellipsises (rather than expanding or clipping mid-glyph), and
  `LONG_MODE_CLIP` clips. If either diverges from the old label behaviour, tune
  `overflow` / `max_lines` here — this is the one mapping the plan flags as
  "verify on hardware" (§11).

## 11. Risks / open questions

* **The DOTS/CLIP mapping is the one thing I could not fully verify by reading.**
  LVGL's label DOTS clamps to the *box height* with `LV_TEXT_FLAG_BREAK_ALL` on
  the last line; the spangroup stops at `coords.y2` and honours `overflow`, which
  *should* be equivalent, but the number of lines that fit, the ellipsis
  placement and mixed-font baselines could differ by a pixel or a line. Plan B if
  it diverges: derive `max_lines` from `lv_spangroup_get_expand_height()` / the
  box height and the tallest span font. Must be eyeballed on hardware (§10).
* **Markup is now parsed by us, so its grammar is our contract.** The parser
  deliberately implements LVGL's rules (six hex digits + a consumed space to open,
  an unmatched `#` literal) so *existing* `recolor` strings keep rendering the
  same. The risk is the opposite direction: a host that relied on LVGL's
  zero-width swallowing of a mid-line closing `#` (i.e. on the buggy behaviour)
  sees a *corrected* line — that is the point, but it means a wrapping recolour
  label's line breaks can shift by a word versus `Widget.Version` 30. Called out
  in `CHANGELOG.md`.
* **A literal `#` still cannot be escaped** — LVGL's `##` is dead code in this
  version, so `#` is always a command delimiter and hosts strip it (lb17's
  documented gotcha; `voicebot` already does). *Open question:* since we now own
  the parser we **could** support `##` (or `\#`) as an escape. Deliberately out
  of scope to keep the wire semantics identical to today — but flagged, and if
  adopted the sim/DSL mirror must match, so it would be its own stage.
* **Alloc churn on a chatty host.** Every `text` override now re-allocates the
  spans (`lv_span_set_text` mallocs) instead of writing one string into the
  label's buffer. Mitigations: (a) the ctx stores the last markup string and an
  identical override is a no-op (§5.3), (b) rebuilding only the spans that
  changed is possible later if needed, (c) `free_heap_bytes` is watched during
  the hardware loop (§10). This is the main new runtime cost; it is bounded and
  measurable.
* **No marquee for span groups.** A `SpanGroup` cannot scroll, so a host that
  wants a *coloured ticker* keeps using
  `label(recolor=True, long_mode=SCROLL*)`, which stays on the
  buggy-but-unaffected LVGL path. Documented in `docs/python-api.md` so the
  advice is unambiguous ("`recolor` scrolls, span groups wrap").
* **Upstream is still wrong.** Worth filing against LVGL with this analysis:
  carry the parser state across lines/draw passes in `lv_text_get_next_line()`,
  `lv_draw_label()` and `lv_text_get_width()`, referencing the closed-stale
  #8547 rather than opening blind. Until then touchy-pad simply does not use the
  parser for wrapping labels.
* **Stage-number collision** with `docs/plans/thought-bubbles.md` (both want
  `lb24`) — resolve at implementation time; whichever lands second renumbers.
* **`Span.text` at 128 bytes** is a guess at the right run size. It matches
  `Label.text`; a host that wants a single 400-character coloured run must split
  it or use the markup override (512). Easy to raise later without a
  wire-version bump.

## 12. Alternatives considered

| Option | Why not |
|---|---|
| Keep `lv_label_set_recolor`, fix it in LVGL | We do not vendor LVGL (managed component), and the upstream issue has been open/unmerged since v8. Patching `managed_components/` is not maintainable. |
| Host-side mitigation: one recolour command per word | Works (a wrapped line then always starts at a valid opener) but costs ~8 bytes per coloured word against the 511-byte `text` budget, still leaves measurement and centre/right alignment wrong, and has to be re-implemented in *every* host. Fix it once, device-side, instead. |
| Make rich text a **new** widget kind and deprecate `Label.recolor` | Breaks voicebot and every existing screen for no benefit; the compat path is a strict superset. |
| Do the markup → spans conversion **host-side** | The live path is a raw `text` string over `SetPropertiesCmd` — there is no host in the loop on the device. The parser must live on the device, so it is the single implementation (mirrored in the sim, not duplicated). |
| Indexed span overrides (`span0_text`, `span0_color`, …) for live updates | Chatty (N RPCs per frame) and a worse fit for a one-string-per-frame host. The markup `text` override keeps it one RPC and reuses the parser we already need. |
| Encode the span list as JSON in the override string | Bytes that only version-coupled code can produce, plus its own escaping story inside a string already full of `#` and quotes. |
| A `mode` field on `SpanGroup` | `lv_spangroup_set_mode()` is deprecated in 9.5 — the mode is implied by the object's size, which `Widget.placement` already controls. A wire field would be dead weight that could fight `apply_rect()`. |
| Fix the *sim* only, leave hardware as-is | The sim does no real glyph layout, so it cannot reproduce the defect at all. |
| Wait for the LVGL fix | Unmerged for two major versions; LCD users have no multi-colour wrapping text today. |

## 13. Definition of done

1. voicebot's failing screen renders correctly on `jc4827w543` with **no change
   to voicebot's code** (a firmware update only) — §10's acceptance case.
2. The runtime `text` override still updates a wrapping recolour label in one
   RPC, immediately (Stage lb16), with no heap growth over a sustained loop.
3. `span_group()` is documented in `docs/python-api.md` + `llms.txt` as the
   recommended multi-colour text API, with a worked example; `recolor` is
   documented as the compatible spelling with the scroll-only caveat.
4. `just build-proto`, `just rust-gen-proto` (output committed),
   `just app-test`, `just app-lint`, `just firmware-build` all green.
5. `Widget.Version.CURRENT == 31` stated in `AGENTS.md` + `llms.txt`;
   `docs/design.md` has the `lb24` (or `lb25`) section and
   `docs/hardware/lightbar/design.md` the detailed one; `CHANGELOG.md` has the
   user-facing entry; `docs/open-issues.md` records the upstream LVGL defect.
6. The human commits — never me.
