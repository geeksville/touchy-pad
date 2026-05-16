# Stage 15 — Host-uploaded screen layouts (revised)

## Background: why not XML

Earlier drafts of stage 15 tried to reuse LVGL's XML UI loader so layouts could be authored in a human-readable, declarative form and uploaded from the host. That approach was abandoned after a multi-week experiment:

- **LVGL's in-tree XML loader was removed in v9.4** (lvgl PR #9565). v9.3's loader is still there but has bugs that affect even the "hello world" cases (e.g. unresolved consts, missing complex-gradient fields). Staying on 9.3 forever to keep a buggy loader is a dead end.
- **Lui-XML** (https://github.com/lui-xml/lui-xml) is the community spin-out of that removed loader. It needs significant packaging fixes to build under ESP-IDF Component Manager (4-segment manifest version, no IDF-aware `CMakeLists.txt`, hard-coded `../<dir>/` includes that only resolve inside lvgl's source tree). Even after fixing all of that — see fork at `geeksville/lui-xml` branch `fix-idf-component` — the actual sources still reference LVGL symbols that no longer exist in 9.5: `lv_obj_find_by_name`, `LV_GLOBAL_DEFAULT()->lv_event_xml_store_timeline`, a private `LV_USE_OBJ_NAME` Kconfig, and several typedef aliases that lvgl removed alongside its XML code. Making it compile is effectively a port, not a packaging fix, and there's no upstream maintenance signal indicating it would stay in sync with future lvgl releases.
- XML brings a parser (expat) and a string-attribute coercion layer into the firmware just to express what is, in practice, a flat list of widgets with a handful of properties each. The runtime cost (~80 KB flash for expat + lvgl-side XML glue) is large for what we actually need.

**Decision:** drop XML entirely. Layouts are uploaded as protobuf-encoded screen definitions and rendered by a small device-side interpreter that calls LVGL's normal C API.

## Wire & on-disk format

- One protobuf message per screen, defined in `proto/touchy.proto` (extension of the existing host-api schema).
- Encoded screens are written to LittleFS at `/littlefs/from_host/screens/<name>.pb` by the existing host-api file-upload path (stage 13). No new transport.
- The device-side renderer (`screens.cpp`) is the only consumer; it decodes a `.pb` with nanopb and instantiates widgets one-by-one.

## Schema sketch

```proto
// proto/touchy.proto (additions)

message Screen {
  string name        = 1;   // matches the filename stem
  Layout layout      = 2;   // optional row/col/grid; default = abs positioning
  repeated Widget widgets = 3;
}

message Widget {
  string  id    = 1;        // stable handle used by host_api events
  Rect    rect  = 2;        // x,y,w,h in pixels (or grid cells if Layout set)
  Style   style = 3;        // optional: bg color, radius, border, padding
  oneof kind {
    Button   button   = 10;
    Label    label    = 11;
    Slider   slider   = 12;
    Switch   toggle   = 13;
    Image    image    = 14;
    Arc      arc      = 15;
    Spacer   spacer   = 16;
    // … extend as needed; reserve enough field numbers for future widgets
  }
}

message Button { string text = 1; Action on_click = 2; }
message Label  { string text = 1; FontRef font = 2; }
message Slider { int32 min = 1; int32 max = 2; int32 value = 3;
                 Action on_change = 4; }
message Switch { bool on = 1; Action on_change = 2; }
message Image  { string asset = 1; }       // path into /littlefs/from_host/img/
message Arc    { int32 min = 1; int32 max = 2; int32 value = 3; }
message Spacer { }

message Rect  { int32 x = 1; int32 y = 2; int32 w = 3; int32 h = 4; }
message Style { uint32 bg_color = 1; int32 radius = 2; int32 border = 3;
                int32 pad = 4; uint32 text_color = 5; }
message Action { string event = 1; }       // sent back to host via host_api

message Layout {
  enum Kind { ABSOLUTE = 0; ROW = 1; COL = 2; GRID = 3; }
  Kind kind     = 1;
  int32 cols    = 2;        // GRID only
  int32 gap     = 3;
}
```

`touchy.options` should mark every `repeated`/`string`/`bytes` field with explicit `max_size`/`max_count` so nanopb can keep the decoder allocation-free.

## Python tool (`app/`)

The companion app gains an `app.screens` module that lets a layout be expressed in idiomatic Python and then serialised to a `.pb`:

```python
from touchy_pad.screens import Screen, button, slider, toggle, label, grid

s = Screen("synth", layout=grid(cols=4, gap=8))
s += label("title", "Synth")
s += button("play",  text="▶",  on_click="synth.play")
s += slider("cutoff", min=20, max=20000, on_change="synth.cutoff")
s += toggle("mute",  on=False,            on_change="synth.mute")

s.write("build/screens/synth.pb")          # nanopb-compatible binary
```

A CLI wrapper (`touchy-pad screens push synth.py`) compiles the script, uploads each resulting `.pb` via the existing host-api file transport, and tells the device to reload.

## Device-side renderer (`firmware/main/screens.cpp`)

- On boot, scan `/littlefs/from_host/screens/*.pb`, decode each with nanopb, build an in-memory `std::vector<ScreenDef>`.
- `screens_load(name)` looks up the def, creates an `lv_obj_t * scr = lv_obj_create(NULL)`, iterates `widgets`, dispatches on the `oneof kind` to an `lv_*_create` call, and applies `Rect`/`Style`. Widget IDs are stored via `lv_obj_set_user_data` for event routing.
- A single `lv_event_cb` on each interactive widget posts `{screen, widget_id, action.event, value}` back through the existing host-api event channel — no new protocol bits needed.

## Why this is better than XML for our use case

- One source of truth (`touchy.proto`) shared between device + host; nanopb already in the build.
- No string-attribute parsing on-device; every property is already typed when it arrives.
- Trivially forward-compatible: unknown protobuf fields/widgets are skipped, so a newer host tool can target older firmware without breaking it.
- The Python side gives us full programmatic layout (loops, conditionals, importing constants) for free — something XML would need a templating layer to match.
- Smaller flash footprint (no expat, no lvgl XML glue, no font/image name → pointer lookup tables): ~80 KB saved vs the lui-xml route.

## Out of scope for stage 15

- Animations / timelines (lui-xml's main extra over plain widget instantiation). If we later want them, add an `Animation` message and a small tween runner around `lv_anim_t`.
- Styles-as-first-class-objects (lui-xml `<styles>` section). For now styles are inlined per-widget; if duplication becomes painful, add a top-level `repeated Style named_styles` and let `Widget.style_ref` point at one by index.
- A WYSIWYG editor. The Python DSL is the authoring surface.

## Migration notes

- Drop the `lui_xml` dep from `firmware/main/idf_component.yml`.
- Drop `CONFIG_LV_USE_XML` and `CONFIG_LV_USE_DRAW_SW_COMPLEX_GRADIENTS` from `firmware/sdkconfig.defaults` (the latter was only needed by lvgl's compiled-but-unused in-tree XML code).
- LVGL can stay on `~9.5.0` — nothing in the new renderer needs the 9.3 XML APIs.
- Delete `ui/screens/*.xml` and the `ui/` companion-app XML scaffolding; replace with `app/src/touchy_pad/screens/*.py`.
- The `geeksville/lui-xml` fork can stay parked on the `fix-idf-component` branch as a record of why this didn't work; no PR upstream.
