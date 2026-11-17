# Thought bubbles — chat-style rounded rectangles with one sharp corner

**Status:** proposed. The **design decision in §9 must be made before implementation**
(each route has non-obvious trade-offs and one of them bakes a chat-specific aesthetic
into the firmware).

| | |
|---|---|
| **Stage name if implemented** | `lb24` (last landed stage is lb23) |
| **Sides touched** | `proto/`, `firmware/main/`, `app/` (DSL + sim + docs); `rust/` only via the regenerated prost bindings |
| **Wire impact** | additive fields on `Style` + `LayoutFlex`; **`Widget.Version` 30 → 31**. No `ProtocolVersion` / `PreferencesFile.Version` change |
| **Just recipes that validate it** | `just build-proto`, `just app-test`, `just firmware-build` (+ `just rust-gen-proto`) |

## 1. Goal

voicebot (a consumer of the published `touchy-pad` package) wants to draw **"thought
bubbles"**: a rounded rectangle with three rounded corners and one deliberately *sharp*
corner (bottom-right), with centred text inside — the SMS/chat-bubble look. This plan
answers two questions: (a) can that be expressed with the API that exists today, and
(b) if not, what is the smallest good firmware/API addition that makes it a one-liner.

## 2. What the stack can express today (verified against the code)

* **LVGL has exactly one corner radius.** `LVGL_VERSION 9.5.0`
  (`firmware/managed_components/lvgl__lvgl/lv_version.h`); `lv_style_gen.h` documents
  `LV_STYLE_RADIUS` as "Get radius on **every** corner", and
  `lv_draw_rect_dsc_t.radius` is a single `int32_t` (`src/draw/lv_draw_rect.h`).
  There is no per-corner radius anywhere in LVGL v9, so LVGL's own background drawing
  can never produce "3 rounded + 1 sharp".
* **The wire format exposes only that one knob**: `Style.radius = 2`
  (`proto/widgets.proto:302`) → `lv_style_set_radius` (`widget_styles.cpp:233`).
* **A `Label` cannot paint a background today**: `build_label` forces a *local*
  `bg_opa = LV_OPA_TRANSP` (`widget_builders.cpp:297`), and local styles outrank the
  host's `lv_obj_add_style` styles — which is why `pages/trackpad.py` puts a styled
  `spacer` *behind* its text.
* **A `Button` has a background and centres its text** (`lv_obj_center(lbl)`, the label
  is content-sized → genuinely vertically centred) **but cannot wrap**: the label box is
  content-sized.
* **A `Label` with a fixed rect does wrap** (`long_mode = WRAP`) but LVGL draws the text
  from the **top** of the box (`lv_label.c:861 draw_main`); there is no vertical text
  alignment in LVGL.
* **Children only exist under layout widgets**: `widget_build_children()`
  (`widget_builders.cpp:1189`) switches on `layout_absolute|flex|grid` and reads
  `Layout.children`, so a leaf kind (button/label/spacer) cannot carry children.
* **Absolute placement** (`screen_layout.cpp:76-84`) uses `lv_obj_set_pos` +
  `lv_obj_set_size`; an unspecified axis becomes `lv_pct(100)` — under an absolute parent
  a child cannot be content-sized on just one axis.
* **Flex has no alignment**: `apply_layout` maps only `flow` + `gap`
  (`screen_layout.cpp:158-171`) and never calls `lv_obj_set_flex_align`, so flex children
  are always packed to the start — a `col()` cannot centre anything today.
* **Live text already works**: `set_properties(widget_id, "text", …)` → LVGL's `text`
  property on a label (`docs/python-api.md` → "Overriding widget properties at runtime");
  the simulator implements the documented subset `x/y/w/h/bg_color/text`.
* **The simulator only styles *some* widget kinds.** `spacer` applies host `bg_color`
  + `radius` (`sim/widgets.py:411-434`) and `label` applies `font_size` / `recolor`, but
  **`button` ignores host styles entirely** (`sim/widgets.py:300-303`). Verified by rendering
  the W1 screen offscreen: the button came out as a stock Qt push button (`stylesheet: ''`)
  while its corner-covering spacer was correct (`#2c2c34`, `border-radius: 0px`, at
  `(262, 70, 18, 18)`). Any bubble work has to teach the sim to style buttons/containers,
  or hosts cannot preview a bubble in the sim.
* **Drawing our own shape is a supported LVGL path**: user `LV_EVENT_DRAW_MAIN` callbacks
  run **after** the object's own `lv_obj_draw` (`lv_obj_event.c:381-388`: preprocess
  callbacks → class callback → normal callbacks), and `lv_draw_rect()` can be queued from
  such a callback using `lv_event_get_layer(e)`.

## 3. So — can voicebot do it today?

Not as a single style or widget. Three things work **today, with no firmware change**:

### W1 — rounded `button` + a corner-filling `spacer` (the "elbow" hack)

```python
cont = Layer(layout=absolute())
cont += button("q0", text="Did you try rebooting?",
               rect=rect(x=0, y=0, w=280, h=88),
               style=[style(bg_color=0x2C2C34, radius=18, pad=12, text_color=0xF2F2F2)])
# square off the bottom-right corner: an 18×18 rect of the same colour on top
cont += spacer("q0_c", rect=rect(x=280 - 18, y=88 - 18, w=18, h=18),
               style=style(bg_color=0x2C2C34))
```

Works because `build_spacer` is `lv_obj_remove_style_all()` + the host style, and children
draw in order (z-order = child order), so the square covers the button's rounded corner.
Limits: two extra objects per bubble, coordinate arithmetic the host must redo on every
resize/override, no border support, and the fill must be opaque (a translucent background
would show the seam). The geometry renders in the sim (verified: the covering spacer lands at
`(262, 70, 18, 18)` with no `border-radius`), but the sim does not style *buttons*, so W1 only
*looks* like a bubble on real hardware (or after the §4.4 sim fix).

### W2 — host-rendered bubble image + a `label` on top

Render the whole bubble with PIL (exact per-corner radii, anti-aliasing, tail, gradient,
shadow) → `ImageSource` / `ImageCache` (content-hash dedup + in-place rewrite *is* the
repaint, Stages 85-87) and overlay a normal `label` for the text so `set_properties` can
retarget/restyle it. Maximum pixel control; but text metrics live on the host (device
fonts differ), every colour change costs a re-render + re-upload, and it is the heaviest
option.

### W3 — all-four-corners-rounded button/label (not the ask)

`button(text=…, style=[style(bg_color=…, radius=16, pad=12)])` already gives an SMS-ish
bubble; only the *sharp* corner is missing.

**Interim recommendation for voicebot:** W1 now (cheap, uses only hardware-proven
primitives), W2 if the bubble has to be decorative.

## 4. Recommended design — Route A: per-corner radius as a general `Style` primitive

The idea: give the firmware the primitive LVGL is missing (a rectangle whose four corner
radii are independent), and let the **host** compose the bubble out of widgets it already
has. Nothing about text, recolour, scrolling, animations or live property overrides
changes — the bubble's text stays an ordinary `label`.

### 4.1 Proto (`proto/widgets.proto`)

* `Style` gains four additive fields:

  ```proto
  optional int32 radius_tl = 22;   // overrides Style.radius for the top-left
  optional int32 radius_tr = 23;
  optional int32 radius_br = 24;
  optional int32 radius_bl = 25;
  ```

  Unset = follow `Style.radius` (and the theme). Putting them on `Style` rather than on a
  new widget kind keeps them a *graphics* primitive (cards, ribbons, tooltips, asymmetric
  panels — not just chat), and keeps live text on the widgets hosts already drive.
* `LayoutFlex` gains an align (mirrors `lv_flex_align_t`):

  ```proto
  enum Align { ALIGN_START = 0; ALIGN_CENTER = 1; ALIGN_END = 2;
               ALIGN_SPACE_BETWEEN = 3; ALIGN_SPACE_AROUND = 4; ALIGN_SPACE_EVENLY = 5; }
  Align main_align  = 4;   // → lv_obj_set_flex_align(cont, main_align, cross_align, START)
  Align cross_align = 5;
  ```

  This is what lets a bubble centre its text **vertically** (COLUMN main axis) without
  inventing new text semantics, and it is generally useful (centred button rows, cards).
* `Widget.Version.CURRENT` 30 → 31 (the *look* changes, and the project bumps on
  wire-visible features even when older firmware simply ignores the new fields).

### 4.2 Firmware

* **New `firmware/main/widgets/widget_shape.{h,cpp}`** with one entry point:

  ```cpp
  // Draw `coords` with independent corner radii. `radii` = {tl, tr, br, bl};
  // `src` supplies colours/opacity/border/shadow resolved from the object's styles.
  void widget_shape_draw_rect(lv_layer_t *layer, const lv_area_t *coords,
                              const int32_t radii[4], const lv_draw_rect_dsc_t *src);
  ```

  Implementation: draw the silhouette as the **union of 2 bands + up to 4 corner discs**
  (plain `lv_draw_rect` tasks): an h-band `[x1..x2] × [y1+max(r_tl,r_tr) .. y2−max(r_bl,r_br)]`,
  a v-band `[x1+max(r_tl,r_bl) .. x2−max(r_tr,r_br)] × [y1..y2]`, and one
  `LV_RADIUS_CIRCLE` square of side `2r` per corner with `r > 0`. With uniform radii this is
  exactly the shape LVGL draws; with `r_br = 0` the bottom-right corner comes out square.
  A **2-task fast path** (uniform-radius rect + a square overpaint of the sharp corner)
  covers the "exactly one sharp corner" chat case and is only valid for
  `bg_opa == LV_OPA_COVER`; use it when it applies, the general union otherwise.
* **`widget_styles.cpp`**: a new `apply_corner_radius(obj, w)` called from `apply_styles`.
  When any corner field is present and differs from the resolved `radius`, it stores an
  `int32_t radii[4]` side-car (heap, freed from `LV_EVENT_DELETE`, like `WidgetStyles`) and
  attaches an `LV_EVENT_DRAW_MAIN` handler that draws the silhouette from **resolved** style
  values (`lv_obj_get_style_bg_color/bg_opa/border_*/shadow_*`) — i.e. pressed-state colours
  and transitions keep working. Supported kinds (documented): `spacer`, `button`, `label`,
  layout containers, and `image_button` while no image slot paints a background; other kinds
  ignore the fields with a one-shot `ESP_LOGW`.
* **`widget_builders.cpp`**: two local-style fixes, because *local* styles beat host styles:
  * `build_layout` (lines 1011-1016) forces local `bg_opa`/`border_width`/`pad_all`, so a
    host `style(pad=12)` on a container silently loses. Switch it to
    `lv_obj_remove_style_all(cont)` — the `build_spacer` precedent at line 969 — so the host
    styles are the only styling (identical default appearance, host styles finally win).
  * `build_label` (line 297) always forces `bg_opa = TRANSP`; only do that when no style in
    `w.styles` declares a `bg_color`, so a styled label can show its bubble.
* Borders/shadows on a per-corner shape: draw the silhouette twice when `border_w > 0`
  (border colour inflated by `border_w`, then the fill inset by it) and keep the native
  shadow pass (uniform radius) — a uniformly rounded shadow under a sharp corner reads fine.


### 4.3 Host DSL (`app/src/touchy_pad/api/screens.py`)

* `style()` gains `radius_tl=`, `radius_tr=`, `radius_br=`, `radius_bl=`.
* `flex()` / `col()` / `row()` gain `main_align=` / `cross_align=` (`LayoutFlex.Align`).
* New helper:

  ```python
  def bubble(id, text="", rect=None, *, sharp="br", radius=16,
             bg_color=None, text_color=None, font_size=0, pad=12,
             text_align=TextAlign.CENTER, long_mode=LongMode.LONG_MODE_WRAP):
      """Container with a 3-rounded-1-sharp background and centred text."""
  ```

  It returns a `col(main_align=CENTER)` container carrying the bubble rect + the shaped,
  padded style, containing one wrapped `label` child with id `f"{id}:text"` and
  `grow_x=1` (cross-axis fill → wrap, main-axis CENTER → vertically centred). Expose
  `bubble_text_id(bubble_id)` (and document the container id) so hosts can drive either with
  `set_properties`, and note `recolor=True` + `#RRGGBB …#` runs for multi-colour chat text
  (what voicebot already streams today).

### 4.4 Simulator (`app/src/touchy_pad/sim/widgets.py`)

* teach the style→CSS path to style **buttons and layout containers** too — today only
  `spacer` honours `bg_color`/`radius` (`sim/widgets.py:411`), so W1/a bubble currently
  previews as a stock Qt push button (verified). Apply `bg_color`, `radius`, `pad`
  (contents margins), `text_color` and the per-corner radii;
* wherever that helper emits `border-radius`, also emit per-corner radii
  (`border-top-left-radius: …; border-bottom-right-radius: 0; …`) — Qt stylesheets support
  them for every widget type, so this mirrors the firmware for spacers, buttons, labels and
  containers alike;
* `layout_flex` maps the new align fields onto `QBoxLayout` (`setAlignment` /
  `addWidget(w, alignment=…)`);
* the `bubble()` container must apply its style `pad` as Qt contents margins so sim geometry
  matches the device.

### 4.5 Rust

`just rust-gen-proto` regenerates the checked-in prost bindings
(`rust/touchy-pad/src/proto/touchy.rs`); nothing else is required (Rust hosts build raw
`Style`s). Optionally add a `layout::corner_radius(...)` helper.

## 5. Alternatives considered

### Route B — a new `Bubble` widget kind (shape **and** its own text)

`Widget.kind.bubble = Bubble { string text; Corner sharp_corner; repeated Action on_click; }`,
with the firmware drawing the silhouette and centring an inner label.

* **Pros:** one widget, one id, one style list; `bubble(text=…, on_click=…)` mirrors `Button`
  exactly; the sim is a single `QLabel` with a stylesheet; a tail/triangle or an avatar could
  be added later without touching other widgets.
* **Cons:** the text becomes firmware-owned, so voicebot's fast live updates
  (`set_properties("text")`) stop working — LVGL's `text` property does not exist on a plain
  `lv_obj`, so the firmware would need a **redirect** special case in `widget_property.cpp`
  (and the sim would have to mirror it), and `recolor` / `long_mode` / scrolling would have to
  be re-plumbed as `Bubble` fields. It also bakes one specific aesthetic (chat tail) into the
  firmware, against the "device is a dumb renderer; each kind carries only what it must"
  convention.

### Route C — make `Label` itself the bubble

Add the sharp corner to `Label`, honour a host `bg_color` (drop the forced transparency), and
add vertical text centring (measure the wrapped text and set `pad_top`, recomputed on text/size
change, LB19-style).

* **Pros:** one widget with host-addressable text, both-axis centring, wrapping, recolour and
  scrolling for free.
* **Cons:** three separate changes to the *text* widget for what is really a *shape* feature;
  vertical centring is bespoke (LVGL has no vertical align) and needs text measuring on every
  text/size change (extra CPU + a new failure mode); and none of it helps non-label widgets.

## 6. Phased implementation (Route A)

1. **Proto + bindings** — `Style` corner fields, `LayoutFlex` align, `Widget.Version` 31 →
   `just build-proto`; `just rust-gen-proto` and commit the regenerated prost file.
2. **Firmware** — `widget_shape.{h,cpp}`, `apply_corner_radius` in `widget_styles.cpp`, the
   two local-style fixes in `widget_builders.cpp` → `just firmware-build` (default
   `jc4827w543`).
3. **DSL** — `style()`, `flex()`, `bubble()`, `bubble_text_id()` + `app/tests/test_screens.py`
   cases → `just app-test`.
4. **Simulator** — per-corner CSS, flex align, container padding + a sim test.
5. **Demo/validation page** — `app/src/touchy_pad/pages/bubble.py` (a few bubbles: one plain,
   one with `recolor` markup, one with a border, one translucent) plus an entry in
   `pages/test.py`.
6. **Docs** — `docs/design.md` **Stage lb24**, `AGENTS.md` highlight + `Widget.Version` 31,
   `docs/python-api.md` ("Bubbles / per-corner radii" section next to the existing
   "Overriding widget properties" one), `llms.txt` version numbers + a snippet,
   `CHANGELOG.md` entry.

## 7. Testing strategy

* `app/tests/`: DSL field population (`style(radius_br=0)`), `bubble()` tree shape / ids /
  rects / wrap flags, `flex(main_align=…)`; a sim stylesheet assertion for a
  3-rounded-1-sharp bubble; a `--sim-headless` check that the label wraps inside the bubble.
* Firmware has no unit tests → validate on `jc4827w543`: push the demo page
  (`just app-run -- …`), eyeball opaque fills for seams, confirm a translucent fill + sharp
  corner degrades gracefully, and compare against `just app-run -- --sim-gui`.
* Regression: existing screens that rely on container `pad_all=0` / transparent background
  and on the label's forced transparency (the trackpad page, the default chrome) must render
  identically.

## 8. Risks / open questions

* **Translucency seams.** The union of primitives double-blends where they overlap when
  `bg_opa < COVER`. Mitigation: the 2-task fast path for the common one-sharp-corner shape
  and a documented "use opaque fills" note (decide whether to `ESP_LOGW` once).
* **Borders** on a per-corner shape need the double-draw; a single `lv_draw_rect` with
  `border_width` would follow the *uniform* radius and look wrong at the sharp corner.
* **Widget-specific drawing** is out of scope: sliders, arcs, toggles, trackpads ignore the
  corner fields (documented) — only "rect-like" widgets get the treatment.
* `lv_obj_remove_style_all` on layout containers also drops the theme's scrollbar styling
  (cosmetic; worth a note, `LV_OBJ_FLAG_SCROLLABLE` behaviour is unchanged).
* **Tappable bubbles are not covered by Route A** — layout containers carry no `Action`s. If
  voicebot needs tap-to-speak bubbles, that favours Route B (or a transparent `button`
  overlay, which is a hack).
* **Decision needed (§9):** single widget (Route B/C) vs. composed container (Route A)?

## 9. Decision needed

* **(a) Route A — recommended:** per-corner radii on `Style` + `LayoutFlex` alignment, with a
  host-side `bubble()` helper composing a shaped container and an ordinary `label`.
* **(b) Route B:** a `Bubble` widget kind (shape + text + actions), including a `text`
  override redirect for live updates.
* **(c) Route C:** `Label` becomes the bubble (sharp corner + background + vertical centring).
* **(d) No firmware work:** document the W1/W2 recipes in `docs/python-api.md` and leave it
  there.

