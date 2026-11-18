# Plan: supported font sizes — a published set + a runtime font-size override

**Status: DONE — implemented as Stage lb26** (`docs/design.md`). All three open
questions in §8 were resolved as recommended: **Route A** (a `FontSize` proto
enum), **strict** DSL validation, and a `ProtocolVersion` **14 → 15** bump. The
file is kept as the design record; the on-hardware check of a live `text_font`
change is the only outstanding item (§7).

**Stage: `lb26`** — `lb25` is reserved by the still-open
`docs/plans/thought-bubbles.md`; if that plan is dropped or renumbered, take the
lower number.

Sides touched: `proto/` (one enum + one value arm), `firmware/main/` (the mapping
already exists — one special case + one WARN), `app/src/touchy_pad/` (DSL, props
mapper, sim, CLI docs), `app/tests/`, `rust/touchy-pad/`, `docs/`, `bin/`.

## 1. Goal / summary

Two gaps, one theme: **font size is an untyped integer everywhere, so consumers
can neither discover the valid values nor change them at runtime.**

1. **Nobody can tell a consumer which sizes exist.** `Style.font_size`
   (`proto/widgets.proto`, tag 21) and `Span.font_size` (tag 3) are bare
   `int32`s, and the DSL takes a bare `int` (`label(..., font_size=)`,
   `span(..., font_size=)`, `style(font_size=)`). The firmware compiles exactly
   four fonts — `LV_FONT_UNSCII_8`, `LV_FONT_MONTSERRAT_14/16/30`
   (`firmware/sdkconfig.defaults`; no board enables more) — and
   `font_for_size()` (`firmware/main/widgets/widget_styles.cpp:151`) matches them
   **exactly**, returning `nullptr` for anything else, which every caller treats
   as "leave the theme font in place". So
   `label("t", text="Hi", font_size=24)` uploads and renders *at the wrong size,
   with no error* — in the CLI, in the sim and on the device alike. (The repo
   ships exactly that line in `app/examples/demo.py:41`, and asserts it
   round-trips in `app/tests/test_screens.py:90`.) A Python or Rust consumer has
   no way to discover `[8, 14, 16, 30]`.
2. **No way to change a font size at runtime.** The lb12/lb14 property-override
   path (`SetPropertyCmd` / `SetPropertiesCmd`) reaches LVGL style properties
   *by name* — that is how lb19 retimes a marquee from the host
   (`build_property_override("ticker", "anim_duration", 4000)`). `text_font`
   resolves through the same name table (`"text_font" →
   LV_PROPERTY_STYLE_TEXT_FONT`, LVGL
   `src/widgets/property/lv_style_properties.c:125`) but its value is a
   `const lv_font_t *`: `lv_obj_set_property()` would pass the union straight to
   `lv_obj_set_local_style_prop()` (`src/core/lv_obj_property.c:119-122`, where
   `style`/`ptr` overlap `num`) — so a host sending `property_name="text_font"` +
   `int_value=30` today hands the device `(const lv_font_t *)30`, i.e.
   **undefined behaviour and a draw-time crash**. A typed "font size" value arm
   removes the footgun *and* is what makes a live font change expressible at all.

Deliverable: (a) one canonical, cross-language list of supported pixel sizes,
discoverable from Python and Rust and validated at authoring time with an
actionable error; (b) a pixel-size value arm on `SetPropertyCmd` the firmware
converts with the existing `font_for_size()` → `lv_obj_set_style_text_font()`,
plus host helpers, a sim mirror, a JSON key, CLI docs and tests.

Non-goals (see §8.5–8.7): per-run fonts inside `#RRGGBB …#` markup; custom /
converted fonts (`lv_font_conv`); LED-matrix-specific glyph sets; a
family/weight/range font API.

## 2. The API in one picture

```python
from touchy_pad.api import FONT_SIZES, FontSize, build_property_override, label, touchy_open

FONT_SIZES                       # (8, 14, 16, 30) — derived from the proto enum
label("title", text="Hello", font_size=FontSize.FONT_SIZE_30)   # static, validated

with touchy_open() as pad:                                      # live, one RPC
    pad.set_properties([
        build_property_override("title", "text_font", FontSize.FONT_SIZE_8),
    ])
```

```rust
// Phase 3 — rust/touchy-pad
use touchy_pad::{FontSize, FONT_SIZES};
assert_eq!(FONT_SIZES, &[8, 14, 16, 30]);
pad.set_font_size("title", FontSize::FontSize30).await?;
```

```bash
# no new CLI flag needed: build_property_override maps "text_font" + int itself
touchy property set title text_font 30 --int
```

## 3. Current state (what already exists — do not re-invent)

* **The mapping exists and is shared.** `font_for_size(int32_t)`
  (`firmware/main/widgets/widget_styles.cpp:151-230`, declared in
  `widget_styles.h:40-45`) maps a pixel size to a compiled `lv_font_t*`,
  `nullptr` when there is none. `apply_styles()` uses it for `Style.font_size`
  and `widget_span.cpp:150` uses it for a per-span `Span.font_size` — so a new
  caller should use it too rather than duplicating a switch.
* **The compiled set is exactly `{8, 14, 16, 30}`.** `firmware/sdkconfig.defaults`
  (`LV_FONT_UNSCII_8=y`, `LV_FONT_MONTSERRAT_14/16/30=y`); no
  `firmware/boards/*/sdkconfig.defaults` adds or removes a font.
  Note `8` is **UNSCII 8**, not Montserrat 8 — the enum should be named by pixel
  size so that detail stays internal.
* **What "the theme default" actually is: Montserrat 14.** LVGL's Kconfig choice
  `LV_FONT_DEFAULT` defaults to `LV_FONT_DEFAULT_MONTSERRAT_14` (non-minimal
  config) and that is what the generated board configs select
  (`firmware/sdkconfig.jc4827w543:4448`), so `LV_FONT_DEFAULT ==
  &lv_font_montserrat_14`. Consequence worth writing down: today
  `font_size=24` does not render *big*, it renders at **14 px** — the same as
  passing nothing — which is exactly the silent-wrong-answer this plan removes.
* **Two latent bugs in that switch** (worth fixing while we are here):
  `case 8:` appears twice — once under `#if LV_FONT_UNSCII_8` and again under
  `#if LV_FONT_MONTSERRAT_8` — so a build that enables both fails to compile;
  and the `#error` text says "enable LV_FONT_MONTSERRAT_8" where the guard it
  belongs to is `LV_FONT_UNSCII_8`. The header comment also claims "nearest
  Montserrat font" while the code is exact-match (`screens.py:289-292` repeats
  the same "nearest" claim). Phase 0 fixes the wording and the guard.
* **The runtime override path is in place.** `widget_property.cpp::apply_to_obj()`
  (`:89-152`) resolves an ident (name → `lv_obj_property_get_id()`, which falls
  back to `lv_style_property_get_id()` — LVGL `lv_obj_property.c:232`) and builds
  an `lv_property_t` from the value arm. Overrides are session-scoped and sticky;
  lb16 kicks the repaint when a live widget changed; lb15 counts the batch as
  display activity. **No firmware-side plumbing is missing except the font arm.**
* **Host mapping.** `build_property_override(widget_id, prop, value)`
  (`app/src/touchy_pad/api/props.py:50-102`) maps `bool`/`int`/`str`/`Color`/
  `Point`/`None` → arms; `TouchyClient.set_properties([...])` sends one batch;
  CLI `touchy property set … [--int]` (`cli.py:975-1017`) builds through the same
  helper. Rust has **no** override helper at all (`rust/touchy-pad/src/lib.rs:63`:
  *"Not here yet: live property overrides (text/colour/geometry) … have no Rust
  helper"*).
* **Sim mirror.** `sim/device.py::_cmd_set_properties` (`:407-427`) keeps a
  sticky `(widget_id, ident)` table and `_apply_override_to_widget` (`:112-166`)
  implements the documented subset (x/y/w/h, bg_color, text). Re-materialising
  from pristine bytes + overrides means removal is lossless for free.
  `sim/widgets.py` already reads the **last** `Style.font_size` for a label
  (`:320-329`) and a span group (`:353-360`) — a font override that appends a
  `Style` composes with that and needs no new rendering code.
* **JSON endpoint.** `net/json.cpp:45-81` parses `setProperties.props[].value`
  by canonical key (`boolValue` / `intValue` / `stringValue` / `colorValue` /
  `pointValue`) and treats an unrecognised key as an error, so a new arm needs a
  new key there (`bin/set-property.sh` is the curl example).
* **Enum plumbing is routine here.** `AnimPath`, `LvState`, `StyleProp`,
  `TextAlign`, `LongMode`, `SpanOverflow` are all `widgets.proto` enums
  re-exported from `touchy_pad.api` as `X = _proto.X` (`screens.py:123-129`,
  listed in `__all__`), reached in Rust as prost types and in C as
  `touchy_<Enum>`. A `FontSize` enum should follow exactly that pattern.
  nanopb does not range-validate enum fields (this version's `pb_decode.c` has no
  enum check), so an enum-typed arm cannot be *rejected* by an older or newer
  decoder — worth knowing for §4.3.

## 4. Design

### 4.1 Half 1 — where the canonical list lives

**Route A (recommended): a `FontSize` enum in `proto/widgets.proto`**, beside
`TextAlign` / `LongMode` / `SpanOverflow`:

```proto
// The pixel font sizes this firmware compiles a font for, i.e. the values
// `Style.font_size` / `Span.font_size` render exactly (see CONFIG_LV_FONT_*
// in firmware/sdkconfig.defaults; 8 is LVGL's UNSCII 8, the rest Montserrat).
// Any other number is a request the device cannot honour — it falls back to
// the theme font — so hosts should validate against this list
// (`touchy_pad.api.FONT_SIZES`, `touchy_pad::FONT_SIZES`).
enum FontSize {
    FONT_SIZE_DEFAULT = 0;   // theme default (Montserrat 14, see below)
    FONT_SIZE_8  = 8;
    FONT_SIZE_14 = 14;
    FONT_SIZE_16 = 16;
    FONT_SIZE_30 = 30;
}
```

Why an enum rather than four hand-written constants per language:

* **One definition, three languages.** Python gets `_proto.FontSize`, Rust gets
  the prost type from the checked-in bindings, C gets `touchy_FontSize` — no
  drift, no hand-mirrored tuple, no parity test to babysit.
* **It is the shape this project already uses** for every "valid choices"
  surface (`TextAlign`, `LongMode`, `AnimPath`, `SpanOverflow`, `LvState`,
  `StyleProp`), including the `X = _proto.X` re-export + `__all__` convention in
  `screens.py:107-129`.
* **The numeric values are the pixel sizes**, so the existing `int32`
  `Style.font_size` / `Span.font_size` fields keep working unchanged
  (`FontSize.FONT_SIZE_30 == 30`) — **no `Widget.Version` bump**.
* Python consumers still need an *iterable* — the enum's descriptor gives it, so
  `FONT_SIZES = tuple(v for v in FontSize.values() if v)` is derived, not
  maintained by hand.

Host surface:

* Python (`app/src/touchy_pad/api/screens.py`): `FontSize = _proto.FontSize`
  next to the other aliases; `FONT_SIZES: tuple[int, ...]` and a
  `_check_font_size(value, *, what)` helper used by `label()`, `span()` and
  `style()`; both names in `__all__` and re-exported from `touchy_pad.api`
  (`api/__init__.py` lists them under the LB19 precedent).
  `screens.py:54-59` (the `SCROLL_SPEED_*` block) is the natural home for
  `FONT_SIZES` and its comment.
* Rust: re-export the generated enum at the crate root (`pub use proto::FontSize;`)
  plus `pub const FONT_SIZES: &[u32]` in `lib.rs`, documented as "the pixel sizes
  `touchy_pad::FontSize` names" — matching the crate's `#![warn(missing_docs)]`
  requirement and its habit of keeping shared constants in `lib.rs`.
* Validation rule (same everywhere): `None` / `0` (theme default) or a member of
  the set. Anything else raises `ValueError` naming the offending value, listing
  `FONT_SIZES`, and pointing at `FontSize` — replacing today's "renders at the
  theme size, silently".

Routes considered (and why not):

* **Route B — hand-written `FONT_SIZES` constants in Python + Rust + a firmware
  `#define`,** validated by a parity test (the
  `test_label_scroll_speed_bounds_match_firmware` precedent,
  `app/tests/test_screens.py:1230`). Zero proto change, but three copies to keep
  in sync, no names for C/JSON clients, and nothing generated for the docs to
  link to. Cheaper today, worse in six months.
* **Route C — device-reported `repeated uint32 font_sizes = 16;` on
  `SysBoardInfoResponse`:** the board is the only true authority (it is the thing
  that compiled the fonts). Costs a `ProtocolVersion` bump, a `fill_board_info()`
  addition and a sim mirror, and gives a *dynamic* answer where a static one is
  enough today. Recommended as a **follow-up only** if a board ever ships a
  different set (§8.4) — the enum plus the `font_for_size()` WARN covers the
  practical failure mode.

### 4.2 Half 2 — the runtime override

**Wire** (`proto/touchy.proto`, `SetPropertyCmd.value`):

```proto
oneof value {
    bool   bool_value      = 4;
    int32  int_value       = 5;
    string string_value    = 6;
    uint32 color_value     = 7;   // 0xRRGGBB
    Point  point_value     = 8;
    // The widget's text font, expressed as a PIXEL SIZE instead of a font
    // pointer (the wire cannot carry a pointer). The device resolves it with
    // the same mapping `Style.font_size` uses and applies
    // lv_obj_set_style_text_font(). Use it with property_name = "text_font"
    // (or property_id = LV_PROPERTY_STYLE_TEXT_FONT). FONT_SIZE_DEFAULT
    // removes the local font override so the widget inherits its style stack /
    // the theme again.
    FontSize font_size_value = 9;
}
```

An enum-typed arm (rather than `uint32`) is safe: proto3 enums are open, nanopb
does not range-validate them (§3), and Python allows an unknown int to be
assigned — so a *newer* host sending a future size is a device-side
`INVALID_ARG`, never a decode failure.

**Firmware** (`firmware/main/widgets/widget_property.cpp::apply_to_obj()`), a
branch placed *before* the generic resolve/set path:

1. If `which_value == font_size_value_tag`: resolve the ident and require it to
   be the text-font style property (`lv_obj_property_get_id(obj, "text_font")`
   or `LV_PROPERTY_STYLE_TEXT_FONT`). A font arm aimed at any other property →
   `ESP_LOGW` + `false` (`INVALID_ARG`); **never** fall through to
   `lv_obj_set_property()`, which is the pointer-reinterpretation crash.
2. `FONT_SIZE_DEFAULT` (0) → `lv_obj_remove_local_style_prop(obj,
   LV_STYLE_TEXT_FONT, LV_PART_MAIN)`; the widget goes back to its style stack —
   the runtime mirror of `style(font_size=...)` being unset.
3. Otherwise `const lv_font_t *f = font_for_size(px)`; `nullptr` → `ESP_LOGW`
   (naming the size and the compiled set) + `false`; else
   `lv_obj_set_style_text_font(obj, f, LV_PART_MAIN | LV_STATE_DEFAULT)` and
   return `true`.
4. Returning `true` on a live widget is what makes lb16's repaint kick and lb15's
   backlight wake happen — a font change is as immediate as a `text` change.

Stickiness comes for free: the override table keys on `(widget_id, ident)` and
`apply_to_obj()` runs from `widget_property_register()` on every rebuild, so a
rebuilt widget gets `remove-local` or `set_text_font` re-applied. Reject the
reverse footgun too: `text_font`'s real value is a `const lv_font_t *`, so
*every* other value arm aimed at it (int / colour / string / bool / point) is
rejected with a hint to use `font_size_value` instead of being handed to LVGL
as a pointer.

**Host Python** (`app/src/touchy_pad/api/props.py`): one rule in the value→arm
cascade, *before* the `int` arm — *if the ident is the text-font property name
(`"text_font"`) and the value is an `int` (or `FontSize`, which **is** an int),
emit `font_size_value`*. The property name is already unambiguous, which is
exactly the condition under which this repo **does not** require a wrapper type:
`Color`/`Point` exist because a bare `int` cannot be told from a coordinate;
`30` for `text_font` can only be a size. A raw numeric `property_id` cannot be
classified, so the documented rule is "use the name"; any *non-int* value for
`text_font` (`str` / `Color` / `Point` / `bool`) raises `TypeError` in the
mapper rather than being sent as something the device would have to read as a
font pointer.

Consequences: **the CLI needs no new flag** (`touchy property set title text_font
30 --int` goes through `build_property_override`), and the docs value table in
`docs/python-api.md` gains one row. (Implemented: the CLI also builds the entry
*before* opening the connection, so `build_property_override`'s validation
surfaces as a `click.BadParameter` message rather than a traceback.)

**Sim** (`sim/device.py::_apply_override_to_widget`): `ident == "text_font"` +
`value_arm == "font_size_value"` → replace/append a `Style(font_size=px)` on the
widget (`FONT_SIZE_DEFAULT` → drop the font style). The last-style-wins reads in
`sim/widgets.py:320-329` / `:353-360` then render it and the sticky/removal
machinery is untouched. Leave px→`setPointSize` as-is (advisory, documented);
`setPixelSize` would be a separate fidelity fix.

**JSON** (`net/json.cpp:45-81`): parse the canonical key `fontSizeValue` (number)
into the new arm for `setProperties` entries; add a `text_font` example to
`bin/set-property.sh`'s header (or a sibling `bin/set-font-size.sh`).

### 4.3 Wire version

* `Style.font_size` / `Span.font_size` stay `int32` → **no `Widget.Version`
  bump** (the enum is naming + documentation, not a field-type change).
* One new value arm on `SetPropertyCmd` → **recommended: bump
  `SysBoardInfoResponse.ProtocolVersion` 14 → 15** with a `V15` comment in
  `touchy.proto`, so a host can detect *"this firmware can set font sizes at
  runtime"*. `MIN_FIRMWARE_VERSION` stays 13.
  The alternative (no bump, "additive arm") is defensible under the Stage-86
  precedent, but leaves a silent trap: proto3 ignores an unknown tag, so an older
  device receiving a `font_size_value` entry decodes `which_value == 0` — i.e.
  **"remove the override"** — and answers `OK`. The host believes it set a font
  it never set. A version gate is worth one enum line.

## 5. Files & components

| File / component | Change |
|---|---|
| `proto/widgets.proto` | new `FontSize` enum; doc-comment fix on `Style.font_size` / `Span.font_size` ("exact match, else theme default"). |
| `proto/touchy.proto` | `SetPropertyCmd.value` gains `FontSize font_size_value = 9`; `ProtocolVersion.CURRENT` 14→15 + a `V15` line. |
| `firmware/main/widgets/widget_styles.{h,cpp}` | `font_for_size()`: fix the duplicate `case 8:` (use `#elif`/one guard) + the `#error` text, correct the "nearest" comment, add an `ESP_LOGW` naming the size + the compiled set when nothing matches. |
| `firmware/main/widgets/widget_property.cpp` | new arm in `apply_to_obj()` (font branch, `remove-local` for 0, reject non-font idents and `text_font`+`int_value`). |
| `firmware/main/net/json.cpp` | parse `fontSizeValue`. |
| `firmware/main/proto/*.pb.{c,h}` | regenerated (generated artefacts — do not hand-edit). |
| `app/src/touchy_pad/api/screens.py` | `FontSize = _proto.FontSize`, derived `FONT_SIZES`, `_check_font_size()`, `__all__`; validation in `label()` / `span()` / `style()`; docstring wording. |
| `app/src/touchy_pad/api/__init__.py` | re-export `FontSize`, `FONT_SIZES`. |
| `app/src/touchy_pad/api/props.py` | `text_font` + int → `font_size_value`; docstring/table update. |
| `app/src/touchy_pad/cli.py` | docs only (`property set … --int`; no code change expected). |
| `app/src/touchy_pad/sim/device.py` | mirror the font arm in `_apply_override_to_widget`. |
| `app/src/touchy_pad/_proto/*_pb2.py` | regenerated. |
| `app/examples/demo.py` | `font_size=24` → a supported size (24 is not valid; this is the point of the plan). |
| `rust/touchy-pad/src/lib.rs` (+ `pad.rs`) | `pub use proto::FontSize`, `pub const FONT_SIZES`, `Touchy::set_font_size()` (Phase 3); update the "Not here yet" doc bullet. |
| `rust/touchy-pad/src/proto/touchy.rs` | regenerated + committed (`just rust-gen-proto`; CI drift-check). |
| `docs/python-api.md` | new "Changing a font size at runtime" subsection under *Overriding widget properties at runtime*; value-table row; `FONT_SIZES` mention in *Authoring screens*. |
| `docs/rust-api.md`, `llms.txt` | constants/enum rows. |
| `docs/design.md`, `AGENTS.md`, `CHANGELOG.md` | new stage section + highlights + release note (on landing). |
| `bin/set-property.sh` | `text_font` example (JSON, `fontSizeValue`). |

## 6. Implementation phases

**Phase 0 — wording + latent bugs (no wire change).** Fix the `font_for_size()`
duplicate `case 8:` / `#error` text / "nearest" comment; add the unmatched-size
`ESP_LOGW`; add the `FontSize` enum to `widgets.proto`. Regenerate and confirm
nothing else moved: `just build-proto`, `just rust-gen-proto`,
`just firmware-build`.

**Phase 1 — "what are the valid sizes?"** Python `FontSize` + derived
`FONT_SIZES` + `_check_font_size()` wired into `label()` / `span()` / `style()`;
fix the call sites/tests that pass `24` (`app/examples/demo.py:41`,
`screens.py:11` docstring, `app/tests/test_screens.py:90/111`); Rust re-export +
`FONT_SIZES`; docs + `llms.txt`. Gate: `just app-test`, `just app-lint`,
`just rust-lint`.

**Phase 2 — the runtime override.** Proto arm + `ProtocolVersion` 15; firmware
`apply_to_obj()` branch; `build_property_override()` rule; sim mirror; JSON key;
tests (below). Gate: `just build-proto`, `app-test`, `app-lint`,
`firmware-build`.

**Phase 3 — Rust helper.** `Touchy::set_properties(Vec<SetPropertyCmd>)` (the
crate's first override helper, one RPC) + `Touchy::set_font_size(widget_id,
size)` sugar; update the "Not here yet" bullet; add a runnable
`rust/touchy-pad/examples/` snippet. Gate: `just rust-test`, `just rust-lint`.

**Phase 4 — optional follow-ups.** Route C (device-reported list) if a board
ships a different set; a `--font-size` CLI flag (only if `--int` proves
confusing); sim `setPixelSize`; per-span font markup.

## 7. Testing strategy

* **Python (`just app-test`)** — `app/tests/test_set_properties.py`:
  `build_property_override("lbl", "text_font", 30)` and
  `(…, FontSize.FONT_SIZE_30)` both produce `font_size_value`; a non-int value
  for `text_font` raises `TypeError`; `FontSize.FONT_SIZE_DEFAULT` →
  `font_size_value == 0`;
  batch order preserved. `app/tests/test_screens.py`: `label(font_size=24)` now
  raises, `font_size=0/16/FontSize.FONT_SIZE_30` round-trip, `FONT_SIZES` equals
  the enum's non-zero values, and the existing span/style assertions still hold.
  `app/tests/test_sim_transport.py` / `test_set_properties.py`: a `text_font`
  override is applied to a label's style in the sim, is sticky across a screen
  reload, and `None` removes it. `test_network_http.py`-style: the sim HTTP
  endpoint accepts `fontSizeValue`.
* **Parity test (the one thing codegen cannot check)** — read
  `firmware/sdkconfig.defaults` and assert every non-zero `FontSize` member has a
  matching `LV_FONT_*` enable line and vice versa, so adding a font to sdkconfig
  without widening the enum (or the reverse) fails CI rather than silently
  changing what the device renders.
* **Firmware** — no unit tests in this repo: `just firmware-build` for
  `jc4827w543` is the compile gate (the new arm must build against nanopb +
  LVGL), plus a documented **on-hardware checklist**: `touchy property set title
  text_font 30 --int` on a live label resizes it immediately; `… text_font 0
  --int` restores the theme font; `… text_font 24 --int` returns `INVALID_ARG`
  with a device WARN; a reload/page swap keeps the override.
* **Rust** — `just rust-test` for `FONT_SIZES`, the enum re-export and
  `set_font_size()`'s encoding (reuse the crate's existing client test doubles),
  `just rust-lint` for `missing_docs`.
* **Docs** — `just build-docs` is *not* needed (plans and guides are plain
  Markdown); keep `docs/python-api.md`'s table and `llms.txt` in step, since
  those are the AI/human entry points.

## 8. Risks / open questions

1. **Route A vs Route B (§4.1)** — *needs a decision.* The proto enum is cleaner
   and generated, but it does add a schema type for what is really a build-time
   fact. Route B (host constants + parity test) is the smallest possible change.
   Recommendation: **Route A**.
2. **Strict or advisory validation?** Strict (raise on `24`) removes the silent
   wrong-size failure mode, but it is a **breaking change for existing callers**:
   `app/examples/demo.py:41`, the `screens.py` docstring example and
   `app/tests/test_screens.py:90` all use `24`, and an out-of-tree host that
   passes an unsupported size starts raising instead of rendering at the theme
   size. Advisory (publish the list, add a `logger.warning`, never raise) is
   non-breaking. Recommendation: **strict**, with a `ValueError` that lists the
   valid sizes — but this is the item most worth the user's explicit nod. A
   middle option: strict for `label()` / `span()` (the documented authoring
   sugar) and advisory for `style(font_size=)`, which is the lower-level escape
   hatch — slightly inconsistent, so only if a caller needs an arbitrary value.
3. **Bump `ProtocolVersion` to 15, or not?** (§4.3) Recommendation: **bump**.
   Cost: one enum constant + regenerated bindings + the sim's reported value.
   Without it, an older device answers `OK` to a font override it silently
   dropped (and interpreted as a *removal*).
4. **Per-board font sets.** If a board ever compiles a different set (or a board
   wants a size outside the enum), the enum becomes a lie. Mitigations today: the
   parity test (§7) plus the device-side WARN. If it actually happens, route C
   (device-reported `font_sizes`) is the principled fix — deliberately deferred.
5. **`font_for_size()` nearest-match.** Should an unmapped `24` render as `30`
   instead of falling back to the theme? Recommendation: **no** — an explicit
   authoring error beats a silent approximation, and the enum + validation make
   the approximation unnecessary. (If it is ever wanted, it is ~10 lines in
   `font_for_size()` and would *weaken* the enum's meaning.)
6. **Per-span font markup** (`#FONT:30 …#` in the lb24 recolour parser) is out of
   scope: the span list is authored from Python protos anyway (`span(font_size=)`
   already works), and a live `text` override re-parses markup only for colours.
   Revisit if a host needs mixed sizes over the one-RPC-per-frame path.
7. **Rust scope.** Phase 3 adds the crate's first override helper. If the user
   prefers to keep Rust to "constants + enum only", Phases 0–2 still deliver
   everything the Python consumers asked for; the Rust helper is the piece that
   makes it *actionable* from Rust, so it is recommended but separable.
8. **Sim fidelity.** The sim renders the pixel size as a Qt *point* size
   (`sim/widgets.py:328`), so sizes look ~33 % larger than on the device. Only
   advisory rendering, unchanged here; noted so nobody "fixes" the enum to match
   the sim.

## 9. How it will be validated (the handover checklist)

```
just build-proto          # Python + C + Rust bindings, + embedded default screen
just app-test             # pytest: DSL validation, override mapping, sim mirror
just app-lint             # ruff format + lint
just rust-lint            # cargo fmt --check + clippy -D warnings
just rust-test            # cargo test --workspace (FONT_SIZES, enum, helper)
just firmware-build       # ESP-IDF build for jc4827w543 (the new arm compiles)
```

plus the on-hardware checklist in §7 and a manual `touchy property set … --int`
round-trip against the simulator (`just app-run -- --sim-headless …`).

When it lands: add the stage section to `docs/design.md`, the highlight bullets to
`AGENTS.md`, a `CHANGELOG.md` entry, the plan's status header → **DONE**, and the
one-line entry in `.clinerules/plans.md`.
