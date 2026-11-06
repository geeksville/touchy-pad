# Plan — custom label scrolling speed (+ document the default)

**Status: implemented (Stage lb19)** — all three phases landed, with one
improvement over the original Phase-3 sketch (see §9).

**Ask:** give API consumers a clean way to choose the scrolling *speed* of a
scrolling LVGL text label, and document what the default speed is.

**Touched sides:** `proto/` (two additive fields), `firmware/main/`
(`build_label` + one log line), `app/src/touchy_pad/` (DSL kwargs, CLI `--int`,
docs + tests). **No** `rust/` change (the Rust crate has no widget DSL — it uses
the linked prost types directly), **no** simulator change (Qt has no marquee;
see §6). Wire: the new fields are additive on `Label`, so
**`Widget.Version` 29 → 30**; no `ProtocolVersion` /
`PreferencesFile.Version` change.

---

## 1. What exists today

| Piece | Where |
|---|---|
| `Label { text, text_align, long_mode, recolor }` | `proto/widgets.proto:614-629` (tags 1, 3, 4, 5) |
| `LongMode { WRAP, DOTS, SCROLL, SCROLL_CIRCULAR, CLIP }`, values map 1:1 onto `lv_label_long_mode_t` | `proto/widgets.proto:604-613` |
| `build_label()` — sets text, bg/text opa, `text_align`, `long_mode`, `recolor`, logs a one-line `ESP_LOGI` | `firmware/main/widgets/widget_builders.cpp:200-244` |
| DSL `label(id, text, font_size, text_align, long_mode, recolor, rect, style, animations)` | `app/src/touchy_pad/api/screens.py:751-802` |
| Sim label rendering (`setWordWrap` only — no marquee) | `app/src/touchy_pad/sim/widgets.py:305-329` |

So today a host can pick *whether* a label scrolls (`long_mode`) but **not how
fast**: the speed is LVGL's compiled-in default, and there is no field, no DSL
kwarg and no documented way to change it.

**One undocumented escape hatch already exists** (worth capturing in this
stage): LVGL exposes the style property `anim_duration` by *name*, and
`lv_obj_property_get_id()` falls back to `lv_style_property_get_id()`
(`lv_obj_property.c:220-240`, name table `widgets/property/lv_style_properties.c:20`).
`lv_obj_set_property()` routes any id `< LV_PROPERTY_ID_START` (0x100) to
`lv_obj_set_local_style_prop()` (`lv_obj_property.c:109-125`, and `num`
deliberately aliases `style.num` — `lv_obj_property.h:135-186`), which is
exactly what our Stage-lb14 `SetPropertiesCmd` path does
(`firmware/main/widgets/widget_property.cpp:55-108`). So:

```python
pad.set_properties([build_property_override("marquee", "anim_duration", 4000)])
```

already works from the Python/Rust API — **but not from the CLI**, because
`touchy property set` always sends a *string* value
(`app/src/touchy_pad/cli.py:975-992`), which would arrive as `prop.ptr` for a
style prop. Nothing documents any of this.

---

## 2. The LVGL facts we must document (and pin)

All verified against the vendored **LVGL 9.5.0**
(`firmware/managed_components/lvgl__lvgl/`, `idf_component.yml: version: 9.5.0`).

**The only speed knob is the `anim_duration` style property.** `lv_label.c`
takes it verbatim and uses it as the animation duration:

```c
/* lv_label.c:1116-1124 (SCROLL) and :1231-1242 (SCROLL_CIRCULAR) */
uint32_t anim_time = lv_obj_get_style_anim_duration(obj, LV_PART_MAIN);
if(anim_time == 0) anim_time = LV_LABEL_DEF_SCROLL_SPEED;   /* lv_label.c:35 */
...
lv_anim_set_duration(&a, anim_time);
```

* `LV_LABEL_DEF_SCROLL_SPEED = lv_anim_speed_clamped(40, 300, 10000)`
  (`lv_label.c:35`) — a **speed-encoded** value, not a duration. It is resolved
  per instance by `lv_anim_resolve_speed()` (`lv_anim.c:522-534`): stored speed
  = `(40+5)/10 = 4` (`lv_anim.c:258`) and `time_ms = d_px * 100 / 4` → **25 ms
  per pixel, i.e. a default of 40 px/s**, clamped to **300 ms … 10 000 ms** per
  traversal (the pack/clamp round-trip through 10-bit fields is why the code is
  full of `/10` and the *10230* oddity at `lv_anim.c:241-262`).
* A **plain millisecond value passes straight through** — `lv_anim_resolve_speed()`
  returns it unchanged (`lv_anim.c:525`) and it is **not** clamped, so a host
  that sets `4000` gets exactly 4000 ms.
* **Distance, i.e. "one traversal"** (`lv_label.c:1127-1184`, `:1245-1300`):
  * `LONG_MODE_SCROLL_CIRCULAR`: `offset.x` goes `0 → −(text_width + 3 spaces)`
    (the gap is `LV_LABEL_WAIT_CHAR_COUNT`, default 3 — `lv_label.c:963`,
    `lv_conf_internal.h:2481-2485`), repeating forever.
  * `LONG_MODE_SCROLL` (bounce): distance `= visible_width − text_width`, run
    forwards **and** backwards with a **300 ms pause at each end**
    (`LV_LABEL_SCROLL_DELAY`, `lv_label.c:36`, applied at `:1124-1125`). Both
    directions use the same `anim_time`, so a full bounce cycle is
    `2 × duration + 600 ms`.
  * Neither mode animates at all unless the text overflows the fixed box
    (`if(size.x > lv_area_get_width(&txt_coords))`).
* **The style `anim` template cannot set the speed.** `overwrite_anim_property()`
  (`lv_label.c:1034-1057`) copies only `act_time`, `repeat_cnt`, `repeat_delay`
  and the callbacks from a template animation — **not** `duration`. Use
  `anim_duration`.
* **The value is re-read on every relevant change.** `lv_label_event()` reacts to
  `LV_EVENT_STYLE_CHANGED` *and* `LV_EVENT_SIZE_CHANGED` by re-running
  `lv_label_refr_text()` (`lv_label.c:796-798`), which re-creates the scroll
  animation with the current `anim_duration`. That is what makes both the
  authoring-time field and the Stage-lb14 runtime override stick, including
  across a runtime `text` change.

### The "default scrolling speed" text we will ship

| Long mode | Behaviour | Default speed |
|---|---|---|
| `WRAP` / `DOTS` / `CLIP` | no animation | — |
| `SCROLL` (bounce) | slides out and back, 300 ms pause at each end | 40 px/s → `duration = clamp(1000 × distance_px / 40, 300 ms, 10 000 ms)` |
| `SCROLL_CIRCULAR` | continuous loop, 3 space-glyphs of gap between laps | same 40 px/s, same 300 ms…10 s clamp |

Converting a target speed to a duration (what a host should do when it wants a
specific px/s):

```
duration_ms ≈ 1000 × distance_px / speed_px_s
distance_px = text_width_px + gap_px            # SCROLL_CIRCULAR; gap ≈ 3 space widths
distance_px = text_width_px − visible_width_px  # SCROLL
```

The host does not know `text_width_px` (it depends on the resolved Montserrat
font on the device), so the value carried on the wire is the **duration**; §3
explains why, and §5 Phase 3 sketches a px/s convenience built on top.

---

## 3. API design (recommended)

### Wire — one additive field on `Label`

```proto
message Label {
    string text      = 1;
    TextAlign text_align = 3;
    LongMode long_mode = 4;
    bool recolor = 5;

    // Scrolling SPEED for the SCROLL / SCROLL_CIRCULAR long modes, two
    // interchangeable spellings (see the shipped proto comment for the
    // full text): `scroll_duration_ms` is the ms lap time, applied
    // verbatim; `scroll_speed_px_s` is px/s (10..10000), which the
    // device resolves against the label's real text length. Duration
    // wins if both are set. Unset = LVGL's 40 px/s default.
    optional uint32 scroll_duration_ms = 6;
    optional uint32 scroll_speed_px_s  = 7;
}
```

Tag 6/7 are free (`text`, `text_align`, `long_mode`, `recolor` are 1/3/4/5).
`optional` + proto3 presence keeps "unset" distinguishable from an explicit
`0`; the firmware treats both as "LVGL default", and the docs say so.

### Firmware — three lines in `build_label`

```cpp
// Stage lb19 — scroll speed. LVGL drives a scrolling label with the
// `anim_duration` style property and re-reads it whenever the label's
// style/size/text changes, so a runtime `text` override keeps it.
if (w.kind.label.has_scroll_duration_ms && w.kind.label.scroll_duration_ms > 0) {
    lv_obj_set_style_anim_duration(lbl, w.kind.label.scroll_duration_ms, 0);
}
```

…plus the existing `ESP_LOGI` in `build_label` gaining the new value (the repo
convention is that every new field shows up in the boot log). Call it **after**
`lv_label_set_long_mode()`; the style change only emits
`LV_EVENT_STYLE_CHANGED`, which marks the label dirty for the next layout pass,
so the ordering is not load-bearing.

### Host DSL — one kwarg, mirroring `recolor`

```python
def label(
    id: str,
    text: str = "",
    font_size: int = 0,
    text_align: int = TextAlign.AUTO,
    long_mode: int = LongMode.LONG_MODE_WRAP,
    recolor: bool = False,
    scroll_duration_ms: int = 0,     # NEW — 0/unset = LVGL default (40 px/s)
    rect: _proto.Rect | None = None,
    style: _proto.Style | Iterable[_proto.Style] | None = None,
    animations: _proto.Animation | Iterable[_proto.Animation] | None = None,
) -> _proto.Widget:
```

```python
if scroll_duration_ms:
    if scroll_duration_ms < 0:
        raise ValueError("scroll_duration_ms must be >= 0 (0 = LVGL default)")
    w.label.scroll_duration_ms = int(scroll_duration_ms)
```

Guarding on truthiness (like the existing `font_size` sugar) keeps "unset" on
the wire for the common case; rejecting negatives host-side avoids a protobuf
`uint32` assignment error that reads like a library bug.

Worked example (goes into the docs):

```python
# A brisk ticker: one 480 px lap every 4 s (≈120 px/s, i.e. 3× LVGL's default).
s += label(
    "ticker",
    text="#00FF00 live# " * 3,
    recolor=True,
    long_mode=LongMode.LONG_MODE_SCROLL_CIRCULAR,
    scroll_duration_ms=4000,
    rect=rect(x=0, y=0, w=480, h=24),
)
```

### Runtime override — document, don't re-build

`build_property_override(widget_id, "anim_duration", 4000)` (an **int**, so it
lands as `int_value` → `prop.num` → `lv_obj_set_local_style_prop()` on the
label's main part) re-times a live marquee, and it is sticky for the session
exactly like `text`. That needs a paragraph in the docs; the CLI gap is Phase 2.

### Why a duration, not a px/s "speed" field — and what was rejected

| Option | Verdict |
|---|---|
| **`Label.scroll_duration_ms` → `lv_obj_set_style_anim_duration()`** | **Chosen.** Exact, LVGL-native, ~3 lines of firmware, no LVGL internals touched, no dependence on text length, no measurement code, no drift when the text changes at runtime. |
| `Label.scroll_speed_px_s` → firmware forwards a *speed-encoded* duration via `lv_anim_speed_clamped()` | **Shipped as well (Phase 3),** and much simpler than this plan first assumed — see §9. It is kept *alongside* the duration because the two have different invariants (constant lap time vs constant px/s when the text changes). |
| `Style.anim_duration` field + `style(anim_duration=...)` sugar | Rejected as the *primary* API: it is literally the same LVGL prop, but it hides a label-scrolling concept in the generic style bag, while `long_mode` (its partner) lives on `Label`. The generic route already exists at runtime via the `anim_duration` property *name* (§1) — document that instead. |
| New `Label` enum `scroll_speed = SLOW/NORMAL/FAST` | Rejected: a coarse second knob with arbitrary mappings that cannot express "match my other labels" or "N px/s". |
| LVGL `anim` style template + `lv_anim_set_speed()` | Impossible — `overwrite_anim_property()` does not copy `duration` (§2). |

---

## 4. File & component layout

| File | Change |
|---|---|
| `proto/widgets.proto` | `Label.scroll_duration_ms = 6` (+ comment); `Widget.Version.CURRENT` 29 → 30 |
| `proto/widgets.options` | none (`Label.text max_size:128` is unchanged; the new field is a scalar) |
| `firmware/main/widgets/widget_builders.cpp` | 3 lines + the field in the existing `build_label` log line |
| `app/src/touchy_pad/api/screens.py` | `label()` kwargs + docstrings (`scroll_duration_ms`, `scroll_speed_px_s`) + the `SCROLL_SPEED_MIN/MAX_PX_S` constants |
| `app/src/touchy_pad/api/__init__.py` | re-export the two constants |
| `app/src/touchy_pad/cli.py` | `touchy property set … --int` (Phase 2) |
| `app/tests/test_screens.py` | round-trip + default-unset + negative + both-forms + range tests |
| `app/tests/test_cli.py` | `--int` / string / bad-literal tests |
| `docs/python-api.md` | new "Scrolling speed" subsection (the default table lives here) |
| `docs/design.md` | new `## Stage lb19 … — DONE` section at the end |
| `AGENTS.md` / `CLAUDE.md` (symlink) | version line (`Widget.Version` 29 → 30) + one stage bullet |
| `.clinerules/plans.md` | index entry for this plan |
| `rust/` | **unchanged** — prost types are regenerated from the symlinked proto by `just rust-build`; the crate has no widget DSL, and `set_properties` already accepts `"anim_duration"` with an `i32` |
| `app/src/touchy_pad/sim/**` | **unchanged** — see §6 |

Regenerated, never hand-edited: `firmware/main/proto/*.pb.{c,h}`,
`app/src/touchy_pad/_proto/*_pb2.py` (`just build-proto`), and
`firmware/main/default_screen_pb.h` / `proto/default_screen.json`
(`just build-default-screen` — unaffected, but the recipe regenerates them).

---

## 5. Phased implementation

### Phase 0 — document the default (docs-only, shippable on its own) — **DONE**

1. `docs/python-api.md`: new subsection under "Authoring screens" →
   *"Scrolling speed (stage lb19)"* containing the default-speed table from §2,
   the `duration_ms ≈ 1000 × distance_px / speed_px_s` conversion, and the
   runtime-override paragraph from §3.
2. Note the escape hatch that works *today* (`"anim_duration"` property with an
   int) so existing hosts are unblocked before the field ships.
3. No proto, firmware or code change; `just app-test` stays green untouched.

### Phase 1 — the authoring-time knob (the actual stage) — **DONE**

1. `proto/widgets.proto`: add the field + bump `Widget.Version.CURRENT` to 30.
2. `just build-proto`.
3. `firmware/main/widgets/widget_builders.cpp`: apply the style prop + log.
4. `app/src/touchy_pad/api/screens.py`: the kwarg, validation, docstring.
5. Tests (§6), then docs (`docs/design.md` stage section, `AGENTS.md`, this
   plan marked DONE, `.clinerules/plans.md`).

### Phase 2 — close the CLI gap on runtime overrides — **DONE**

`touchy property set` only ever sent strings (`cli.py`). It now takes `--int`
(`touchy property set ticker anim_duration 4000 --int`), which is what makes an
LVGL **style** property reachable from the shell; the command validates the
literal with `int(raw, 0)` (decimal/`0x`/`0b`) and raises a `click.BadParameter`
otherwise. Tests in `tests/test_cli.py` cover the int form, the unchanged string
form and the bad-literal case.

Rejected: auto-detecting a numeric-looking value — a label whose `text` happens
to be `"123"` would silently become an integer property and fail.

### Phase 3 — px/s sugar — **DONE** (and far simpler than sketched)

See §9 for the design that shipped. The original sketch below (measure the text
in the firmware, then re-apply on every `text` change) turned out to be
unnecessary:

* ~~New helper `label_apply_scroll_speed(lv_obj_t *, uint32_t px_s)` measuring
  the text with `lv_text_get_size_attributes()` + `LV_TEXT_FLAG_RECOLOR`, adding
  the 3-space gap, clamping to `[100, 60000]` ms.~~
* ~~Invoke the same helper from the `text` override path
  (`firmware/main/widgets/widget_property.cpp`) so a changing marquee keeps
  constant px/s.~~

Both were dropped: LVGL accepts a **speed-encoded** value in `anim_duration` and
resolves it against the real distance itself, on every `refr_text()` — so one
`lv_anim_speed_clamped()` call at build time gives exact px/s *and* constant
speed across text changes, with no measurement code and no hooks.

---

## 6. Testing strategy

**Host (pytest, `just app-test`)** — mirrors the Stage-lb17 `recolor` tests:

* `test_label_scroll_duration_round_trips()` in `app/tests/test_screens.py`:
  `label("ticker", text="abc", long_mode=LongMode.LONG_MODE_SCROLL_CIRCULAR,
  scroll_duration_ms=4000)` → `_proto.Screen.FromString(s.to_bytes())` →
  `w.label.scroll_duration_ms == 4000`.
* `test_label_scroll_duration_defaults_to_unset()`: the default keeps the field
  **absent** (`not w.label.HasField("scroll_duration_ms")`), so the wire stays
  byte-compatible with the old shape for hosts that don't use it.
* `test_label_scroll_duration_rejects_negative()`: `pytest.raises(ValueError)`.
* A DSL-docstring/kwarg test only if the suite already has that pattern
  (`test_screens.py:1011-1019` is the model to extend).

Presence semantics were verified locally against the generated bindings (the
same rule will hold for the new field, which is declared `optional` like
`Style.bg_color`): `_proto.Style().HasField("bg_color")` is `False` and
serialises to **0 bytes**, while an explicit `bg_color = 0` makes `HasField`
`True` and emits 2 bytes. Hence the truthiness guard in the DSL: hosts that
never pass `scroll_duration_ms` keep today's exact wire bytes, and
`has_scroll_duration_ms` is a meaningful check on the device even though the
firmware treats "unset" and "explicit 0" identically.

**Firmware.** No unit tests exist (by design). Validation is
`just firmware-build` (default board `jc4827w543`) — the new call is a single
`lv_obj_set_style_anim_duration()` and compiles into every board; the
`ESP_LOGI` in `build_label` prints the value so a device log confirms decode.
Hardware check: push a screen with a `SCROLL_CIRCULAR` label and
`scroll_duration_ms`, load it, and eyeball the speed; then re-time it live with
`set_properties("anim_duration", …)` and confirm the change takes effect
*without* a screen reload (that is the `LV_EVENT_STYLE_CHANGED` path).

**Sim.** No change and no sim test. `touchy_pad/sim/widgets.py:305-329` already
renders non-`WRAP` long modes as a single clipped line with the comment
*"Qt has no built-in marquee … close enough for advisory sim rendering of
SCROLL / DOTS / CLIP"*. Adding a `QTimer` marquee would be new visual state, a
new teardown path and new flakiness for a knob that only affects animation
timing in a renderer that does not animate — out of scope for this stage
(revisit only if someone asks for a scrolling sim preview).

**Recipes that must pass:** `just build-proto`, `just app-test`
(283 passed / 2 skipped after this stage), `just app-lint`,
`just firmware-build`. `just rust-test` / `just rust-lint` only if the proto
regen happens to touch the Rust surface (it should not).

---

## 7. Documentation deliverables (what "documented" means here)

1. `proto/widgets.proto` — the field comment above is the wire-level spec
   (defaults, units, which long modes it applies to).
2. `docs/python-api.md` → new **"Scrolling speed"** subsection with the
   default-speed table from §2, the px/s↔duration conversion, the worked
   example, the `0 = LVGL default` rule, the "ignored unless the text
   overflows a fixed box" caveat, and the runtime `anim_duration` override.
3. `docs/design.md` → `## Stage lb19: label scroll speed — DONE` section in the
   house style (Status / Behaviour / Wire / Docs), plus the version reference in
   the surrounding stage notes.
4. `AGENTS.md` (+ the `CLAUDE.md` symlink) → the version line becomes
   `Widget.Version.CURRENT == 30`, plus one bullet summarising the field, the
   default (40 px/s) and the runtime `anim_duration` escape hatch.
5. `.clinerules/plans.md` → index entry ("**Done — Stage lb19**", or
   "draft" while unimplemented) and, when the stage lands,
   `.clinerules/memory-bank/{activeContext,progress}.md` per the memory-bank
   rules.

---

## 8. Risks & open questions

**Open questions — all resolved by the implementation (see §9)**

1. **Duration (ms) vs speed (px/s) as the wire unit.** *Resolved: both shipped.*
   `scroll_duration_ms` is the exact LVGL-native knob (constant lap time);
   `scroll_speed_px_s` is the consumer-friendly unit (constant px/s, resolved
   device-side). They are additive, not exclusive; the DSL rejects sending both.
2. **Should tiny/absurd values be clamped?** *Resolved: the duration is passed
   through verbatim* (predictable, LVGL-consistent) and the docs warn about CPU
   cost; the *speed* form is clamped to `10..10000` px/s because LVGL's
   `(speed + 5) / 10` encoding would otherwise produce 0 and
   `lv_anim_resolve_speed()` would divide by it.
3. **Phase 2 (CLI `--int`) in or out of scope.** *Resolved: in — shipped.*

**Non-blocking risks**

* **`Widget.Version` 29 → 30 deletes old screen files.** The firmware validates
  the root widget version on load and deletes on mismatch (Stage 56), so after a
  firmware update hosts must re-upload their screens (`pad.screen_save(...)` +
  `pad.screen_load(...)`, or `touchy init` for the stock chrome) — the usual cost
  of any `Label` field addition, worth a line in the release notes.
* **The runtime override is session-scoped (RAM only)** and sticky, so after a
  reboot the screen file's `scroll_duration_ms` (or LVGL's default) wins again.
  Document, don't fix.
* **Never-`SIZE_CHANGED` gotcha** — moot in the end: the speed form is resolved
  *inside* LVGL (`lv_anim_resolve_speed()` runs on every `lv_label_refr_text()`),
  so no event hook is needed. Kept here so a future px/s-in-software change knows
  the trap exists.
* **Sim parity gap** is deliberate (§6): the sim shows scrolling labels as
  clipped single lines. If that ever becomes misleading, the fix is a `QTimer`
  marquee honouring the same duration — a separate, opt-in change.
* **Long marquee text** is still capped by `Label.text max_size:128`
  (`proto/widgets.options:18`, raised to 512 for the *runtime* `text` override in
  lb17). A long ticker must use a runtime override or several labels; unrelated
  to this stage but easy to hit while writing the docs example.

**Verification checklist (as run for Phase 1)**

```sh
just build-proto                      # regenerates C + Python bindings
just app-test                         # 283 passed, 2 skipped
just app-lint                         # ruff format + check: clean
just firmware-build                   # default board jc4827w543
# optional, hardware:
#   just app-run -- init                       # chrome + baseline trackpad page
#   python: pad.screen_save(marquee_screen); pad.screen_load("F:host/s/<name>.pb")
#   device log: build_label ... scroll_duration_ms=4000 scroll_speed_px_s=0
#   live retime: pad.set_properties([build_property_override("ticker", "anim_duration", 1000)])
```

> **Drive-by done in the same pass:** `docs/design.md` (Stage-21 summary, the two
> manual-hardware notes, the Stage-68 summary), `docs/companion-app.md`'s CLI
> table, `docs/why-not-xml.md` and the `screens.py` docstrings all mentioned
> `touchy screens push` / `touchy screen push SCRIPT` / `touchy screen init`,
> none of which exist in `app/src/touchy_pad/cli.py`. They now point at the real
> surface: `touchy init` + `pad.screen_save()` / `pad.screen_load()`.

---

## 9. What actually shipped (implementation notes)

* **Phase 0** — `docs/python-api.md` gained a **"Scrolling speed (stage LB19)"**
  section: both kwargs, the default-speed table (40 px/s, 300 ms…10 s lap,
  300 ms bounce pause, 3-space circular gap), the
  `duration_ms ≈ 1000 × distance_px / speed_px_s` conversion, the "must overflow
  a fixed box" caveat, and the runtime `anim_duration` re-timing recipe
  (Python + `touchy property set … --int`).
* **Phase 1** — `Label.scroll_duration_ms = 6` (`Widget.Version` 29→30);
  `build_label` applies it with `lv_obj_set_style_anim_duration()` and logs both
  fields; DSL kwarg `scroll_duration_ms=` with validation; 12 new pytest cases
  in `tests/test_screens.py` (round-trip ×2, presence, negative ×2, both-forms,
  out-of-range ×3, range endpoints ×2, bounds).
* **Phase 2** — `touchy property set … --int` (+ 3 pytest cases in
  `tests/test_cli.py`), validated with `int(raw, 0)`.
* **Phase 3** — `Label.scroll_speed_px_s = 7`, mapped to
  `lv_anim_speed_clamped(px_s, 300, 10000)`; DSL kwarg `scroll_speed_px_s=`
  guarded to `10..10000` (`SCROLL_SPEED_MIN_PX_S` / `SCROLL_SPEED_MAX_PX_S`,
  exported); firmware clamps the same range so a hand-built proto can't trigger
  LVGL's divide-by-zero. Duration wins when both are set on the wire; the DSL
  rejects that combination outright.

**The key insight that simplified Phase 3:** LVGL's `anim_duration` accepts a
*speed-encoded* value (`lv_anim_speed_clamped()`, bit 31 marker) and resolves it
per animation start via `lv_anim_resolve_speed()`, which is exactly how LVGL
itself implements its 40 px/s default. Since `lv_label_refr_text()` re-creates
the scroll animation on every text/size/style change, a px/s value stored in the
style stays a px/s value — no firmware text measurement, no re-apply hook, no
per-label bookkeeping. The value round-trips through the style intact because
the generated setter stores `.num = (int32_t)value` and the getter returns
`(uint32_t)v.num`.

**Deliberately not done:** simulator animation (Qt has no marquee — the sim's
existing "clipped single line" behaviour is documented), any `rust/` code change
(prost regen only), and clamping of absurdly small *durations* (passed through
verbatim, documented).
