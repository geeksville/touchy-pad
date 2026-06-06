# General design goals/requirements

* Use platform.io as the build environment (vscode based, with an 'arduinoish' api)
* Use LVGL as the rendering library (provides layers and GUI primitives)
* Primiarily use C++ as the programming lanuage
* Use C++ classes as needed for new big systems

# Development stages

Development will proceeed in a series of stages.  Currently none of these stages are implemented, after implementing each stage update this document as appropriate.

## Stage 0: Stub appload builds under platformIO ✓ DONE

At this point a basic hello world app builds on plaformIO and runs on the target hardware.  It prints 'hello world' using the arduino console.

## Stage 1: Helloworld extended to support LCD display ✓ DONE

Prints 'hello world' on the LCD screen of the devboard

## Stage 10: Appears on the host as a USB HID mouse device. ✓ DONE

Work items:
* Create a new C++ class called TrackpadWidget (corresponding new a new lvgl widget)
* This class should contain an instance of a USBHidMouse (so it can send mouse events to the host when the user does gestures on the 'trackpad')
* Initially create an instance of TrackpadWidget filling most of the display.  But just above that widget reserve
space for a debug output line.
* if the user touches the TrackpadWidget, print debug output to the logs and the debug line at the top of the display
* Create the TrackpadWidget somewhere in setup() but check for touches etc... in loop()

### Multitouch gestures supported
* one finger tap = left click
* two finger tap = right click
* three finger tap = middle click
* one finger drag = mouse move
* (don't implement yet) two finger drag = scroll (left/right = horizontal scroll, up/down = vertical scroll)

## Stage 11: Protobufs defined ✓ DONE

* create a touchy.proto protobuf definition based on host-api.md.

## Stage 12: host side app created ✓ DONE

Create a python app based on the following:

* create in "app" directory, call the pypi package "touchy-pad", call the executable wrapper "touchy"
* poetry build system, using best practices for directory layout, test harness, ready to be published to pypi etc...
* update .devcontainer with required tools for building/running python/poetry/pip
* include github ci actions to build the python app 
* use a popular python library for python wrappers to talk using our touchy.proto to the USB device
* structure the app where most of the functionality is available as an API library, the command line "touchy" app just just uses that API library to do its work
* Implement "touchy getversion" - have it send a get version protobuf to the device and print the response.  I'll have you reuse most of this code in the future to implement other commands.
* select and use a popular usb library for communicating with the device
* improve host-api.md with more specifics on USB endpoints to support this API (so that I can eventually have the C++ device code refer to that document for its implementation)

## Stage 13: device side custom USB protocol ✓ DONE

* Most of this code should be in host_api.cpp
* per host-api.md add a new USB interface/endpoints
* listen on the command endpoint and send responses as appropriate
* for now, just implement the getversion command and associate responses, we'll add other stuff later
* after your done (and I've flashed the code) i'll use the python app to test it

Stage 13 implementation notes:
* `firmware/main/host_api.{h,cpp}` runs a FreeRTOS dispatcher task that
  reads `Command` frames (u16 LE length + nanopb payload) from the
  vendor-class bulk OUT endpoint and writes the matching `Response` to
  the bulk IN endpoint.
* The vendor interface (class `0xFF`, subclass `0x54`, protocol `0x01`)
  is declared in `usb_hid.cpp` alongside the existing CDC-ACM + HID-mouse
  interfaces. The host transport locates it by class, not interface number.
* Only `SysVersionGet` is fully implemented; every other command currently
  returns `RESULT_NOT_SUPPORTED`. The interrupt-IN event endpoint is
  deferred to a later stage (it requires a custom TinyUSB class driver to
  coexist with the bulk pair on the same interface); the host library
  treats it as optional.

## Stage 14: device side filesystem ✓ DONE

### Create fs.cpp/.h with a FS class inside
* Use LittleFS as the FS format
* Have the following standard base directories precreated if needed:
  * /prefs - contains device specific prefs/data (not yet used)
  * /from_host - a directory tree of files which the host has sent using FileSave/FileReset
* Have methods for:
  * readText(name) - returns file as a C++ str
  * readBinary(name) - news a byte array with the file contents, caller is expected to call delete[] on it later
  * remove(name) - delete a file
  * (construct some elegant api for iterating over filenames in a subdir)
  * writeFile(name, byteptr, len) - write a file to a path
  * For all methods parse filename for / path separator, create subdirectories as needed

Include logging output for key FS operations

### Use the FS class to implement FileSave/FileReset cmds on device
* Use the paths provided by the PC client, but prefix them with /fromhost/ (so that all files from host are in that subdir).
* Implement FileSave and FileReset based on the docs, using FS to do most of the work

### Extend host app/AI to allow file writing
* Add a new command line option "writefiles SRCDIR".  Which should first do a FileReset and then recursively for every file in SRCDIR do FileSave.

## Stage 15: support host driven screen creation/management — DONE

Implemented per [this plan](why-not-xml.md): the host serialises a
`touchy.Screen` protobuf per layout, uploads it with `FileSave` to
`screens/<name>.pb`, and switches to it with `ScreenLoad`. The firmware
walks the decoded message and instantiates the corresponding LVGL widgets
directly via its C API — no XML, no `lv_xml`, no `lui-xml` port required.

* Schema: `proto/touchy.proto` (`Screen`, `Widget`, `Layout`, `Style`,
  `Rect`, `Action`, plus widget kinds).
* Host DSL: [`touchy_pad.api.screens`](../app/src/touchy_pad/api/screens.py)
  (`Screen`, `button(...)`, `label(...)`, `slider(...)`, `toggle(...)`,
  `image(...)`, `arc(...)`, `spacer(...)`, layout helpers
  `absolute()` / `row()` / `col()` / `grid(cols, gap)`).
* CLI: `touchy screen push SCRIPT [--load NAME]` runs the Python file,
  collects every `Screen` instance created, uploads them, and optionally
  switches to one.
* Firmware: [`firmware/main/screens.cpp`](../firmware/main/screens.cpp).

## Stage 16: Button behaviors — DONE

Screen widgets gained *Actions* that fire when the user interacts with
them. Each clickable / changeable widget (Button, Slider, Switch,
Checkbox) carries a `repeated Action` slot (`on_click` / `on_change`),
and each `Action` is one of:

* **`ActionMacro`** — a list of `MacroStep`s replayed entirely
  device-side. Steps are: `key_down`/`key_up`/`key_tap` (HID keyboard),
  `mouse_button_down`/`mouse_button_up`/`mouse_click` (HID mouse button
  bitmask), `mouse_move {dx, dy, wheel}`, `set_delay_ms` (sticky
  inter-step delay, default 10ms), and a one-shot `delay_ms`. The macro
  runner is a dedicated FreeRTOS task (`firmware/main/macros.cpp`)
  pinned to APP CPU, fed by a queue of heap-copied
  `touchy_ActionMacro` payloads.
* **`ActionHost`** — a `uint32 code` forwarded to the host inside an
  `LvEvent.host_code`. The Python client routes events by `host_code`
  to callbacks registered via `TouchyClient.on_host_event(code, fn)`.

Other notable changes:

* **New widget: `Checkbox`** via LVGL's `lv_checkbox_create`.
* **Composite USB HID interface** with report IDs (1 = mouse,
  2 = keyboard) — single interface, single endpoint, two descriptors,
  matching what `arduino-libraries/Mouse` style stacks expect on the
  host.
* **Event mailbox endpoint:** the vendor interface adds an interrupt-IN
  endpoint at 0x85 (8 ms / 16-byte MPS) used only as a *mailbox* — the
  firmware writes a fixed encoded `Event{event_ready=true}` payload
  when the queue transitions from empty to non-empty. The host drains
  via `EventConsumeCmd` until `RESULT_NOT_FOUND`. This keeps the
  bulk pipe free for command/response traffic.
* **Protocol version bumped to 2.** `EventConsumeResponse.event` is now
  an `LvEvent` directly (no longer wrapped in `Event`), and `LvEvent`
  gained a `uint32 host_code` field.
* **nanopb static caps (deviation from the original spec):** The
  generated `touchy_Screen` struct must stay ≤ 64 KB so nanopb can
  use 16-bit field offsets. With static allocation the budget forces
  trade-offs; the current caps (in `proto/touchy.options`) are:
  `Button/Slider/Switch/Checkbox.on_* = 4`,
  `ActionMacro.steps = 16`, `Screen.widgets = 32`. Raising these
  meaningfully requires the dynamic-allocation work tracked in
  Stage 18.
* CLI: `touchy screens demo` now uploads a screen with one macro
  button (types `"hi"` over USB HID) plus three host-action widgets
  (`button`, `slider`, `checkbox`); `--listen` registers handlers and
  prints incoming events live.

## Stage 17: nanopb cleanup — DONE

Make a new protobuf.cpp/h which provides a C++ class wrapper for the generated nanopb c code.  Use the more advanced nanopb API options for dynamic memory allocation (using new/delete or malloc/free) so that screens/widgets/lists of actions can be **much** smaller normally.  Much better than all the arbitrary ints in touchy.options.  Use your judgement - for simple fields it is (very rarely) okay to just use a fixed length array.

Implemented in [`firmware/main/protobuf.h`](../firmware/main/protobuf.h) — a
header-only `PbMessage<T>` RAII wrapper that owns a nanopb-generated
struct, calls `pb_release()` in its destructor, and exposes `decode()` /
`encode()` / `clone_into()` helpers. `PB_ENABLE_MALLOC=1` was already
defined on the nanopb component, so flipping the right fields to
`FT_POINTER` in `proto/widgets.options` and `proto/touchy.options` was
enough to switch them to heap allocation.

What moved to `FT_POINTER`:

* every `repeated` widget/action/macro field
  (`Screen.widgets`, `Button.on_click`, `Slider.on_change`,
  `Switch.on_change`, `Checkbox.on_change`, `ActionMacro.steps`)
* `FileSaveCmd.data` (the 32 KB upload payload that previously sat in
  `.bss` inside `touchy_Command`).

What stayed `FT_STATIC`:

* All widget text fields (`Button.text`, `Label.text`, …): tiny, accessed
  on every LVGL builder, and a `NULL` deref check would just add noise.
* `LvEvent.user_data` / `LvEvent.state`: copied by value through the
  FreeRTOS event queue.
* `SysVersionResponse.firmware_version_str`, `FileSaveCmd.path`,
  `ScreenLoadCmd.name`: small bounded strings used in hot paths.

Consumer-side changes:

* [`host_api.cpp`](../firmware/main/host_api.cpp) now stack-allocates
  `PbMessage<touchy_Command>` / `PbMessage<touchy_Response>` per RX
  frame; the 32 KB FileSave buffer is heap-only.
* [`screens.cpp`](../firmware/main/screens.cpp) holds the active screen
  in a `std::unique_ptr<PbMessage<touchy_Screen>>`; reloads free the
  prior screen (and every nested widget / action / step array)
  automatically.
* [`macros.cpp`](../firmware/main/macros.cpp) deep-copies queued macros
  via nanopb encode + decode into a fresh `PbMessage<touchy_ActionMacro>`,
  because shallow struct copies no longer carry their `steps[]` array.

## Stage 18: Touchpad widget cleanup — DONE

The trackpad proof of concept that previously lived as a hard-coded
construction inside `app_main()` is now a first-class, host-configurable
widget that can be placed on any screen via the Python DSL, just like
buttons or sliders.

* New protobuf widgets in `proto/widgets.proto`:
    * `Trackpad` — multitouch surface; on-device touches inside the
      widget's rect become USB HID mouse events (1-finger drag → move,
      1/2/3-finger tap → left/right/middle click).
    * `LogLine` — one-line readout of the most recent device log
      message. Subscribes to a shared sink so any subsystem (currently
      the trackpad's gesture recogniser) can publish status without
      knowing where the readout is on screen.
* `firmware/main/trackpad_widget.{h,cpp}` was rewritten as a self-owning
  LVGL widget: it builds its own container, hooks LVGL's PRESSED /
  PRESSING / RELEASED events on that container (so the touch surface
  only reacts to fingers landing inside its rect), and deletes itself
  on `LV_EVENT_DELETE`. Multi-finger snapshots still come from
  `esp_lcd_touch_get_data()` since LVGL's indev abstraction only
  carries a single point.
* `firmware/main/log_line.{h,cpp}` is the new shared sink. Producers
  call `log_line_post(fmt, ...)`, which also `ESP_LOGI`s the line; an
  internal registry pushes the latest line to every live `LogLine`
  widget so multiple readouts on different screens stay in sync.
* `screens.cpp` registers builders for the two new widget tags and
  exposes `screens_set_touch(handle)` so `main.cpp` can hand it the
  GT911 handle once at boot. `main.cpp` no longer creates a trackpad
  itself — the surface only appears once a host-uploaded screen
  containing a `Trackpad` widget is loaded.
* `app/src/touchy_pad/api/screens.py` exposes matching `trackpad(...)` and
  `log_line(...)` builders, and `build_demo_screen()` was reworked to
  use absolute layout: controls on the left half, the trackpad
  occupying the right half, and a `log_line` strip along the bottom
  echoing each recognised gesture.

## Stage 19: Backlight auto-sleep + host control - DONE

Implement display backlight power management:

* **Auto-sleep** — an `esp_timer` one-shot counts down from `screen_timeout_ms`
  (default 0 = disabled).  When it fires `board_backlight_set(false)` turns the
  panel off.
* **Wake on touch** — an LVGL indev `LV_EVENT_PRESSED` callback calls
  `backlight_touch_activity()`, which turns the backlight back on and resets the
  timer.
* **Persistence** — a new `proto/preferences.proto` `PreferencesFile` message is
  stored in LittleFS at `prefs/prefs.pb` (nanopb encode/decode).  The Prefs
  singleton loads it at boot and saves whenever `set_screen_timeout_ms()` is
  called.
* **Host commands** — `ScreenWakeCmd` → `backlight_wake()`; `ScreenSleepTimeoutCmd`
  → `backlight_set_timeout(timeout_ms)` (immediately persisted to flash).
* **New files**: `firmware/main/prefs.{h,cpp}`, `firmware/main/backlight.{h,cpp}`,
  `proto/preferences.proto`, `proto/preferences.options`,
  `firmware/main/proto/preferences.pb.{c,h}`,
  `app/src/touchy_pad/_proto/preferences_pb2.py`.
* **Board API**: added `board_backlight_set(bool on)` to `board.h`, implemented
  in both board ports (GPIO for jc4827w543; CH422G IO expander for waveshare).
* **`touch.h`**: added `touch_get_indev()` to both board touch drivers.

## Stage 20: Image file support — DONE

Host-uploaded image files in `/from_host/` can now back `Image` and the
new `ImageButton` widgets. Image decoding is RGB565 BMP only — both
target boards run LVGL in `LV_COLOR_DEPTH_16`, and LVGL's BMP decoder
is zero-copy and refuses any other pixel layout.

* **sdkconfig (defaults + both boards):** added
  `CONFIG_LV_USE_FS_POSIX=y` with `LV_FS_POSIX_LETTER=70` (`'F'`) and
  `LV_FS_POSIX_PATH="/littlefs"`, plus `CONFIG_LV_USE_BMP=y`. The
  POSIX FS bridge maps `F:/...` straight onto the LittleFS mount, so
  the existing `Fs::writeFile()` path used by `FileSave` is enough —
  no separate driver registration.
* **Protobuf (`proto/widgets.proto`):** new `ImageButton` widget with
  `asset` (required), `optional pressed_asset`, and `repeated Action
  on_click`. Added as `Widget.kind.image_button = 20`. `Screen.Version.CURRENT`
  bumped to `3`; older `.pb` files are auto-deleted on boot by the
  existing version-check path in `screens.cpp`.
* **Firmware (`firmware/main/screens.cpp`):**
    * Fixed `build_image` to rebase asset paths under `F:/from_host/`
      (previously produced `F:<asset>` which couldn't resolve).
    * New `build_image_button` uses `lv_imagebutton_create` +
      `lv_imagebutton_set_src`. The PRESSED state is only set when
      `has_pressed_asset` is true on the decoded message; otherwise we
      let LVGL fall back to the released image on its own. Heap-allocated
      path strings are owned by an `ImageButtonPaths` struct attached as
      an `LV_EVENT_DELETE` callback so they're freed when the screen is
      swapped out.
* **Host DSL (`app/src/touchy_pad/api/screens.py`):** new
  `image_button(id, asset, pressed_asset=None, on_click=...)` helper.
  `pressed_asset=None` (the default) leaves the protobuf field unset
  so the firmware skips the PRESSED-state assignment.
* **Host image assets (`app/src/touchy_pad/images.py` +
  `app/src/touchy_pad/assets/`):** demo images are shipped as binary
  resources inside the wheel/sdist. `make_smiley_png()` is a one-liner
  that reads the pre-built `assets/smiley.png` (RGBA, transparent
  background) via `importlib.resources`. The host-side
  `client.file_save` then transparently converts any common source
  format (PNG, JPEG, BMP, GIF, WebP) to LVGL's native `.bin` container
  before upload, so users do not ship pre-built `.bin` files.
* **CLI (`touchy screens demo`):** uploads the smiley as
  `images/smiley.png` alongside the screen `.pb`, and the demo screen
  now includes an `image_button("smile", asset="images/smiley.png",
  on_click=host_action(0x103))` in row 4 of the grid. `--listen`
  registers a handler for `0x103`.
* **Docs:** [docs/host-api.md](host-api.md) section on the file API
  spells out the `F:/from_host/` rebase rule and the host-side image
  conversion behaviour.

Deferred: PNG/JPEG decoders on the device (`LV_USE_PNG`,
`LV_USE_LODEPNG`, etc.) — no longer needed because Stage 20.3 added
host-side conversion of arbitrary image formats to LVGL's native `.bin`
container, which the always-on built-in decoder reads directly.

## Stage 20.1: Better style support — DONE

Replace `apply_style()`'s inline `lv_obj_set_style_*` calls with proper
`lv_style_t` instances applied through `lv_obj_add_style`, so widgets
can carry different styles for different states / parts (e.g. a button
that turns blue while pressed). Wire-format bump: `Screen.Version.CURRENT`
is now `4`.

* **Proto** ([proto/widgets.proto](../proto/widgets.proto)):
  - New `LvState` enum flattens LVGL's state bits (0x0001..0x0080) and
    part bits (0x010000..0x0F0000) into a single namespace, matching the
    bit layout LVGL uses in `lv_style_selector_t` so they can be OR'd
    together in one field. (See LVGL's
    [styles overview](https://lvgl.io/docs/open/common-widget-features/styles/overview)
    for the source of truth on bit values and cascade rules.)
  - `Style` gains `uint32 for_state = 6;` — the OR'd selector. The
    proto3 default (0) means `LV_PART_MAIN | LV_STATE_DEFAULT`, i.e.
    the most common case is free.
  - `Widget.style` (singular) becomes `repeated Style styles = 3;` —
    stack as many `Style`s on one widget as you need, each with its own
    `for_state`. Tag 3 is reused; this is a wire-incompatible change.
* **Firmware** ([firmware/main/screens.cpp](../firmware/main/screens.cpp)):
  - `build_lv_style()` builds one heap-allocated `lv_style_t` per proto
    `Style`. Each populated scalar maps to exactly one
    `lv_style_set_<prop>` call (zero / unset = inherit theme).
  - `apply_styles()` loops the widget's `styles[]`, calls
    `lv_obj_add_style(obj, st, (lv_style_selector_t)s.for_state)`, and
    stashes the pointers in a `WidgetStyles` struct.
  - A `LV_EVENT_DELETE` callback (`widget_styles_delete_cb`) calls
    `lv_style_reset` + `delete` for each style when the widget is
    destroyed — same lifetime pattern already used for `ImageButton`'s
    path strings.
* **Host DSL** ([app/src/touchy_pad/api/screens.py](../app/src/touchy_pad/api/screens.py)):
  - Module-level `STATE_*` and `PART_*` constants re-export the
    `LvState` enum values for convenient OR'ing.
  - `style()` gains a `for_state=` kwarg.
  - Every widget factory's `style=` argument now accepts a single
    `Style`, an iterable of them, or `None` (single-Style stays
    backwards-compatible with stage-15 code).
* **Demo** (`build_demo_screen`): the smiley `image_button` now carries
  `style=[style(bg_color=0x1E90FF, for_state=STATE_PRESSED)]`, so a
  Dodger-blue background flashes under the smiley while it's pressed —
  a visible round-trip of state-targeted styling all the way from
  Python through protobuf to LVGL on the device.
* **Tests** ([app/tests/test_screens.py](../app/tests/test_screens.py)):
  for_state round-trip (state alone and state|part OR), single-`Style`
  auto-wrapping, list-of-`Style` ordering, and
  `Screen.Version.CURRENT == 4`.
* **Docs:** [docs/ui.md](ui.md) has a new "Styles" section walking
  through the Style ↔ `lv_style_t` mapping, selector composition and
  cascade rules, plus the smiley pressed-state example.

## Stage 20.2: Style transitions, recolor & transform — DONE

Adds the missing visual knobs needed for the LVGL-stock "press to
widen + darken" image-button look, plus a wire-level **Transition**
type so styles can animate when added / removed from a widget's
selector match. Wire-format bump: `Screen.Version.CURRENT` is now `5`.

* **Proto** ([proto/widgets.proto](../proto/widgets.proto)):
  - Every visual `Style` field is now `optional`, so explicit zeros
    (e.g. `bg_color=0x000000`, `transform_width=0`) round-trip
    faithfully instead of being indistinguishable from "unset". Only
    `for_state` stays required (default 0 = main / default).
  - New `Style` fields: `recolor`, `recolor_opa`, `transform_width`,
    and `transition` (a `Transition` sub-message).
  - New `Transition` message mirroring `lv_style_transition_dsc_t`:
    `repeated StyleProp props`, `AnimPath path`, `duration_ms`,
    `delay_ms`.
  - New curated enums `StyleProp` and `AnimPath`. Both are wire-stable
    subsets translated to the matching `LV_STYLE_*` constant /
    `lv_anim_path_*` callback at decode time, so the wire format stays
    insulated from LVGL version drift.
* **Firmware** ([firmware/main/screens.cpp](../firmware/main/screens.cpp)):
  - `build_lv_style()` now reads `has_<field>` on each scalar (instead
    of "non-zero means set") and emits the new
    `lv_style_set_image_recolor` / `_image_recolor_opa` /
    `_transform_width` calls.
  - New `build_lv_transition()` heap-allocates an
    `lv_style_transition_dsc_t` and a 0-terminated
    `lv_style_prop_t[]`, initialises the descriptor, and stashes both
    in the widget's `WidgetStyles` so they outlive the widget.
  - `WidgetStyles` and `widget_styles_delete_cb` extended to own and
    free the new transition descriptors + prop arrays alongside the
    existing styles list.
* **Host DSL** ([app/src/touchy_pad/api/screens.py](../app/src/touchy_pad/api/screens.py)):
  - `style()` gains `recolor`, `recolor_opa`, `transform_width`, and
    `transition=` kwargs. `recolor_opa` is range-checked (0..255).
  - New `transition(props=[...], path=ANIM_PATH_LINEAR,
    duration_ms=200, delay_ms=0)` helper.
  - Module-level `PROP_*` and `ANIM_PATH_*` constants re-export the
    new enums for convenient use with `transition()`.
* **Demo** (`build_demo_screen`): the smiley `image_button` now uses
  the [`lv_example_imagebutton_1`](https://github.com/lvgl/lvgl/blob/master/examples/widgets/imagebutton/lv_example_imagebutton_1.c)
  pattern — default style binds a transition over
  `(TRANSFORM_WIDTH, IMAGE_RECOLOR_OPA)`, pressed style widens by
  20 px and applies a 30 %-opaque black image-recolor. Both press and
  release animate over 200 ms linear.
* **Tests** ([app/tests/test_screens.py](../app/tests/test_screens.py)):
  explicit-zero round-trip, new scalar fields round-trip, transition
  round-trip (props order, path, durations), default-transition
  values, range check for `recolor_opa`, demo smile structural test,
  and `Screen.Version.CURRENT == 5`.
* **Docs:** [docs/ui.md](ui.md) gains a "Transitions" subsection and
  the smiley example is rewritten to the new pattern.

## Stage 20.3: Image scale/rotation + transparent host conversion — DONE

Two pieces landed together because they share the same wire-format
bump and overlap heavily on `screens.cpp`:

* **Scale & rotation in the DSL/wire format.** `Image` grew
  `optional uint32 scale` (LVGL units, 256 = 100%) and
  `optional int32 rotation` (tenths of a degree). The Python helpers
  accept human-friendly values: `scale=2.0` means 200%, `rotation=90`
  means 90°. Internally the DSL multiplies both by 256 / 10
  respectively before serialising. The firmware calls
  `lv_image_set_scale` / `lv_image_set_rotation` only when the
  corresponding `has_*` flag is set, so unscaled / unrotated assets
  cost nothing on the wire.

* **`ImageButton` refactored to embed `Image`.** The widget previously
  carried its own `asset` + `pressed_asset` strings. It now embeds
  `Image released = 1;` plus `optional Image pressed = 2;`, sharing
  scale/rotation with plain images and removing duplicated fields.
  The DSL accepts `pressed_scale` / `pressed_rotation` independently
  of `pressed_asset` — supplying any one of them auto-fills `pressed`
  using the released bitmap as the fallback source. Firmware tracks
  per-state values in an `ImageButtonState` cookie and applies them on
  `LV_EVENT_PRESSED` / `RELEASED` / `PRESS_LOST`. `Screen.Version.CURRENT`
  bumped to `6`; old `.pb` files auto-evict.

* **Transparent host-side image conversion.** `TouchyClient.file_save`
  detects BMP / PNG / JPEG / GIF / WebP magic bytes and converts the
  payload to LVGL's native `.bin` container (RGB565A8) before sending
  it to the device. The converter lives in
  `app/src/touchy_pad/lvgl_image.py` and depends on Pillow (added to
  `app/pyproject.toml` as a runtime dep). API users can keep storing
  files at intuitive paths like `images/foo.png`; the bytes on flash
  will be LVGL `.bin` payload but the firmware's always-on bin decoder
  reads them directly, so no extra config is needed. Already-converted
  `.bin` blobs and non-image data pass through unchanged.

* **Tests:** `app/tests/test_lvgl_image.py` covers magic detection,
  pass-through of existing `.bin` headers, and the RGB565A8 layout
  (header + RGB565 plane + A8 plane). `test_screens.py` updated to
  assert `Screen.Version.CURRENT == 6`.

* **Docs:** [docs/host-api.md](host-api.md) explains that BMP/PNG/JPEG
  are auto-converted on the host so API users don't need to think
  about LVGL's bin format. The device-side PNG/JPEG decoder note in
  Stage 20 is now marked unnecessary.

## Stage 20.4: Nice animations on touches to the "Trackpad" - DONE

I'd like you to come up with a fairly general/protobuf driven abstraction so that we can show a nice 'water droplet effect' where the user touches with their fingers on the touchpad.  Features:

* Extend protobufs so that there is a general/reusable set of optional 'touch', 'tap' and 'drag' animations on the Trackpad message. (Try to reuse/share/refactor existing Style/Image/Transition/Animation messages/code as much as is possible)
* Read https://lvgl.io/docs/open/main-modules/animation and https://github.com/lvgl/lvgl/blob/master/examples/anim/lv_example_anim_2.c for inspiration
* When touches/drags/taps start/stop create/remove animations as needed
* Extend the demo python code to install whatever 'first attempt' animations you think make sense

## Stage 24: Add widgets for "on-device" behaviors  ✅ done

Add a few widgets to the protobufs so that users can optionally include the following widgets in their layouts:
* FPS - just shows the current display update FPS (for profiling)

Make new subtype of Action: "ActionDevice" is analogous to the ActionHost actions, but it is for special code we want run on the device itself when that action occurs.  
The first subtype of ActionDevice is ActionSwitchScreen - it has a nested enum for Behavior (BY_NAME=0 NEXT, PREVIOUS).  Since BY_NAME is the default value, also have a name string field which will be used as the base filename (relative to /from_host/screens/NAME.pb). 

My attaching an ActionSwitchScreen to a button (with text labels like "Next" and "Previous") the demo can be enhanced.

* Extend the demo so that:
  * First screen has next and previous screen buttons in the upper left/right areas respectively, and a big touchpad region in the middle/bottom of the screen.  Also put a FPS widget in that same top row with the other buttons.  In the future we might put other buttons there
  * Second screen is various test widgets - you can move them from the current demo.  Also put next/prev screen buttons on this page - with a matching layout to the first page.  Also move the debug logging line from the old demo to the bottom of this page.
* have the device preferences store the current screen name, whenever rebooting try to restore the old previous current screen

## Stage 24.1: add layers (DONE)

* Add the concepts of Layers to the protobuf and code.  per https://lvgl.io/docs/open/main-modules/display/screen_layers (AI, you should read this page to learn)
* In particular: 
  * Move layout and widgets from Screen into this new message Layer
  * The Screen protobuf should now contain four separate Layers: Active, Top, System, Bottom.  The default layer for code usage is "Active".  Change existing code that is populating widgets/setting layouts to use that layer

Implementation summary:
* `widgets.proto`: new `Layer` message holding the `layout` oneof + `repeated Widget widgets`.  `Screen` now carries `Layer active = 7` (always present) plus `optional Layer top = 8`, `optional Layer sys = 9`, `optional Layer bottom = 10`.  Persistent layers are `optional` so unpopulated layers can be left untouched between screens.  Wire-format `Version.CURRENT` bumped 10 → 11.
* Firmware: `screen_layout.cpp` now takes a `touchy_Layer` argument; `screens.cpp` factors widget construction into `build_layer(parent, L)` and applies it to the screen object for `active`, and (only when the corresponding `has_*` flag is set) to `lv_layer_top()` / `lv_layer_sys()` / `lv_layer_bottom()` after `lv_obj_clean()`.  Unset persistent layers are explicitly *not* touched, preserving whatever the previous screen left there.  An explicit empty `Layer()` is the wire-level "clear this layer" signal.
* Python DSL (`touchy_pad.api.screens`): new `Layer` class; `Screen(name, layout=, widgets=, *, top=, sys=, bottom=)`; `add_top()` / `add_sys()` / `add_bottom()` helpers.  `Screen.layout` and `Screen.widgets` remain as back-compat properties that delegate to `Screen.active`.  Demo header (prev/next buttons) moved to the top layer.

Update docs as needed

## Stage 24.2: layout cleanup - DONE

I made a mistake on the current Layout abstraction.  They really should be treated like widgets.  Change the protobufs as follows (and then make corresponding code changes)

* The basic Layout widget contains a repeated Widget "children".
* All three of the existing layout variants should contain that Layout message
* Layer should no longer contain a Layout field, instead the three layout types should be added to the variant of acceptable Widgets in Layout.  Also remove the widgets array from Layer, instead it should just have a single "root" Widget.  (that widget will usually be one of the layout widgets)

## Stage 24.3 python api cleanup - DONE

The host-side library was refactored into a single supported public
entry point under `touchy_pad.api`:

* The internal `touchy_pad.screens` / `touchy_pad.macros` /
  `touchy_pad.hid_keys` / `touchy_pad.images` / `touchy_pad.lvgl_image`
  modules moved verbatim into `touchy_pad/api/`. The top-level
  `touchy_pad` package still exposes the lower-level
  `TouchyClient` / `Transport` / `UsbTransport` for legacy callers,
  but new code should import from `touchy_pad.api`.
* New `touchy_pad.api.device` adds `Touchy`, `touchy_open(serial=None)`,
  and `touchy_get_pad_ids()` — the lifecycle-managed facade. `Touchy`
  is a context manager that owns a background USB poller thread and
  dispatches host events to user-registered callbacks via
  `on_host_event(code, cb)`. `touchy_open()` verifies the device's
  USB protocol version against `MINIMUM_FIRMWARE_VERSION` and raises
  `IncompatibleFirmwareError` if too old.
* New `touchy_pad.api.protobuf` re-exports the generated protobuf
  classes (`Screen`, `Widget`, `Layout*`, `Action*`, …) so users
  never need to reach into the internal `_proto` module.
* `Touchy.screen_save()` accepts a host-DSL `Screen`, a raw
  `protobuf.Screen`, a JSON `dict`, or a `Path`/`str` pointing at a
  `.json` file — the higher-level alternative to writing raw `.pb`
  bytes with `file_save()`.
* The `touchy screen push` CLI subcommand (and its
  `_collect_from_script` helper) was removed. The `touchy screen
  demo` subcommand was rewritten on top of `touchy_open()` /
  `pad.screen_save()` / `pad.on_host_event()` and now uses
  `threading.Event().wait()` rather than the old
  `client.stream_events()` blocking generator.
* New `app/mkdocs.yml` + `app/docs/` skeleton + optional Poetry
  `docs` group (`mkdocs`, `mkdocs-material`, `mkdocstrings`).
  `just build-docs` writes the rendered site to `docs/python-api/`
  so it can be committed for GitHub Pages.
* New narrative `docs/python-api.md` guide covering install, screen
  authoring (DSL + raw protobuf), macros, event callbacks /
  threading model, lifecycle, and version compatibility.
* `app/tests/test_api.py` covers `Touchy` lifecycle (open / close /
  context manager), `screen_save` accepting all four input shapes,
  per-host-code callback dispatch (single + multiple handlers), and
  the firmware-too-old exception path — all against an in-process
  fake transport.

## Stage 24.4 better animations - DONE
Improved the trackpad animations.

* The touch ripple now starts **at** ``max_radius`` (full size) and
  shrinks down to a small resting radius (`max(6 px, max_radius / 4)`)
  over `duration_ms` instead of growing 0 → `max_radius`. Opacity
  stays pinned at `start_opa` for the entire touch — the ripple is no
  longer auto-deleted by an opacity tween.
* While any finger is on the trackpad, the resting ripple keeps
  following that fingertip (the existing per-frame `ctx->cx/cy`
  update is already wired into `_process()`'s drag-to-follow block).
  When a specific finger lifts (count drops below `_prev_count`), the
  newly-orphaned ripples fade out via `_fade_out_ripple()` over
  `SCROLLBAR_FADE_MS` and delete themselves on completion.
* New `optional uint32 Trackpad.scrollbar_color = 8;` field. When
  set, a thin progress bar (4 px thick) animates onto the active
  scroll axis when a two-finger scroll axis-locks: horizontal scroll
  grows a horizontal bar along the bottom edge from 0 → container
  width; vertical scroll grows a vertical bar along the right edge
  from 0 → container height. The bar fades and self-deletes on
  all-fingers-lifted.
* New `optional uint32 Trackpad.tap_max_ms = 9;` field exposes the
  previously-hardcoded `TAP_MAX_MS` (200 ms) so very long-presses can
  still register as taps when the host opts in.
* The Python builder `touchy_pad.api.trackpad(..., scrollbar_color=,
  tap_max_ms=)` plumbs both new fields through.

Tap ripples retain the legacy grow + fade-out + auto-delete behavior;
only touch ripples were re-orchestrated.

## Stage 30: device simulator
Big task - please be careful.
I'd like you to make a 'simple' simulator in python of the device side code.  It doesn't have to be exactly right or render exactly the same as the lvgl/c++ implementation.  Mainly I want a tool so I can do app development without needing my device hardware with me.

Requirements:
* has to support rendering the screens in a smallish (480x300 by default) window
* has to be 100% python (though if you use python libs that have native bits thats fine)
* Provides a bidirectional stream interface analogous to what the USB device provides for the host-api.
* Provide a new class that is a subclass of Transport (SimDeviceTransport), so it can be provided to the TouchyClient constructor (instead of the USB implementation).  if an instance of that class is created you'll need to open the gui window and get ready to start displaying screens.  Don't start the gui until that happens
* Add a --sim option to the cli that configures the touchy device enumeration so it finds this 'sim device with a fake serial number' and eventually creates a SimDeviceTransport
* definitely don't bother with/not required: any of the USB HID emulation stuff.  Exactly matching lvgl layouts, no need to do any animations or transitions.
* you don't need to copy the fancy preferences behavior of the real device or implement 'backlight' operations etc...  I really just want a basic window to see and use as if it was a screen.
* you do need to support the change screen device actions
* Since this sim device will be receving protobufs originally destined for the lvgl hardware solution it will need to keep a filesystem directory to store files

Ideas/recommendations:
* Use a popular python library for the gui rendering.  CustomTkinter?  something better? I'm new at this.
* if it makes things easier you could change Transport to have a bool property "needs_image_conversion".  The real USB based transport would set that true and the python API code would see that and know that it needs to convert image formats to the 'native' lvgl image representation before sending (current behavior).  But the sim device would return false for that flag, allowing the simulator to be easier because it could use pngs instead of raws etc...

# Stage 50: Emulate the StreamDeck API

So one end goal of this project is that I want the [StreamController](https://github.com/StreamController/StreamController) app to be able to use these cheap $15 devices as if they were SteamDeck like devices.

That app uses [this library](https://github.com/StreamController/streamcontroller-python-elgato-streamdeck) (pypi package [here](https://pypi.org/project/streamcontroller-streamdeck/)).

The API described by that package is fairly [simple](https://python-elgato-streamdeck.readthedocs.io/en/stable/) but a bit underspecified, so I'm worried if I just ask my AI to start coding up something based on it I might get garbage.  So!  I propose substages

## Stage 50.1 Reverse engineer a real StreamDeck device using the existing library

Make a new mini project in a new "reverse-engineering" sub directory.  It should contain a minimal poetry build based on the examples https://python-elgato-streamdeck.readthedocs.io/en/stable/examples/deckinfo.html and https://python-elgato-streamdeck.readthedocs.io/en/stable/examples/basic.html.  

This project should do a whole series of operations as documented in that API, but focus on outputting logging data that you will later refer to when implementing the version for our device.  In particular our device will be quite similar to this: https://python-elgato-streamdeck.readthedocs.io/en/stable/modules/devices.html#module-StreamDeck.Devices.StreamDeckOriginal

Focus on logging things you are curious about given that fairly underspecified documentation.

When you are ready to run this tool, tell me and I'll run it for you while talking to my test StreamDeck device.

Have this tool emit the logs into a datafile you can refer to later when planning stage 50.2.

## Stage 50.2 Implement our Sim StreamDeck API - DONE

**Done (May 2026).** Implemented as `touchy_pad.touchydeck`:

* `TouchyDeck(StreamDeck)` subclass at `app/src/touchy_pad/touchydeck/deck.py`
  drives a real Touchy-Pad via `TouchyClient`. Builds a grid of
  `image_button` widgets (default 5×3) whose `on_press` / `on_release`
  action slots both carry a host-code in the range `0xA000..0xA0FF`;
  the `_read_control_states` poller distinguishes the two edges via
  `LvEvent.code` (`1`=PRESSED, `8`/`32`=RELEASED/PRESS_LOST) and
  returns the StreamDeck-format snapshot the base read loop expects.
* `touchy_pad.touchydeck.install()` monkey-patches
  `StreamDeck.DeviceManager.DeviceManager.enumerate` so connected
  Touchy-Pads appear alongside real StreamDecks. Available as the
  optional extra `pip install 'touchy-pad[streamdeck]'`.

Required a small Stage 50.2 prerequisite — adding explicit
`on_press` / `on_release` action slots to `Button` / `ImageButton`
(proto fields 3,4 / 4,5; nanopb FT_POINTER) because the original
`host_action` wiring only fired on `LV_EVENT_CLICKED`. The firmware
attaches the same action list to `LV_EVENT_PRESSED`,
`LV_EVENT_RELEASED`, and `LV_EVENT_PRESS_LOST` (cancelled press still
emits release). See `proto/widgets.proto` and
`firmware/main/widget_builders.cpp`.

Original spec (kept for context):

* Put the code for this simulator/wrapper in app/src/touchy_pad/touchydeck.
* The main class in this module will be subclassing an ABC called [StreamDeck.Devices.StreamDeck.StreamDeck](https://github.com/StreamController/streamcontroller-python-elgato-streamdeck/blob/master/src/StreamDeck/Devices/StreamDeck.py).  Call our subclass TouchyDeck.
* In that subclass you'll USE the public touchypad.api to implement most of those methods.  At the very least, I want you to support sending images to the device for each of the 'buttons' making a button layout and passing presses back as if it was a StreamDeck.
* You'll need to come up with a scheme for mapping hostids (for press events) to the proper asyncio invocations as required by the StreamDeck API
* Monkey patch the DeviceManager().enumerate() method so it **also** includes any TouchyDecks connected to our machine.  Ideally you'll be able to install that monkey patch automatically on any project that has a poetry dependency on our touchy-pad package.
* Change the original reverse engineering tool made in 50.1 so that it uses our library and therefore you can modify that tool so it also dumps TouchyDecks the exact same way it dumped the original StreamDeck usage.

## Stage 51: cleanup filesystem (DONE)

Currently the host uses paths like "images/foo.bin" when writing files (for images or screens or whatever).  The device maps that name to "F:from_host/images/foo.bin" which was a bit of a mistake.  I want to change things so the host has the ability to write files to other filesystems (in addition to the current F flash filesystem I want you to make a new R ram file system)

So the HOST is now responsible for knowing which file goes on which filesystem and the host specifies full paths "R:host/images/foo.bin" etc...

Various aspects of this:
* Use "host" as the standard prefix for files from the host (rather than from_host) - it is shorter
* improve the file writing API so it starts with a name, then a series of writes, then a close - this way we can write large files without needing huge ram space.  The close can also do
an atomic rename to protect from filesystem corruption. until closed the filename can be some private temp name
* have the host provide the full path for file writes, so that it can specify drive letter (R: or F:)
* make a psram based filesytem for lvgl images.  I think this means you probably want to implement this API: https://lvgl.io/docs/open/8.2/overview/file-system.  Also I think you want it to be a subclass of our Fs class.  So we might want three classes Fs (abstract?), FlashFs, RamFS?
* The ram implementation can be very simple, just a c++ hashtable mapping to block of memory allocated for each file?  Or a list of blocks (one allocated for each FileWrite operation)?  Files written to RAM are not persisted if device reboots - they are lost
* Important note: the public python API will still maintain the illusion of a single FileSave operation, our python code will be smart enough to do the USB FileCreate, a series of FileWrites and a FileClose.
* Both the device version and our python sim device should cope with these new file paths (it is acceptable if the simulator stores 'ram' files to the regular filesystem if it makes implemention easier)
* The max block size for a FileWrite should be picked to fit in a USB bulk transfer (4KB?)
* The current screen preferences string should include the full path to the file instead of just the screen name.  The device protocol screen_load command should also provide full paths.

## Stage 52: mmap image files when possible (DONE)

* Extend our fs.h interface so that there is a get_memory_ptr(path, size_t *lenout) operation that can be applied to our two filesystem classes.  This new method will be a bit like mmap wrt use.  The flash filesystem will always return NULL to indicate not supported.  But the ram filesystem will return the base address of the bytes that make up that file.
* Extend our Image widget image load code:
  * try to use our Fs classes to get an in-memory base pointer for our image file.  If we can get such a ptr and the file format EXACTLY matches our display format (for our current displays that is probably RGB565 but you should ask lvgl) build up an lv_image_dsc_t based on the file header and a ptr to the rest of the bytes.  This will avoid most of the 'file copy overhead for images'
  * For any other case (image wasn't from R: or the file format was not an exact match), print a WARN log message to the device log saying why the image couldn't be direct mapped.  Then just use the standard file read API that is already working.

Implementation notes:

* `Fs::peek()` (already used by `lv_fs_drv`) was promoted to a virtual
  method on the `Fs` base, defaulting to `nullptr` (FlashFs has no
  in-memory representation). `RamFs::peek()` returns the existing
  PSRAM-resident pointer + length, so we reuse one entry point instead
  of inventing a parallel `get_memory_ptr` API.
* New helper `try_mmap_image()` in `firmware/main/widgets/image_mmap.cpp`
  walks the wire path through `fs_peek()`, validates that the on-disk
  `lv_image_header_t.cf` matches `LV_COLOR_FORMAT_NATIVE` (currently
  `RGB565` for the 16bpp display build), and populates an
  `lv_image_dsc_t` whose `data` pointer aliases directly into RamFs.
  On any mismatch it returns a short human-readable reason and the
  caller logs an `ESP_LOGW`.
* `apply_image_attrs` (plain image widget) and `build_image_button`
  (released + pressed assets) both try the fast path first and fall
  back to the file-read decoder via `lv_image_set_src(img, "F:host/…")`.
  Heap-allocated `lv_image_dsc_t`s are owned by the widget and released
  via `LV_EVENT_DELETE` callbacks.
* The LVGL `lv_image_header_t` layout is sniffed directly from the
  bundled `lvgl__lvgl` component; if LVGL is ever bumped, both the host
  encoder and this helper get updated in lockstep so layouts stay
  consistent.
* Defers a known hazard to Stage 55: overwriting an in-use Ram file
  while the LVGL widget tree still references the aliased pointer.
  Today the host doesn't do that mid-session.

## Stage 53: prefer native formats (DONE)

In our python function that generates LVGL image files (in native format), highly prefer RGB565 as the output format.  Only fall back to an alpha channel supporting format if the source image file includes alpha channel data.  If you must do such fallback print a WARN log message to the python log.

Implementation notes:

* `touchy_pad.api.lvgl_image.to_lvgl_bin()` now defaults to `cf=None`
  ("auto"): it inspects the source image's alpha channel and emits
  RGB565 whenever every pixel is fully opaque, falling back to
  RGB565A8 only when at least one pixel has `alpha < 255`.
* "Has an alpha channel" is interpreted strictly — many editors save
  opaque artwork as PNG-RGBA, and we don't want to penalise those
  with the slow path. We check the actual min-alpha via
  `Image.getextrema()`.
* On fallback the converter logs a single `logging.WARNING` on the
  `touchy_pad.api.lvgl_image` logger so callers can see exactly which
  asset missed the Stage 52 mmap fast path.
* `cf="RGB565"` and `cf="RGB565A8"` are still accepted as explicit
  overrides for callers that want deterministic output.

## Stage 54: Allow optionally storing Widget data in files (DONE)

Added a `WidgetRef { string path = 1; }` message in `proto/widgets.proto`
and a new `widget_ref = 33` variant in `Widget.kind`. A widget tree
node carrying a ref is read from the device filesystem at screen-load
time and the decoded widget is spliced inline in place of the ref.
`Screen.Version.CURRENT` bumped 13 → 14; backwards-compat intentionally
not preserved (firmware deletes mismatched `.pb` files as before).

Host changes:

* New DSL helper `widget_ref(path: str)` in
  `touchy_pad.api.screens` — emits a `Widget(widget_ref=...)`. The
  helper rejects empty paths and refuses to accept `id`/`rect`/`cell`/
  `style` kwargs (those belong to the *referenced* widget).
* `Touchy.widget_save(name, widget, *, drive="F")` writes the bare
  `Widget` to `{drive}:host/widgets/{name}.pb`. Convention: `F:` for
  persistent flash, `R:` for the volatile PSRAM ramdisk.
* `widget_save` deliberately mirrors the existing `screen_save` path
  shape rather than sharing a wrapper — `Screen` carries a version
  field that we validate, `Widget` does not. *(Fixed in Stage 56 —
  see below.)*

Firmware changes (`firmware/main/widgets/widget_builders.{h,cpp}` +
`screens.cpp`):

* `resolve_widget_ref()` reads `{drive}:host/widgets/<name>.pb` via
  `Fs::readBinary`, decodes a `PbMessage<touchy_Widget>` into a
  newly-constructed holder, and returns the inner `touchy_Widget *`.
  Decoded holders are parked in a per-build pending vector. Cycles
  (e.g. `a.pb → b.pb → a.pb`) are caught via an `unordered_set` of
  in-progress paths and logged at `ESP_LOGE` before short-circuiting.
* `widget_build_children` and `widget_build_layer` resolve refs at
  the dispatch boundary, so the *inner* widget's `rect`/`style`/`cell`
  attributes are the ones applied (and the outer `widget_ref` Widget
  carries no styling on the wire — see DSL guard above).
* `screens.cpp::load_decoded()` calls `widget_refs_reset_pending()`
  before building and `widget_refs_commit()` immediately after the
  old LVGL screen is freed — same ordering as the active-screen swap
  so old action-slot pointers remain valid until their owning
  widgets are deleted.

Open follow-ups deferred to Stage 55:

* ~~Refs are resolved once at screen-load time. Rewriting a referenced
  widget file via host upload does **not** trigger a redraw — the new
  bytes only take effect on the next `Screen_load`.~~ Resolved in
  Stage 55 via `screens_notify_file_changed`, which reloads the
  active screen whenever any referenced file (image, widget-ref, or
  the screen itself) is overwritten.

## Stage 55: minimize redraws (DONE)

The Stage 54 implementation reloaded the whole screen on every host
file commit, which was both expensive and visually noisy when the
host streamed unrelated uploads (e.g. the next twenty TouchyDeck
icons while a settings screen was on display). Stage 55 makes
redraws conditional on whether the changed file actually feeds the
currently-displayed widget tree.

Shipped:

* **Field rename + drop (wire-format bump 14 → 15)**: `Image.asset` →
  `Image.path` for consistency with `WidgetRef.path`. `Screen.name`
  was briefly renamed to `Screen.path` but then removed entirely:
  the firmware never reads that field from the decoded proto (it
  tracks the active path separately as `g_current_path`), and the
  host derives the upload destination from the DSL `Screen.name`
  attribute directly. Dropping the field keeps the wire format
  lean and removes a potential source of confusion. Wire-format
  version bumped to 15; the firmware deletes any `.pb` carrying an
  older version on next boot.
* **`screens_notify_file_changed(path)`** (firmware/main/screens.h):
  called by `host_api.cpp` after every successful `FileClose(commit=true)`,
  alongside the existing `screens_register_from_file` hook. Walks the
  currently-decoded `touchy_Screen` (active + the three persistent
  layers) plus the Stage 54 active-`WidgetRef` path list looking for a
  reference to `path`. On a hit (or when `path == g_current_path`)
  it kicks `screens_load(g_current_path)` to rebuild the active
  screen against the now-updated on-disk bytes. On a miss it returns
  silently — the common case when the host is bulk-uploading
  unrelated assets.
* **Reload granularity**: we deliberately do *not* try per-image
  cache-drop + `lv_obj_invalidate` here. The mapping from
  `touchy_Widget *` to its `lv_obj_t *` isn't built by the current
  builders (each builder allocates its own LVGL object pointer and
  doesn't surface it), and a single screen-rebuild is fast enough on
  the S3 (≈ tens of ms) that the saved work isn't worth the extra
  state. The thing that mattered — *not* reloading on uploads that
  don't affect the screen — is now in place.
* **Threading**: the notify path runs on whichever task processed the
  USB command (today the host_api task). `screens_load` already takes
  `lvgl_port_lock()` internally, which is the same lock LVGL's timer
  handler honours, so the rebuild is serialised against rendering
  exactly the way an explicit `Screen_load` is.
* **PSRAM cache-flush in `RamFs::closeWrite`** (firmware/main/fs/ram_fs.cpp):
  when ``CONFIG_SPIRAM`` is set we `esp_cache_msync(…, DIR_C2M)` the
  freshly-committed bytes before publishing the entry. Today's
  consumers (LVGL `fread` + Stage 52 mmap) are CPU-only and the data
  cache would serve them transparently, but we add the msync as a
  future-proofing measure for DMA decoders.
* **Simulator**: the sim re-renders the whole canvas on every screen
  update (it does that anyway), so no per-file change tracking was
  added there. Performance isn't a goal in the sim.

Not done (deferred):

* Per-image invalidation (`lv_image_cache_drop` + `lv_obj_invalidate`)
  in place of a full reload when only `Image` / `ImageButton` assets
  change. Would need either a widget→LVGL-object side-channel map
  built during `widget_build_layer`, or `lv_obj_set_user_data` tagging
  on every built object (which collides with trackpad's existing
  user-data use). Cheap follow-up if a real workload reveals the
  full-reload cost matters.

## Stage 56: widget tweak (DONE)

In Stage 54 we noted that `widget_save` mirrored `screen_save` but
couldn't validate a wire-format version because only `Screen` carried
the field. Stage 56 fixes that asymmetry by moving the version onto
`Widget` (only the *root* widget of any `.pb` file carries it), so
both file types are validated identically.

Shipped (wire-format bump 15 → 16):

* **`proto/widgets.proto`** — added `Widget.Version` enum and
  `Widget.version` field (tag 6). Removed `Screen.version`; tag 6 on
  `Screen` is now `reserved`. For screen files the version lives on
  `screen.active.version`; for standalone widget files
  (`{F,R}:host/widgets/<name>.pb`) the top-level `Widget` carries it.
  Nested children always leave it at `VERSION_UNKNOWN`.
* **Host** — `Screen.to_proto()` (DSL) now stamps
  `msg.active.version = CURRENT` instead of `msg.version`.
  `Touchy.widget_save()` deep-copies the supplied widget and stamps
  the version on the copy before serialising, so callers never have
  to think about it. `proto/default_screen.json` moved its `version`
  into the `active` block.
* **Firmware** — `screens.cpp::screens_register_from_file` validates
  `screen.active.version`; `widget_builders.cpp::resolve_widget_ref`
  validates the decoded widget's `version` and deletes the file from
  the underlying `Fs` on mismatch (same delete-and-let-the-host-retry
  pattern as screen files). Both messages log at WARN with the bad
  version number.
* **Tests** — `test_screen_save_accepts_dict` /
  `test_screen_save_accepts_json_path` now pass
  `{"active": {"version": "CURRENT"}}`; the existing
  `test_widget_ref_serialises_inside_screen` asserts
  `msg.active.version == Widget.Version.CURRENT`.

No backwards-compat path: older `.pb` files are deleted on first boot
after upgrade, exactly as for prior wire-format bumps.

## Stage 57: Change ActionSwitchScreen to ActionChangeWidgetRef (DONE)

Now that we have WidgetRefs we can be smarter about the idea of Screens and sublayouts.  

* Change ActionSwitchScreen into ActionChangeWidgetRef.  It should also have a "target_id" which is the id of the WidgetRef it is going to change.  The path inside this action will always be used.  
If by_name we it should be a full path to a widget pb file.  
If next/prev it should be the DIRECTORY name where we look for the next/previous widget file (vs the current value of the targeted widget ref).  If the current widget ref value is not found in that directory just pick the first file in that dir.  Otherwise print a warning to the console
* Changes made to WidgetRefs by this feature DO NOT GET SAVED BACK TO FS - just change the in-ram state as needed

Because we now have widgetrefs and they are changable by actions you can make the "screen demo" cleaner.  Rather than switching screens when the user goes next prev (with a hardwired set of buttons in the top row).  You can instead make the filesystem something like:
* host/screens/demo.pb - contains a bar at the top with prev/next widgets and the whole bottom initially has a widgetref pointing to...
* host/w/trackpad.pb - contains the former demo's trackpad widget
* host/w/test.pb - contains the former demo's test widgets

note: the prior convention I picked of host/widgets for a dir name should instead be host/w (for brevity)

(I don't care about backwards compatibility - just bump the widet file version #)

# Stage 58: We've been assigned USB VID 0x303A, PID 0x8369
Per https://github.com/espressif/usb-pids/commit/d32920ad0aacb4e6ab4188d6c351afeec0db0d2f we've been assigned USB IDs!

* Add a Constants enum to touchy.proto, use this for any misc constants that are necessary for clients that want to talk using this protocol (It is nice to store constants in the protobuf because it is implicitly language/platform agnostic)
* Add USB_VID, USB_PID to that enum.  Use it in the C++ and python code.  Update docs if needed.

**Done.** `Constants` enum added to `touchy.proto` with `USB_VID = 0x303A` and `USB_PID = 0x8369`. `usb_hid.cpp` now uses `touchy_Constants_USB_VID/PID`; `usb_ids.py` reads from the generated `touchy_pb2.Constants`.

## Stage 59: improve animation support

* Read existing docs and widgets.proto.
* Improve the protobufs so they could describe an animation like the example in tools/reference/anim.c (i.e. the host could use our protobufs to fully specify such an animation on a device screen)
* enhance the "screen demo" so that a red dot animation similar to that c code is running behind the rest of the widgets on the test page.  (lvgl draws later widgets on top of earlier widgets)

**Done.** Extended the proto schema with five new style-prop enum
values (`STYLE_PROP_X`/`Y`/`WIDTH`/`HEIGHT`/`OPA`), a new `AnimTrack`
message (`prop` + `start`/`end` ints — `from`/`to` are Python keywords)
and a top-level `Animation` message carrying N parallel tracks plus the
full LVGL knob set: `duration_ms`, `path` (re-uses the existing
`AnimPath` enum), `repeat_count` (0 → `LV_ANIM_REPEAT_INFINITE`),
`repeat_delay_ms`, `reverse` + `reverse_delay_ms` +
`reverse_duration_ms`, and `start_delay_ms`. `Widget.animations` is
now a `repeated Animation` heap-alloc field, bumping
`Widget.Version.CURRENT` to **18**.

Firmware grew a single new `widgets/widget_animations.{h,cpp}` pair:
`apply_animations()` allocates one `lv_anim_t` per track, plugs in a
shared `anim_style_exec_cb` that calls `lv_obj_set_local_style_prop`
with the right `lv_style_prop_t`, and tracks the live `AnimCtx*`s on
the widget so a custom delete-cb cancels animations via
`lv_anim_delete()` and frees the ctx storage when the widget is
destroyed. `widget_styles` got two free helpers
(`lv_prop_from_proto` / `lv_path_from_proto`) reused by both the
animation builder and the transition builder. `apply_animations` is
called immediately after `apply_styles` in all three widget code-paths
(ordinary, image, layout-widget).

Python DSL adds `anim_track(prop, start, end)`, `animation(*tracks, …)`,
and five `PROP_X`/`PROP_Y`/`PROP_WIDTH`/`PROP_HEIGHT`/`PROP_OPA`
constants. Every widget factory grew an `animations=` kwarg routed
through the existing `_widget()` helper (alongside `style=`). The
Stage-57 demo `test` page is now wrapped in an outer absolute layer
that overlays a red-radius spacer with a 1 s ease-in-out
`anim_track(PROP_X, …) + WIDTH + HEIGHT` ping-pong animation —
matching the spirit of `tools/reference/anim.c` while exercising the
new pipeline end-to-end.

The PySide6 sim mirrors the firmware in Qt: a per-widget
`_apply_animations()` builds one `QParallelAnimationGroup` per cycle
(forward + optional reverse leg), wraps it in a
`QSequentialAnimationGroup` when `start_delay_ms` / `repeat_delay_ms`
are non-zero, sets `loopCount(-1)` for infinite repeats, and connects
each track's `valueChanged` to `move()` / `resize()` /
`QGraphicsOpacityEffect.setOpacity()` based on the track's `StyleProp`.
`QEasingCurve.Type.Steps` doesn't exist, so `ANIM_PATH_STEP` collapses
to `Linear` in the sim (good-enough preview).

## Stage 60: streamdeck-probe — DONE

Extended `streamdeck-probe` to stress-test the TouchyDeck press/release
round-trip and image-update throughput, and extended the existing
`test_touchydeck.py` suite with two new end-to-end tests.

### What was done

**`tools/streamdeck-probe/src/streamdeck_probe/probe.py`**
- Added `_tile_flipped(deck, label)` — produces a 180°-rotated (`ROTATE_180`)
  version of the standard labelled tile in the deck's native image format.
- Extended `_probe_callbacks`: pre-builds `normal_tiles` and `flipped_tiles`
  caches (one entry per key) before registering callbacks. Inside `on_key`:
  on press → uploads `flipped_tiles[key]` via `set_key_image`; on release →
  restores `normal_tiles[key]`. Both calls are wrapped in `log.timed(...)` so
  the JSONL carries per-event latency. On exit from the interactive phase, all
  keys are restored to their normal tiles.
- Works against both the headless sim (`--sim-headless`) and real USB hardware
  — no new CLI flags needed; the flip runs whenever `--interactive` is on.

**`app/tests/test_touchydeck.py`** (extended — suite already existed)
- `test_touchydeck_set_key_image_writes_match_image_button_path`: verifies
  that `set_key_image(k, data)` writes to the exact `F:/R:host/...` path that
  the layout builder encodes in the `ImageButton.released.path` field.
- `test_touchydeck_press_flip_roundtrip_via_sim`: full E2E — injects synthetic
  press/release `HostEvent`s for two keys via `SimDevice.push_host_event`,
  calls `set_key_image` inside the callback, and asserts that the sim fs blob
  matches `white` after each press and `black` after each release.

**No firmware changes required.** `Button.on_release` already fires host
events (implemented in Stage 50.2 and wired by the TouchyDeck layout builder);
the device delivers a matching release `HostEvent` via `LV_EVENT_RELEASED` and
`LV_EVENT_PRESS_LOST`.

## Stage 61: Beginning Rust API ✓ DONE

I'm a complete noob at Rust, please be extra clear in the plan so that we can discuss tradeoffs in archtecture/etc...

* Make a new Rust API library (for eventual use in our rust based 'opendeck' plugin device in stage 62).
* Refer to docs/python-api for inspiration but use idiomatic rust styles/documentation standards/directory paths.  Feel free to change API as needed to make it more 'rust like'.  In particular the threading/notification model for reading host-event messages from the device may be different.
* Note: the python api library included a 'dsl' for building screens etc and the ability to pass in JSON for screen/widget files.  I don't think either of those were a great idea.  Instead have the API be smaller with the user passing in built up protobufs (using a standard rust protobuf library)
* Use the 'standard'/conventional rust build steps to generate protobuf glue based on our /proto files.  
* Also note: the protobuf files include the PID/VID you need for your USB device enumeration for your 'find touchpads' operation. 
* Implement standard rust documentation generation for the library
* Configure the build for the rust API so that it can eventually go on standard rust library repo servers (whatever those are?)
* Include test cases (kinda analogous to the Touchydeck )
* Make a new docs/rust-api.md describing this new API to rust developers.  Put a README.md in the root dir of this new source tree with instructions on how to build.
* Update devcontainer and justfile as needed for "rust-build"
* the current python code is in /src, I'm not sure the best place to put this new sister rust code.  You might even want to move /src to a better name (though that sounds painful)
* Make a small text only rust app to exercise this API with real hardware.  It should be similar to the python "screen demo" cli test, create a few widgets (using protobufs), and write some screen/widget/image files to the device and if --listen, print log messages when the user clicks on buttons etc...

### Architecture decisions

After surveying `tools/OpenDeck` (the eventual downstream consumer):

* **`rust/` workspace** — sister to `app/`. Cargo workspace with two members:
  `touchy-pad` (library, published to crates.io) and `touchy-demo`
  (binary, not published). Python `app/` stays where it is.
* **Async-first via `tokio`** — OpenDeck is fully async (`AsyncStreamDeck`,
  `tokio::sync::RwLock<HashMap<...>>` patterns). A sync-only library would
  feel foreign there. Public surface uses `async fn`; the event channel
  is a `tokio::sync::mpsc::Receiver<LvEvent>`.
* **Protobuf codegen: `prost` + `prost-build`** — modern, used by
  the tonic / hyper ecosystem, integrates with `build.rs` so every
  `cargo build` regenerates if `proto/*.proto` changes (analogue of
  `just build-proto`). Generated types live in `OUT_DIR` and are
  re-exported under `touchy_pad::proto`.
* **USB: `nusb`** — pure-Rust, no libusb dependency, works on
  Windows/macOS/Linux without a native install step. (OpenDeck uses
  `hidapi`, but that's HID-only — we use the vendor-class bulk pair so
  we can't share that choice.)
* **Image conversion is in scope for v0.1**: a `touchy_pad::images`
  module decodes PNG/JPEG/BMP/GIF/WebP via the `image` crate (same
  crate OpenDeck uses) and emits LVGL 9 `.bin` blobs (RGB565 + optional
  RGB565A8 plane). Ported from `app/src/touchy_pad/api/lvgl_image.py`.
* **Error type: `thiserror`** in the library (typed `TouchyError` enum
  so downstream code can match on variants); the demo binary uses
  `anyhow` (matches OpenDeck's app-level convention).
* **Edition 2024**, `rustfmt.toml` with `hard_tabs = true,
  max_width = 200` to match OpenDeck's style.
* **No simulator transport in Rust v0.1.** Tests use an in-memory mock
  `Transport`. Cross-language sim integration is deferred: the Python
  simulator (Stage 30) will eventually expose a TCP / WebSocket server
  so any-language client (Rust, Go, …) can run against it without a USB
  device — tracked under Stage 80 / development-environment work.

### Directory layout

```
rust/
  Cargo.toml                # workspace
  rustfmt.toml              # hard_tabs, max_width=200
  README.md                 # build / test / run-demo
  touchy-pad/
    Cargo.toml              # publishable crate metadata
    build.rs                # runs prost-build over ../../proto/*.proto
    src/
      lib.rs                # crate docs + public re-exports
      error.rs              # TouchyError + Result alias
      transport.rs          # async Transport trait + framing
      transport_usb.rs      # nusb-backed impl
      client.rs             # low-level RPC (file_save, screen_save, …)
      pad.rs                # high-level Touchy: open, events, on_event
      images.rs             # PNG/JPEG/… -> LVGL bin (auto RGB565 / RGB565A8)
    tests/
      framing.rs
      client_loopback.rs    # mock Transport, mirrors test_client.py
      pad_events.rs         # event channel + drop-join
      images.rs             # round-trip PNG -> bin -> header check
  touchy-demo/
    Cargo.toml
    src/main.rs             # clap-driven: info | demo | listen
    assets/                 # tiny pre-converted .bin tiles
```

### Public API (sketch)

```rust
use touchy_pad::{Touchy, Result};
use touchy_pad::proto::{Screen, Widget, widget::Kind, ImageButton, Image};

#[tokio::main]
async fn main() -> Result<()> {
    let pad = Touchy::open().await?;
    pad.file_save("R:host/btn0.bin", &tile_bytes).await?;
    pad.screen_save("F:host/demo.pb", &Screen {
        widgets: vec![Widget {
            id: "btn0".into(),
            kind: Some(Kind::ImageButton(ImageButton {
                released: Some(Image { path: "R:host/btn0.bin".into(), ..Default::default() }),
                ..Default::default()
            })),
            ..Default::default()
        }],
        ..Default::default()
    }).await?;
    pad.screen_load("F:host/demo.pb").await?;
    let mut events = pad.events();
    while let Some(evt) = events.recv().await {
        println!("{evt:?}");
    }
    Ok(())
}
```

### Justfile additions

```
rust-build:   cd rust && cargo build --workspace
rust-test:    cd rust && cargo test --workspace
rust-lint:    cd rust && cargo fmt --check && cargo clippy --workspace -- -D warnings
rust-doc:     cd rust && cargo doc --workspace --no-deps
rust-run *A:  cd rust && cargo run -p touchy-demo -- {{A}}
```

### Devcontainer

Add `ghcr.io/devcontainers/features/rust:1` to install rustup + cargo
+ clippy + rustfmt. Existing udev rules already cover device access.

### Out of scope

* StreamDeck-compat shim (Stage 62, OpenDeck plugin).
* TCP/WebSocket sim bridge (Stage 80).
* Async-feature-flag-toggle to sync (the lib is async-only; sync
  callers can use `tokio::runtime::Runtime::block_on`).
* `cargo publish` to crates.io — the metadata will be ready but we
  hold off shipping until the API has stabilised.

## Someday
* Support running a sim on the linux host?
* Use https://lvgl.io/docs/open/debugging/gdb_plugin to faciltiate debugging

## Stage 62: OpenDeck plugin ✓ DONE

[OpenDeck](https://github.com/nekename/OpenDeck) is a Rust-based
alternative to StreamController for driving StreamDeck-like devices.
Per `docs/opendeck.md`, plugins are out-of-process executables that
talk to the OpenDeck app over a WebSocket using the **OpenAction**
protocol; each plugin is then responsible for talking to its own
hardware. Our `touchy-pad` Rust crate (stage 61) gives us that piece
for free, so the plugin is mostly an OpenAction-side adapter.

### Plan (subject to revision)

#### 1. Reference example as submodule

Add `4ndv/opendeck-akp153` (recommended by the OpenDeck author and the
plugin the OpenAction `device_plugin` module was designed around) as a
read-only submodule at `tools/reference/opendeck-akp153`. We won't
build it — just read it for structural cues (manifest layout, packaging,
the `register_device` / `handle_set_image` call shape, hot-plug loop).

`git submodule add https://github.com/4ndv/opendeck-akp153.git
tools/reference/opendeck-akp153` and pin to the latest tag (`v0.9.5` at
the time of writing).

#### 2. Crate location

New crate at `rust/touchy-opendeck/` as a third workspace member
alongside `touchy-pad` and `touchy-demo`. Rationale:

* Shares `Cargo.lock` / rustfmt / clippy config with the rest of the
  Rust workspace.
* Lets the plugin depend on `touchy-pad` via a path dep (no
  duplicate-version drift, no `cargo publish` round-trip during
  development).
* Same hard-tabs/`edition = 2024` style; same `just rust-build` /
  `just rust-lint` recipes pick it up automatically.

`publish = false` — the plugin ships as a packaged `.zip` for
"Install from file", not via crates.io.

```
rust/touchy-opendeck/
├── Cargo.toml          # publish = false
├── README.md           # install / packaging / known issues
├── manifest.json       # OpenDeck plugin manifest (id, version, exec name, supported devices)
├── 99-touchy-pad.rules # symlinked to ../../bin/99-touchy-pad.rules
├── assets/
│   └── icon.png        # plugin icon shown in OpenDeck UI
├── src/
│   ├── main.rs         # openaction::run + register/unregister loop
│   ├── device.rs       # per-device state + OpenAction → touchy-pad bridge
│   ├── layout.rs       # key-index ↔ Touchy-pad widget grid mapping
│   └── hotplug.rs      # nusb hotplug watcher (re-uses crate::transport_usb::enumerate)
└── justfile            # local `just package` recipe (zip up release binary + manifest)
```

Top-level [Justfile](Justfile) gets `opendeck-build` / `opendeck-package`
recipes that delegate to the crate's justfile so contributors can stay
at the repo root.

#### 3. OpenAction wiring

Use `openaction = "2"` with normal caret-semver — minor bumps
(currently 2.6.0, looks healthy at 2.x) come along automatically; we
only pin if a breaking change forces it. All new code in this crate is
licensed **GPL-3.0-or-later**, matching the rest of the workspace and
the akp153 reference plugin.

Sketch of `main.rs`:

```rust
#[tokio::main]
async fn main() -> openaction::OpenActionResult<()> {
    env_logger::init();
    let plugin = TouchyPlugin::new();
    openaction::run(plugin).await
}
```

`TouchyPlugin` implements the OpenAction device-plugin trait surface
with roughly:

| OpenAction event | Our handler |
|---|---|
| Plugin startup | `nusb::watch_devices()` → enumerate existing Touchy-Pads, call `register_device(id, name, kind, rows, cols)` for each. |
| `handle_set_image(device_id, key, image_bytes)` | **Use the touchy-pad Rust API**: `pad.file_save("R:host/opendeck/{dev}/key_{k}.png", &image_bytes).await?` (host auto-converts to LVGL `.bin`); debounce a `pad.screen_load(...)` so adjacent set-image calls coalesce into one screen reload (≈100 ms window — same trick TouchyDeck uses to avoid flashing the whole grid on every key change). |
| `handle_set_brightness(device_id, percent)` | Map 0–100% to a backlight RPC (`screen_sleep_timeout` for now; add a dedicated `BacklightSet` command in firmware stage 62.1 if needed). |
| Hot-plug attach | Build a Touchy-pad screen with an `rows × cols` `ImageButton` grid; each button's `ActionHost{host_code = HOST_CODE_BASE + key_index}` is what surfaces as a key press to OpenDeck. |
| Hot-plug detach | `unregister_device(id)`; drop the per-device `Touchy`; let `Drop` cancel its event poller. |
| `Touchy::events()` → `LvEvent` with `host_code` | Translate to `openaction::device_plugin::key_down(device_id, key)` + `key_up(device_id, key)` calls. (Hold/release semantics already correct as of stage 60 fix.) |

##### Initial screen build (mirrors TouchyDeck)

The Python `touchy_pad.touchydeck.layout` module already solves this
problem for StreamController; we port the same layout verbatim into the
Rust plugin so the on-device behaviour is identical:

* `SCREEN_PATH = "R:host/screens/opendeck_{device_id}.pb"` — per-device
  screen file in PSRAM (rewritten on every connect, no flash wear).
* `HOST_CODE_BASE = 0xB000` for the OpenDeck plugin (TouchyDeck uses
  `0xA000`, so a single board can host both without code clashes).
* `asset_path_for(k) = "R:host/opendeck/{device_id}/key_{k}.bin"`.
* `build_screen(cols, rows)` builds a `Screen` with a `Grid` layout and
  one `ImageButton` per cell wired to `on_press = on_release =
  HOST_CODE_BASE + k`.

Per-device sequence on attach:
1. `Touchy::open()` the device.
2. Build the `Screen` proto in-memory via the touchy-pad Rust API
   (the same primitives `touchy-demo` already exercises:
   `ImageButton`, `ActionHost`, `LayoutGrid`).
3. `pad.file_save(SCREEN_PATH, encoded_screen).await?` — uses the
   existing host→device protobuf bytes path.
4. `pad.client().screen_load(SCREEN_PATH).await?` to make it live.
5. Spawn the event loop: `pad.events().await` → `LvEvent` → OpenDeck
   `key_down`/`key_up`.

Pressed/released animation: leave to OpenDeck. We don't try to do
"set pressed image" tricks — OpenDeck already redraws via
`handle_set_image` when its own pressed state changes.

This means the plugin's hot path is **purely**
`Touchy::file_save` + `Client::screen_load` + `Touchy::events`; we do
not poke USB directly. If any image-format or framing concern arises,
the fix belongs in `touchy-pad` so the Python and Rust paths both
benefit.

#### 4. Device identity & re-connect

OpenDeck plugins identify devices by a stable string. Our board
exposes a name but not (currently) a unique serial; for the first cut
use `format!("touchy:{bus}-{addr}")` so multiple connected pads work
distinctly, and document the limitation in the plugin README. A
firmware-side `serial` field in `SysBoardInfoResponse` is a clean
follow-up if anybody actually has two units.

#### 5. Layout discovery

For now hard-code a 3×5 grid (matching the screenshot demo in
stage 61) but read `display_width` / `display_height` from
`SysBoardInfoResponse` so larger boards (`waveshare_s3_lcd_7b`) can
opt into a different shape via a per-device config saved to OpenDeck
global settings. The grid layout itself is built using the Python
`touchy_pad.api.screens.grid` DSL ported to Rust — or, simpler for v1,
hand-rolled `LayoutAbsolute` arithmetic since `touchy-demo` already
demonstrates that path.

#### 6. Packaging

* Cross-compile release binaries for linux-x64, win-x64-gnu, macos
  (`just opendeck-package`).
* Zip layout per OpenDeck convention: `manifest.json` + executable +
  `assets/` at the root.
* Publish via GitHub Releases initially; once stable, submit to
  `marketplace.tacto.live` (the OpenDeck plugin marketplace) alongside
  the akp153 reference.

#### 7. Documentation

* `rust/touchy-opendeck/README.md` — install, udev rules, supported
  boards, troubleshooting.
* New `docs/opendeck-plugin.md` — design rationale + Stage 62 acceptance
  criteria (kept separate from `docs/opendeck.md` which stays as the
  research notes).
* Link from `docs/README.md`.

### Out of scope for Stage 62

* Encoder / dial support (no rotary inputs on current boards).
* Layer/profile UI inside OpenDeck — we just expose buttons; everything
  beyond a flat key grid is OpenDeck's responsibility.
* Multitouch / trackpad routing into OpenDeck (no obvious mapping;
  defer to a later stage if anyone asks).
* Windows/macOS hot-plug — start with Linux-only event reaction; other
  OSes fall back to enumerate-on-start.

### Acceptance

1. `git submodule update --init tools/reference/opendeck-akp153` works.
2. `just rust-build` builds the `touchy-opendeck` crate.
3. `just rust-test` passes including a unit test for the
   key-index↔widget-name mapping and an `openaction`-mocked
   `handle_set_image` round-trip.
4. With OpenDeck running and a Touchy-Pad plugged in:
   * device appears in OpenDeck's device list,
   * setting an image on a key shows up on the touchpad,
   * pressing the touch shows up as a key event in OpenDeck.
5. Plugin survives unplug/replug without restarting OpenDeck.

## Stage 63: Make the simulator "wire accurate"

The Stage 30 simulator is great for fast Python unit tests but it
cheats in two ways that increasingly hurt as the Rust client and the
StreamController/OpenDeck plugins grow up:

1. The host talks to it via a Python `Queue[bytes]` pair — no framing,
   no socket, so it can't be reached from the Rust client, and it
   doesn't exercise the same code paths the real USB transport uses.
2. It accepts source-format image bytes (PNG/JPG/etc.) and decodes
   them with Pillow, so the host-side LVGL `.bin` conversion is
   never exercised end-to-end. That hid a real bug once already
   (see Stage 24.x notes on image pipeline parity).

Stage 63 makes the simulator behave as much like the real device as
possible:

* Same protobuf messages.
* Same length-prefixed framing as the USB bulk endpoint pair.
* Same image format (raw LVGL `.bin` — Pillow decode goes away on the
  sim side).
* Reachable over a real socket so the Rust client and any out-of-
  process tool (StreamController inside Flatpak, OpenDeck, a fuzzer)
  can talk to it the same way they'd talk to hardware.

This also lets the GUI run *outside* the devcontainer (Qt/Wayland is
fiddly inside containers) while clients inside the container connect
to it over TCP.

> No backwards compatibility is preserved. Existing tests that imported
> `SimDeviceTransport` directly will be updated to use the new entry
> points.

### Plan (subject to revision)

#### 1. Wire format

* **Transport: plain TCP**, loopback by default. Loopback works the
  same from host shell and from inside the devcontainer (via
  `host.docker.internal` / `--add-host=host.docker.internal:host-gateway`),
  needs zero setup on Windows/macOS, and is trivial to mock in tests.
* **Port: `8935`**, exposed as a symbolic constant
  `touchy_pad.transport_net.DEFAULT_SIM_PORT` (Python) and
  `touchy_pad::transport_net::DEFAULT_SIM_PORT` (Rust). Picked because
  it's well clear of common dev ports and doesn't collide with any
  IANA assignment we care about.
* **Framing: identical to USB bulk.** Reuse `_pack` / `_unpack` from
  `touchy_pad.transport` verbatim — 4-byte little-endian length prefix
  followed by the nanopb payload. The same `_MAX_FRAME` cap applies.
* **Channels:** two logical channels (host→device commands,
  device→host responses) multiplexed on a single TCP connection.
  Direction is implicit from who's sending. Events stay request/
  response: clients still poll via `EventConsumeCmd` exactly as they
  do today over USB, so no separate "mailbox" channel is needed.
* **Connection lifecycle:** one client at a time, mirroring USB
  exclusivity. The server `accept()`s a single connection, processes
  it to completion (client `close` or socket EOF), then accepts the
  next. Concurrent connect attempts get a clean `"sim busy"` framed
  error and disconnect.
* **No auth / no TLS.** Loopback only by default; binding to
  `0.0.0.0` is opt-in via `--bind`, with a startup warning.

#### 2. Server: new `touchy simulator` subcommand

A new top-level CLI subcommand that runs only the simulator, in its
own process:

```bash
touchy simulator                # GUI window + TCP listener on 8935
touchy simulator --headless     # no GUI, just listener (CI / SSH)
touchy simulator --port 9000    # override port
touchy simulator --bind 0.0.0.0 # opt-in non-loopback (prints warning)
touchy simulator --serial SIM1  # override pseudo-USB serial (cache dir)
```

Implementation lands in `app/src/touchy_pad/sim/server.py`:

* `SimServer` wraps the existing `SimDevice` (Stage 30) + a `socket`
  listener thread. Per-connection worker thread reads framed commands,
  dispatches to `SimDevice`, writes framed responses.
* In GUI mode, `SimWindow` runs on the Qt main thread; the network
  worker uses `QMetaObject.invokeMethod` (or a `queue.Queue` drained
  by a `QTimer`) to marshal screen changes onto the UI thread, exactly
  the way the in-process sim does today.
* Graceful shutdown on SIGINT / window close.

#### 3. Client: new `TcpTransport`

`app/src/touchy_pad/transport_net.py`:

```python
class TcpTransport(Transport):
    needs_image_conversion = True   # same as USB — host converts PNG → .bin
    def __init__(self, host: str = "127.0.0.1",
                 port: int = DEFAULT_SIM_PORT,
                 timeout_ms: int = 5000) -> None: ...
```

`send_command` / `recv_response` use the existing `_pack` / `_unpack`
helpers — literally the same bytes that would have gone on the USB
bulk endpoint. A short connect retry/backoff covers the "client
starts before server" race common in tests.

Rust side: `rust/touchy-pad/src/transport_net.rs` exposing
`TcpTransport` implementing the existing `Transport` trait. Same
framing, same port constant.

#### 4. CLI flag rework

Replace the current `--sim` / `--sim-headless` pair on the root
`touchy` command with three mutually exclusive flags, all of which
go through the new TCP path so every code path exercises the same
framing:

| Flag | Behaviour |
|------|-----------|
| `--sim-remote [host:port]` | Don't start a sim. Connect a `TcpTransport` to an already-running `touchy simulator`. Default `127.0.0.1:8935`. |

##### `TOUCHY_SIM_URL` env var

If `TOUCHY_SIM_URL` is set (e.g. `tcp://host.docker.internal:8935`),
**both Python and Rust clients prefer it over USB enumeration** when
no explicit transport is selected. The lookup order becomes:

1. Explicit CLI flag / API argument (`--sim-remote host:port`,
   `Touchy::open_tcp(addr)`, an explicit `UsbTransport(...)`, etc.).
2. `TOUCHY_SIM_URL` if set → `TcpTransport`.
3. USB enumeration (current default).

This is what makes the sim usable from *any* host-side consumer
without code changes — the Rust OpenDeck plugin, the StreamController
shim, ad-hoc scripts — by just exporting one env var in the shell
that launches them. Explicit flags always win so tests are
deterministic.
| `--sim-headless` | Spawn an in-process `SimServer(headless=True)` on an ephemeral loopback port, connect `TcpTransport` to it, tear down on exit. Replaces today's in-process queue-backed sim for tests and CI. |
| `--sim-gui` | Same as `--sim-headless` but with `headless=False` so a Qt window opens in the same process. Equivalent of today's `--sim`. |

The current `--sim` flag is removed outright (no backwards-compat
alias). `--sim-size`, `--sim-serial`, `--sim-dir` remain — they
configure the embedded `SimServer` in `--sim-headless` / `--sim-gui`
mode and are ignored (with a warning) under `--sim-remote`.

#### 5. Image pipeline

The big behavioural change: the sim now consumes raw LVGL `.bin`
exactly like the firmware does. Consequences:

* Drop `SimDeviceTransport.needs_image_conversion = False`; the new
  network transport advertises `True`, so the existing host-side
  PNG→`.bin` conversion in `touchy_pad.api.images` is exercised
  end-to-end on every sim run.
* `SimDevice`'s widget renderer learns to load `.bin` headers (LVGL
  v9 image-descriptor format) and decode the underlying pixel data
  for display in the Qt window. The Pillow code path stays only as
  a fallback for tests that explicitly want to bypass conversion.
* On-disk storage: `.bin` files live under the existing sim cache
  root (`~/.cache/touchy-pad/<serial>/F/...` for LittleFS, `.../R/...`
  for the PSRAM ramdisk), one file per host path. No format change
  required — the bytes on disk just happen to be LVGL `.bin` now
  instead of PNG.

#### 6. Tests

* `tests/test_transport_net.py` — `TcpTransport` round-trip against
  a `SimServer` started in a pytest fixture (headless, ephemeral
  port).
* All existing tests that build a `make_tempdir_transport()` switch
  to the new fixture; the in-process `SimDeviceTransport` shim is
  deleted.
* `tests/test_sim_window.py` keeps using the embedded `--sim-gui`
  path so the Qt-side smoke tests still cover screen rendering.
* New Rust integration test under `rust/touchy-pad/tests/` that
  `Command::new("touchy")`-spawns `touchy simulator --headless`,
  connects with the Rust `TcpTransport`, drives a screen, and
  asserts events round-trip.

#### 7. Documentation

* Rewrite `docs/simulator.md` for the new flag set, including the
  "run the GUI on your host, connect from inside the devcontainer"
  workflow (TCP `host.docker.internal:8935`).
* Add a short paragraph to `docs/host-api.md` noting that the wire
  format is now identical between USB and the sim's TCP transport.
* `docs/development.md`: mention `touchy simulator` as the
  recommended way to develop GUI screens without flashing.

### Out of scope for Stage 63

* TLS / authentication — loopback only by default; explicit opt-in
  to `--bind 0.0.0.0` with a printed warning.
* Multi-client support — one connection at a time, like USB.
* mDNS / Bonjour discovery — env var + CLI flag are enough.
* Hot-attach event mailbox — clients keep polling `EventConsumeCmd`.
  A push channel is a possible later optimisation.
* Persisting `.bin` cache across sim restarts in any structured way
  beyond the existing cache-dir layout.

### Acceptance

1. `touchy simulator --headless` listens on `127.0.0.1:8935` and
   serves at least one client through a full screen-load + event
   round-trip.
2. `touchy --sim-remote` (with the above running) drives the sim and
   shows identical behaviour to today's `--sim-headless`.
3. `touchy --sim-gui` opens a Qt window and the in-process listener
   accepts the embedded client.
4. `just app-test` passes; the Pillow→`.bin` round-trip is exercised
   by at least one test against a real `TcpTransport`.
5. Rust `cargo test -p touchy-pad` includes a spawn-the-simulator
   integration test that succeeds on Linux CI.
6. `docs/simulator.md` documents the host-GUI / container-client
   workflow and the new flags.


## Stage 64.1: tunnel device serial logs via our protocol - DONE

Some future devices don't have a 'real' USB port so we will want to tunnel our current cdcacm based serial logging over our existing protobuf protocol.

* If CONFIG_TOUCHY_LOG_OVER_PROTO is set in the build try to push any log message over our protobuf channel.  Change the current boards to set this build flag.
* Add a new log message variant to the Response protobuf.
  * It should include a string and a priority.  If priority is not set assume TRACE level priority.  Other supported levels TRACE, DEBUG, INFO, WARN, ERROR.
  * Add a num_dropped field, which is normally not populated, but if the device had to drop messages (because the queue was full or we were printing from an isr or somesuch the next successful message will include num_dropped)
* Hook the standard esp-idf/freertos logging/printing so that it can try to send log messages via this mechanism.
* Be careful about reentrancy, if we are already somehow processing a log and we get asked to queue a log again, just drop that extra log and bump up the next time we send num_dropped.  This prevents nasty problems if we try to emit log messages in our logging/usb/protobuf code.
* Change the python/rust code to emit host side logs with that same priority for any logs it receives from the client.

### Plan

The wire protocol today is strict request/response on the vendor bulk
pair: the device only ever speaks when spoken to (events are drained
by polling `EventConsumeCmd`). Log records reuse that exact same
channel — we **do not** add a separate poll command. Instead the
device's response to an `EventConsumeCmd` is widened to carry either
an event, a log record, or nothing. The firmware queues log records
into a ring buffer; the host's existing event-poll loop drains them
as a side effect. This keeps the transport state machine unchanged
(no unsolicited frames, no new endpoint, no new command) and reuses
the existing length-prefixed nanopb framing on both USB and the
Stage 63 TCP simulator transport.

Bump `Screen.Version.CURRENT` from 5 → 6 (or add a new
`SysBoardInfoResponse.ProtocolVersion` bump, whichever the existing
"current protocol version" sentinel turns out to be — Stage 13 used
`TOUCHY_PROTOCOL_VERSION` in `host_api.cpp`). Hosts older than the new
version simply ignore (or fail to decode) the new
`Response.log_record` oneof variant and the device keeps cycling
through its log queue until the records age out, so the change is
backwards compatible at the framing level.

#### 1. Protobuf schema (`proto/touchy.proto` + `widgets.proto` unaffected)

* New enum `LogPriority`:

  ```proto
  enum LogPriority {
      LOG_PRIORITY_TRACE = 0;   // default when field unset
      LOG_PRIORITY_DEBUG = 1;
      LOG_PRIORITY_INFO  = 2;
      LOG_PRIORITY_WARN  = 3;
      LOG_PRIORITY_ERROR = 4;
  }
  ```

  Order matches `esp_log_level_t` (NONE/ERROR/WARN/INFO/DEBUG/VERBOSE)
  conceptually inverted so the protobuf default (0) is the safe
  "noisy but harmless" TRACE level the spec calls for.

* New `LogRecord` message — no new command. The host already polls
  `EventConsumeCmd` on a timer; the device's response to that poll
  is now allowed to carry **either** an event **or** a log record
  (whichever the device pops off whichever queue first). The host
  keeps polling at its existing cadence and dispatches on
  `Response.which_payload`:

  ```proto
  message LogRecord {
      LogPriority priority    = 1;   // unset == TRACE
      string      message     = 2;   // pre-formatted line, no trailing \n
      uint32      num_dropped = 3;   // records dropped since last successful send
      uint64      timestamp_us = 4;  // esp_timer_get_time() at enqueue; 0 on host sim
      string      tag         = 5;   // ESP_LOG TAG (best-effort, may be empty)
  }
  ```

* Extend the existing `Response.payload` oneof with
  `LogRecord log_record = N` alongside the existing `event_consume`
  variant (pick the next free tag number; remember
  `proto/touchy.options` needs matching nanopb size hints —
  e.g. `LogRecord.message max_size:160`, `LogRecord.tag
  max_size:16`). No change to `Command` — `EventConsumeCmd` is now
  the universal "give me whatever the device has queued" poll.

* Drain priority on the device: when handling `EventConsumeCmd`,
  the dispatcher checks the event queue first (preserves today's
  semantics and keeps UI events responsive), then falls back to the
  log queue, then returns `RESULT_NOT_FOUND` if both are empty.
  Events therefore never starve under a log flood; logs catch up
  during idle frames between widget activity, which matches the
  human-debugging use case.

* Nanopb `.options` budget per record: 160 bytes of message text is
  enough for typical ESP_LOG lines without bloating the static TX
  buffer. Truncate longer lines on the firmware side and append `…`.

#### 2. Firmware: ring buffer + log hook (`firmware/main/log_proto.{h,cpp}`)

New translation unit so `host_api.cpp` and `main.cpp` stay thin.

* Compile-time gate: wrap the entire implementation in
  `#if CONFIG_TOUCHY_LOG_OVER_PROTO`. Add a Kconfig entry under
  `firmware/main/Kconfig.projbuild` (create if missing):

  ```kconfig
  config TOUCHY_LOG_OVER_PROTO
      bool "Tunnel ESP-IDF logs to host over the touchy protobuf channel"
      default y
      help
          When enabled, esp_log_set_vprintf() is hooked so log lines
          are queued for the host instead of (or in addition to) the
          UART. Required for boards without a real USB-CDC port.
  ```

  Set `CONFIG_TOUCHY_LOG_OVER_PROTO=y` in
  `firmware/sdkconfig.jc4827w543` and
  `firmware/sdkconfig.waveshare_s3_lcd_7b` (and the Stage 64.2
  `sdkconfig.cyd2usb` once that lands).

* Public API (matches the existing `host_api_*` style):

  ```cpp
  void log_proto_start(void);                    // call from main()
  bool log_proto_pop(touchy_LogRecord *out);     // host_api drains
  void log_proto_emit(LogPriority p,             // optional public emit
                      const char *tag,
                      const char *msg);
  ```

* Storage: FreeRTOS queue of fixed-size records
  (`struct { uint8_t priority; uint64_t ts_us; uint32_t dropped; char tag[16]; char msg[160]; }`,
  ~200 bytes × 32 = 6.4 KiB) created in `log_proto_start()`. Using a
  queue (not a stream buffer) lets us push from ISR context via
  `xQueueSendFromISR()` cleanly.

* Hook into the ESP-IDF logger:

  ```cpp
  static vprintf_like_t s_prev_vprintf;
  static int touchy_vprintf(const char *fmt, va_list ap);

  void log_proto_start(void) {
      s_prev_vprintf = esp_log_set_vprintf(touchy_vprintf);
      ...
  }
  ```

  `touchy_vprintf` formats once with `vsnprintf` into a task-local
  buffer, parses the leading `"X (timestamp) TAG: "` prefix that
  ESP-IDF prepends to derive priority + tag, enqueues a `LogRecord`,
  and *also* forwards to `s_prev_vprintf` so the UART log still
  works (gated by another Kconfig, see below). This way nothing else
  in the codebase has to change to start emitting tunneled logs.

* Reentrancy + ISR safety:

  * A thread-local (`__thread`) `bool s_in_emit` flag is set on
    entry; if already set, increment a `pending_dropped` counter and
    return immediately. This catches the
    `host_api → vendor_write → ESP_LOGE → log_proto_emit → host_api`
    loop cleanly.
  * `xPortInIsrContext()` short-circuits to
    `xQueueSendFromISR(..., &hpw)`; on failure bump
    `pending_dropped` and bail. **Never** call `vsnprintf` from the
    raw `printf` redirect when in ISR — but the only ISR path that
    can hit us is firmware code that already calls
    `ESP_EARLY_LOGx`, which is also routed through `vprintf` only
    when explicitly enabled; document the limitation rather than
    fight it.
  * `pending_dropped` (atomic uint32) is folded into the next
    successful enqueue's `num_dropped` field, then reset to zero.
    The host therefore sees the cumulative loss bucketed onto the
    next surviving record, matching the spec.

* Add a `CONFIG_TOUCHY_LOG_TO_UART` Kconfig (default `y` for the
  jc4827/waveshare boards which have CDC-ACM, default `n` for
  Stage 64.2 cyd2usb). When unset, `touchy_vprintf` skips the
  `s_prev_vprintf` forward.

#### 3. Firmware: dispatcher integration (`firmware/main/host_api.cpp`)

* Extend the existing `case touchy_Command_event_consume_tag:`
  branch in `dispatch()` to drain events first, then logs:

  ```cpp
  touchy_LvEvent evt;
  if (xQueueReceive(s_evt_queue, &evt, 0) == pdTRUE) {
      resp->code          = touchy_ResultCode_RESULT_OK;
      resp->which_payload = touchy_Response_event_consume_tag;
      resp->payload.event_consume.has_event = true;
      resp->payload.event_consume.event    = evt;
      break;
  }
  touchy_LogRecord rec;
  if (log_proto_pop(&rec)) {
      resp->code          = touchy_ResultCode_RESULT_OK;
      resp->which_payload = touchy_Response_log_record_tag;
      resp->payload.log_record = rec;
      break;
  }
  resp->code = touchy_ResultCode_RESULT_NOT_FOUND;
  ```

* Because `LogRecord.message` is a `FT_POINTER` field (per
  `touchy.options`), use the existing `PbMessage<>` RAII pattern so
  the encoded TX buffer copies the string and then frees on scope
  exit.

* `host_api_start()` calls `log_proto_start()` before creating the
  dispatcher task — that way the very first `ESP_LOGI(TAG,
  "host_api dispatcher started")` line is already tunneled.

#### 4. Host (Python) — `app/src/touchy_pad/`

* `client.py`:

  * `event_consume()` grows a return-type union: it now hands back
    either an `LvEvent`, a `LogRecord`, or `None`. Existing callers
    that only care about events keep working — the helper that
    drives `stream_events()` ignores `LogRecord` payloads and the
    `LogRecord` branch is forwarded to the log pump instead
    (single source of truth: the existing event-poll thread).
  * No new `log_consume()` RPC and no separate `stream_logs()`
    poll thread. The existing event pump (today: daemon thread that
    calls `event_consume()` on a timer) gains a small `if isinstance
    (item, LogRecord): _dispatch_log(item)` branch.
  * Backwards compat for direct users of `event_consume()`: keep the
    legacy single-value return shape but additionally surface
    `LogRecord` via a new low-level `poll(self) -> LvEvent |
    LogRecord | None` and rewrite `event_consume()` to call `poll()`
    and skip records (logging them on the side). Callers that
    relied on the old name see no behaviour change.
  * The pump forwards each record to a dedicated `logging.Logger`
    named `touchy_pad.device` with the mapped level:

    | proto level | Python level         |
    |-------------|----------------------|
    | TRACE       | `logging.DEBUG - 5`  | (or just `DEBUG`; stdlib has no TRACE — go with `DEBUG` and put it on a `touchy_pad.device.trace` child logger so callers can silence) |
    | DEBUG       | `logging.DEBUG`      |
    | INFO        | `logging.INFO`       |
    | WARN        | `logging.WARNING`    |
    | ERROR       | `logging.ERROR`      |

  * When `record.num_dropped > 0`, emit a `WARNING` "device dropped
    N log records" line on `touchy_pad.device` *before* the record
    itself so the dropped count is impossible to miss in scrollback.
  * Tag is added via `extra={"device_tag": record.tag}` and a custom
    format string `"[%(device_tag)s] %(message)s"` on the default
    handler installed by `touchy_open()`.

* `cli.py`: hidden `--device-log-level {trace,debug,info,warn,error}`
  flag (default `info`) wired into the log pump on every subcommand
  that opens a device. `touchy logs` subcommand: open the device,
  start the pump, and block on `Ctrl-C` — pure convenience wrapper.

* `transport.py` / `sim/`: no changes (records ride the existing
  bulk pair / TCP socket).

* `api/touchy.py` (or wherever `touchy_open()` lives): start the log
  pump automatically; expose a `with_device_logs=False` opt-out for
  programmatic callers that want raw control.

#### 5. Host (Rust) — `rust/touchy-pad/`

* Regenerate prost bindings from `proto/touchy.proto` (or its
  vendored copy under `rust/touchy-pad/proto/`). The existing
  `event_consume()` method's return type widens to an enum
  (`PollItem::Event(LvEvent) | PollItem::Log(LogRecord) | None`);
  the public `events()` stream continues to yield `LvEvent` only
  and shuttles `LogRecord` items into the log pump.
* Wire `LogRecord` into the `log` crate facade with the matching
  level mapping (trace/debug/info/warn/error map 1:1).
* Provide a `Client::start_log_pump()` returning a `JoinHandle` so
  callers can shut it down; default `touchy_pad` examples start it
  automatically. Internally it shares the same event-poll thread
  introduced by Stage 16 — no second polling loop.
* No `tokio` dependency: a `std::thread` pump is sufficient and
  matches the Python design.

#### 6. Simulator (`firmware`-less but transports-the-same)

The simulator's Stage 63 TCP transport already carries the same
`Command`/`Response` frames, so `EventConsumeCmd` arrives at
`app/src/touchy_pad/sim/` exactly like any other RPC. The sim's
handler is extended to also return `LogRecord` payloads (in the
events-first, logs-second order described above). A synthetic
bridge feeds the simulator's own `logging.getLogger
("touchy_pad.sim.device")` records into a `collections.deque
(maxlen=…)` consumed by the same handler, so a user running
`touchy --sim-remote` sees the *same* log lines they'd see on real
hardware. Implementation: a `logging.Handler` that pushes formatted
records onto the deque.

#### 7. Tests (`app/tests/`)

* `test_logs.py`:
  * Stand up a sim with the new log bridge, emit a handful of
    `logging.{debug,info,error}` calls, drive the event poll, and
    assert priority mapping + tag round-trip on the `LogRecord`
    payloads.
  * Mixed traffic: enqueue an event *and* a log on the sim; assert
    the event arrives first and the log on the next poll —
    documents the events-first drain order.
  * Overflow case: monkeypatch the deque `maxlen` to 2, push 5
    records, drain, assert the *next surviving record* carries
    `num_dropped == 3`.
  * Reentrancy: confirm a logger handler invoked from inside the
    pump itself doesn't recurse (sentinel pattern test).
* Extend `test_client.py` to exercise the log path against the
  sim (events-first ordering + log dispatch into
  `touchy_pad.device`).
* Extend `test_transport_net.py` only if the framing changes (it
  shouldn't).

#### 8. Documentation

* `docs/host-api.md`: new "Log tunneling" section documenting the
  reuse of `EventConsumeCmd`, the new `Response.log_record` variant,
  the priority enum, the dropped-record convention, and the
  `CONFIG_TOUCHY_LOG_OVER_PROTO` build flag.
* `docs/development.md`: short paragraph on `touchy logs` and the
  `--device-log-level` flag.
* `AGENTS.md`: bump the "latest active wire-format" line (Stage 13
  paragraph) to the new `Screen.Version.CURRENT` / protocol
  version.

### Out of scope for Stage 64.1

* Batched log drain (returning multiple records per
  `EventConsumeCmd` response) — v1 firmware always returns at most
  one record per poll. Add later if profiling shows the round-trip
  cost matters; the schema can grow a `repeated LogRecord` variant
  without breaking the single-record path.
* Pre-mounted ESP-IDF early log lines (anything emitted before
  `log_proto_start()` runs) — these continue to go to the UART
  only. We can revisit by buffering in a static ring before the
  queue exists.
* Filtering by tag on the device side — the host gets every record
  and filters in Python's `logging` config.
* `printf` (non-`ESP_LOGx`) capture — `esp_log_set_vprintf` already
  catches plain `printf` on ESP-IDF, but we don't promise it works
  for code that bypasses the logger.

### Acceptance

1. `proto/touchy.proto` builds via `just build-proto`; the C and
   Python bindings include `LogRecord` and `LogPriority`, and
   `Response.payload` gains the `log_record` oneof variant.
2. `just firmware-build` succeeds for both `jc4827w543` and
   `waveshare_s3_lcd_7b` with `CONFIG_TOUCHY_LOG_OVER_PROTO=y`, and
   ESP_LOG lines emitted from the dispatcher arrive at the host via
   the existing event-poll loop with the correct priority + tag.
3. With the queue artificially flooded, the first record received
   after recovery carries a non-zero `num_dropped`, and a host
   `WARNING` log line is emitted.
4. Re-entrant log calls (e.g. `ESP_LOGE` from inside the host_api
   write path) do not crash, deadlock, or recurse — they're
   silently dropped and counted.
5. `just app-test` adds and passes `test_logs.py` covering priority
   mapping, dropped-counter folding, and reentrancy.
6. `cargo test -p touchy-pad` covers the Rust log pump against the
   simulator (`touchy simulator --headless`).
7. `docs/host-api.md` documents the reused `EventConsumeCmd` path
   and the priority semantics.

## Stage 64.2: Make CDCACM optional - DONE
* If CONFIG_TINYUSB_CDC_COUNT is zero, do not create cdcacm devices in usb_hid.  This will help us save endpoints in our device (so we can eventually use one for interrupt notification).  This will also work well with our stage 64.1 added ability to send our logs over our private protobuf based endpoints.
* Change our sdkconfig to set that # of CDCs to zero

Implementation notes:

* `firmware/sdkconfig.defaults` now sets `CONFIG_TINYUSB_CDC_ENABLED=n`
  (which causes the Kconfig-generated `CONFIG_TINYUSB_CDC_COUNT` to
  vanish; `esp_tinyusb`'s `tusb_config.h` then falls back to
  `CONFIG_TINYUSB_CDC_COUNT 0`, so `CFG_TUD_CDC == 0`). The
  `firmware/sdkconfig.jc4827w543` save-defconfig snapshot was updated
  to match (`# CONFIG_TINYUSB_CDC_ENABLED is not set`).
* `firmware/main/usb_hid.cpp`:
  * All CDC-only code (`tinyusb_cdc_acm.h` / `tinyusb_console.h`
    includes, the `tinyusb_config_cdcacm_t` local, the
    `tinyusb_cdcacm_init` / `tinyusb_console_init` calls, the
    `ITF_NUM_CDC*` enumerators and `TUD_CDC_DESCRIPTOR` row) is now
    gated on `#if CFG_TUD_CDC`. The CDC string-descriptor slot stays
    in `s_string_desc` (harmless when unused; keeps the indices for
    HID/vendor stable).
  * The `EPNUM_*` `#define`s were replaced with a single conditional
    `enum` so the endpoint numbering compacts when CDC is off:
    `EPNUM_HID=0x81, EPNUM_VENDOR_OUT=0x02, EPNUM_VENDOR_IN=0x82`.
    EPs 3 and 4 are now free for a future interrupt-IN event mailbox.
  * Startup `ESP_LOGI` now reports `"HID + vendor"` or
    `"CDC-ACM + HID + vendor"` depending on the build.
* Device log output continues to reach the host via Stage 64.1's
  `LogRecord` tunnel (`CONFIG_TOUCHY_LOG_OVER_PROTO=y` in defaults)
  plus UART0 (`CONFIG_TOUCHY_LOG_TO_UART=y`). USB CDC-ACM is no
  longer a runtime log sink. Re-enabling CDC for ad-hoc debugging
  is a one-line per-board override
  (`CONFIG_TINYUSB_CDC_ENABLED=y`).

## Stage 64.3: Allow our protobuf based protocol over serial ports

**Status: done.** Self-synchronising frame (`MAGIC | LEN(u16) | payload |
CRC8`) is live on every transport; `ProtocolVersion.CURRENT == 5`. Host
Python `SerialTransport` (pyserial) + `touchy --port`, Rust
`SerialTransport` behind the `serial` cargo feature, firmware
`HostApiLink` abstraction gated on `CONFIG_TOUCHY_PROTO_OVER_SERIAL`
(default n). Validated: 143 Python tests, 11 Rust framing tests, firmware
builds with the flag both off and on.

Though we still want to include support (for some boards) for using our custom USB endpoint based transport of our protobuf based protocol - we want to ADD support for optionally running that protocol over a conventional serial port.

* Use the same 'wire encoding' we used in stage 63 for our TCP based simulator protocol link.  Try to share code.  i.e. on the python/rust side a new sibling class to TCPTransport (which shares much of the code via a common baseclass)
* If the python CLI tool doesn't already have a --port /dev/foo/blah option add one.  If that option is set that tells the API library to use the 'serial' transport via the specified 'uart like' device.  No need to do any other hw probing in this case.
* Add a build kconfig flag to our ESP-IDF firmware TOUCHY_PROTO_OVER_SERIAL.
If set, include appropriate device side code to do this. 
* To facilitate testing this on the jc4827w543 board (and only that board) set this new flag and CONFIG_TINYUSB_CDC_ENABLED (so that we can temporarily use that board for testing the feature - I'll turn these flag off later)

### Plan (subject to revision)

#### 0. Why this matters

The vendor-class bulk transport (Stages 13+) is great but requires a
device with native USB-OTG *and* a host that can claim a custom vendor
interface (libusb / WinUSB). The forthcoming CYD2USB board (Stage 65)
is plain ESP32 with **no native USB at all** — its only host link is a
UART (through the CH340/CP210x bridge that also handles flashing). To
reach those boards the same protobuf protocol must ride a byte-stream
serial port. We get there in two halves: the host transport (this
stage), and a firmware link backend (this stage, validated on
jc4827w543 over USB-CDC so we don't need CYD2USB hardware in hand yet).

#### 1. Wire format — new self-synchronising frame (all transports)

Stage 64.3 replaces the bare `LEN(u32) | payload` frame with a
self-synchronising frame used by **every** transport (USB bulk, TCP
sim, and serial), so there's a single `_pack`/`_unpack` everywhere and
the serial reader can recover sync after corruption or boot noise:

```
  MAGIC(2) | LEN(u16 LE) | payload | CRC8(1)
```

* **MAGIC** — a fixed 2-byte sentinel (e.g. `0xA5 0x5A`) marking a
  frame start; the resync scan anchor.
* **LEN** — u16 little-endian payload length. Two bytes is plenty: the
  largest message is a `FileWriteCmd`, already capped at ~4 KiB to bound
  device buffers, so payloads never approach the 64 KiB ceiling. This
  shrinks the prefix from 4 → 2 bytes and makes an over-cap length
  impossible to even express.
* **CRC8** — a single-byte CRC (e.g. CRC-8 poly `0x07`, table-free)
  over `LEN || payload`. Deliberately cheap: corruption on USB/TCP is
  already caught by lower layers, and even on a real UART the odds are
  low.

Overhead is 5 bytes/frame (was 4). The on-wire size cap is now bounded
by the u16 length; keep a logical `_MAX_FRAME` matching the ~4 KiB
FileWrite budget + framing.

**Resync algorithm** (in the shared stream reader; effectively a no-op
on USB/TCP, essential on serial): scan bytes until `MAGIC` is seen, read
`LEN`, read `LEN` payload bytes + the CRC8, verify; on mismatch discard
and resume scanning one byte past the candidate magic. The host's first
round-trip (`sys_board_info_get`) re-establishes sync, and we tolerate
losing the very first frame after open (covered by the existing
open-time retry in `touchy_open`).

**Serial-specific consequence — the protocol port must be log-free.**
A freshly-opened serial port may carry boot noise (ESP-IDF console
banner, DTR/RTS line glitches), and any `ESP_LOG` bytes mixed into the
protocol stream would corrupt frames. Device logs instead reach the
host in-band via the Stage 64.1 `LogRecord` tunnel — exactly why 64.1
exists. See the firmware section for keeping the console off the
protocol port.

**This is a wire-incompatible change:** bump
`SysBoardInfoResponse.ProtocolVersion` (`CURRENT` 4 → 5) and the
firmware's `TOUCHY_PROTOCOL_VERSION`. Because the framing itself
changes, there's no negotiation path — an old host literally cannot
parse a new device's frames (and vice-versa); it's a hard cutover,
consistent with the project's "no backwards-compat preserved" stance.

* **No auth / no TLS** (same as USB/TCP — physical link).

#### 2. Host side — shared base class

Today `TcpTransport` (Python) and `TcpTransport` (Rust) each inline the
framing read/write over their stream object (`socket` / `TcpStream`).
Refactor so the (now magic + CRC) framing logic lives once and the
byte-stream is pluggable:

* **Python** (`app/src/touchy_pad/transport.py` or a small new
  `transport_stream.py`): extract a `_StreamFramedTransport(Transport)`
  base implementing `send_command` / `recv_response` / `_recv_exact` in
  terms of two abstract primitives:
  * `_write_all(data: bytes) -> None`
  * `_read_some(n: int) -> bytes` (returns `b""` on EOF)

  The base owns the shared `_pack`/`_unpack` (magic + u16 len + CRC8)
  and the magic-scan resync loop, so every transport inherits
  corruption recovery for free. `TcpTransport` becomes a thin subclass
  binding those to the socket.
  New `SerialTransport(port: str, *, timeout_ms=...)` in
  `transport_serial.py` binds them to a `serial.Serial` at a fixed
  115200 baud. `needs_image_conversion = True` (host converts
  PNG → LVGL `.bin`, same as USB/TCP). Thread-safety via the same
  `threading.Lock` pattern `TcpTransport` already uses.
* **Dependency:** add `pyserial` to `app/pyproject.toml`. Import it
  lazily inside `SerialTransport.__init__` so a missing pyserial only
  errors when `--port` is actually used (mirrors the pyusb/libusb
  guarding already done for Windows CI).
* **Rust** (`rust/touchy-pad/src/transport_serial.rs`): mirror with a
  `SerialTransport` over `tokio-serial` (`SerialStream` implements
  `AsyncRead`/`AsyncWrite`, so it slots straight into the existing
  framing helpers). Gate the `tokio-serial` dep behind a cargo feature
  (e.g. `serial`) so the dependency stays optional. Lower priority than
  the Python path; land Python first.

#### 3. CLI

* A `--port PATH` option already exists on the `cli` group, currently
  documented as "serial port for esptool-based commands (`update`)".
  **Extend its meaning** rather than add a second flag: when `--port`
  is set *and* no `--sim*` mode is active, the protocol commands open a
  `SerialTransport` on that path instead of auto-discovering USB. The
  `update` / flash paths keep using the same string for esptool (a
  device's flashing port and protocol port are usually the same
  `/dev/ttyACM*` / `/dev/ttyUSB*`).
* No baud option — the serial transport always uses 115200 (the device
  side uses the same fixed rate).
* Plumbing: in the group callback stash `ctx.obj["serial_port"]`;
  `_make_transport()` returns a `SerialTransport` when a port is set and
  sim is inactive, so both `_open_pad()` and `_client()` pick it up with
  no per-command changes. No extra hardware probing.

#### 4. Firmware — pluggable host_api link

`host_api.cpp`'s dispatcher (`host_api_task`) is already structured
around two helpers, `vendor_read_exact()` and `vendor_write_frame()`,
plus a `tud_mounted()` connectivity check. Abstract those into a small
link interface so the same dispatch/decode/encode loop can run over a
different byte source:

```cpp
struct HostApiLink {
    virtual bool connected() = 0;
    virtual bool read_exact(uint8_t *dst, size_t n) = 0;     // false on disconnect
    virtual bool write_frame(const uint8_t *payload, size_t len) = 0;
};
```

The link's `read_exact` / `write_frame` implement the new
`MAGIC | LEN(u16) | payload | CRC8` framing from section 1: `write_frame`
prepends the magic + length and appends the CRC8; the read side scans
for the magic, validates the CRC, and resyncs on mismatch (shared with
the host-side reader's logic, just in C). `s_rx_buf` shrinks to match
the u16 length cap.

* `VendorLink` wraps the existing TinyUSB vendor-bulk code (now using
  the new framing).
* `SerialLink` (new, `#if CONFIG_TOUCHY_PROTO_OVER_SERIAL`) wraps the
  serial byte source. **For the jc4827w543 test build the serial
  endpoint is the USB-CDC ACM interface** (hence the requirement to also
  set `CONFIG_TINYUSB_CDC_ENABLED=y`): read via `tud_cdc_read`, write via
  `tud_cdc_write` + `tud_cdc_write_flush`, `connected()` =
  `tud_cdc_connected()`. Written generically enough that a future
  hardware-UART backend (CYD2USB, Stage 65) can drop in by swapping the
  read/write calls for `uart_read_bytes` / `uart_write_bytes`.
* `host_api_task` takes a `HostApiLink*`. `host_api_start()` always
  starts the vendor task and, when `CONFIG_TOUCHY_PROTO_OVER_SERIAL`,
  starts a *second* dispatcher task bound to `SerialLink`. The two run
  independently; the shared `s_evt_queue` is drained by whichever link's
  `EventConsumeCmd` poll arrives first (fine for the test scenario where
  only one host is attached at a time).
* **Keep the console off the protocol CDC port.** When CDC is enabled
  *for protocol use*, `usb_hid.cpp` must NOT call `tinyusb_console_init`
  on that CDC interface, and `CONFIG_TOUCHY_LOG_TO_UART` should route
  ESP_LOG to the real UART0 (or rely solely on the 64.1 in-band
  `LogRecord` tunnel). Add a guard so `TOUCHY_PROTO_OVER_SERIAL`
  suppresses the CDC console hookup.

New Kconfig (`firmware/main/Kconfig.projbuild`):

```
config TOUCHY_PROTO_OVER_SERIAL
    bool "Run the touchy protobuf protocol over a serial/CDC byte stream"
    default n
    help
        Adds a second host_api dispatcher that speaks the same
        length-prefixed nanopb framing over a UART-like byte stream
        (USB-CDC on jc4827w543 for testing; a hardware UART on
        USB-less boards like CYD2USB). The chosen port must be free
        of ESP_LOG output — device logs reach the host via the
        Stage 64.1 LogRecord tunnel instead.
```

#### 5. Test-build wiring (jc4827w543, temporary)

Per the stage request, flip on for this board only (user will revert):

* `firmware/sdkconfig.jc4827w543`: `CONFIG_TOUCHY_PROTO_OVER_SERIAL=y`
  and `CONFIG_TINYUSB_CDC_ENABLED=y` (re-enabling the CDC interface that
  Stage 64.2 turned off). Note in the file that this is a temporary
  test override.
* Endpoint budget: re-enabling CDC consumes the EPs that 64.2 freed;
  acceptable for a debug build (HID + vendor + CDC fit on the S3's IN
  endpoint budget as they did pre-64.2).

#### 6. Tests / validation

* **Framing unit test** (`app/tests/test_framing.py` or extend an
  existing transport test): `_pack` → `_unpack` round-trips; a flipped
  byte fails the CRC8 and is rejected; the reader resyncs to the next
  good frame after injected leading garbage / a truncated frame. Mirror
  in Rust (`#[test]`).
* **Python unit test** (`app/tests/test_transport_serial.py`): drive
  `SerialTransport` against a loopback pty (`os.openpty`) wired to a
  tiny in-process echo/dispatch stub that frames responses with the new
  `_pack`, asserting a `sys_board_info_get` round-trips. No hardware.
* **Refactor regression:** `test_transport_net.py` and the Rust
  `sim_tcp` test still pass after the base-class extraction *and* the
  framing change (any hardcoded `_pack` byte expectations get updated
  to the new layout).
* **Manual hardware check** (documented, not CI): `touchy --port
  /dev/ttyACM0 screens push ...` against a jc4827w543 flashed with the
  test config.

#### 6a. Docs to update

* `proto/touchy.proto` — the top-of-file "Wire format" comment (no
  longer a bare u32 prefix) and the `ProtocolVersion` enum (add `V5`,
  move `CURRENT`).
* `docs/host-api.md` — document the magic + u16 len + CRC8 frame and
  serial as an alternative physical layer alongside USB bulk.
* `docs/simulator.md` — TCP sim now uses the same new framing.
* `docs/python-api.md` / `docs/rust-api.md` — the `--port` / `--baud`
  options and the `SerialTransport` class.
* `AGENTS.md` / `CLAUDE.md` — bump the "latest active wire-format"
  line (`ProtocolVersion.CURRENT == 5`) and note the framing change.
* `firmware/main/host_api.cpp` / `transport.py` / `transport.rs`
  header comments that describe the old length-prefix framing.

#### 7. Out of scope for 64.3

* Auto-discovery / probing of serial ports — `--port` is explicit.
* Multiplexing logs and protocol on the *same* hardware UART without the
  64.1 tunnel.
* The CYD2USB hardware-UART `SerialLink` backend itself (lands with
  Stage 65; 64.3 only proves the seam on CDC).

## Stage 64.4

Future devices might have initially buggy display and touchscreen drivers.  Add a kconfig TOUCHY_NO_DISPLAY.  If set, suppress the creation/use of those devices.  No need to log messages at each attempted usage, just log one warning at startup saying "Display hardware disabled due to build options."

**Status: done.** `CONFIG_TOUCHY_NO_DISPLAY` (default n, in
`firmware/main/Kconfig.projbuild`) skips `board_init()`, the backlight
timer, `display_init()` and `touch_init()`. In their place `main.cpp`
stands up a headless LVGL display (`display_init_headless()`): an
off-screen RGB565 framebuffer with a no-op flush, sized by
`CONFIG_TOUCHY_HEADLESS_HRES` / `CONFIG_TOUCHY_HEADLESS_VRES` (default
480×272). The entire screen / host_api stack runs unchanged — screens
still build and render into the discarded buffer, touch simply never
produces events (`screens_set_touch(nullptr)`, and `touch_get_indev()`
already returns NULL so the activity callback is skipped). Exactly one
warning is logged at startup; there is no per-use logging. Validated:
firmware builds with the flag both off (default) and on.

## Stage 65: add support for ESP32-2432S028Rv3 board

See [here](hardware.md) for specs.  Somethings to note about this board:

* Try to find 'built-in'/standard ESP32 drivers for the display and touch screen if you can
* This board is ESP32 (not ES32-S3 based) so make sure the sdkconfig for the board sets that up correctly (no direct USB access so no USB code to be included, no PSRAM)
* include board_pins.h entries for all gpios in the hw doc (even if not currently needed)
* Because theres no USB you'll need to use the board UART for flash programming
* Initially, run the app debug output on that UART but once we've debugged the basics, I'm going to ask you to move our prioritary protobuf based protocol to be on that port instead (same wire encoding as we used for our TCP link to the simulator)
* The touchscreen is resistive and has no multitouch.  To support this (and anticipate boards of the future):
  * make a platform.h/.cpp class in the main code.   Boards will instantiate their own correct subclass which callers can access by platform_get().
  * Add a is_multitouch() method or property to that class.  The prior boards will return true, this ESP32-2432S028R board will return false.  Have our sim trackpad class check for that property and only try to do multitouch (or anything needing more than 'left' press/drag) on the older boards. 
  * Add a has_usb() method that indicates that this board has direct USB port access to the host.  The old boards do, this CYD2USB does not.

### Plan (subject to revision)

Board id / directory: **`esp32_2432s028rv3`**. This is the first
**classic ESP32** target (Xtensa LX6, `set-target esp32`), the first
board with **no native USB**, and the first with **no PSRAM** — three
firsts that touch the build system, the USB/HID layer, and the host
transport. Decisions locked with the user: include protocol-over-UART
in this stage; declare the IDF target per board (read by Justfile +
CI); gate USB behind a build flag *and* expose `platform_get()` plus
new proto capability fields; default the panel to ST7789 (BGR +
inversion configurable in `board_pins.h`).

#### 0. Why this matters

Every board so far has been ESP32-**S3** with native USB-OTG: the host
link is the vendor-bulk transport, and HID mouse/keyboard come "for
free" from TinyUSB. The CYD2USB has none of that — its only host
connection is a **CH340 UART** (USB VID 0x1a86 / PID 0x7523), so the
protobuf protocol must ride a hardware UART (Stage 64.3's framing was
built precisely so this is possible) and HID emulation is simply
unavailable. The app must therefore compile and run with the entire
USB/HID stack absent, and the host must learn at runtime which
capabilities a connected board actually has.

#### 1. Per-board ESP-IDF target (build system)

`firmware/CMakeLists.txt` and `Justfile` currently assume `esp32s3`.

* Each board declares its chip. Add `boards/<board>/target` (a one-line
  file, e.g. `esp32` / `esp32s3`) — self-describing and trivially
  readable from both bash and CMake.
* `just firmware-reconfigure [board]`: read the target from
  `boards/<board>/target` (fallback `esp32s3`) and pass it to
  `idf.py set-target <target>` instead of the hardcoded `esp32s3`.
* CI (`.github/workflows/app-ci.yml`): add `esp32_2432s028rv3` to the
  `build-firmware` matrix, and install **both** toolchains in the
  `install-esp-idf` job (`./install.sh esp32,esp32s3`). Cache key is
  per-IDF-version so this just grows the cached install once.
* `just flash`: also match `/dev/ttyUSB*` (CH340 enumerates there, not
  `/dev/ttyACM*`).

#### 2. Board component `boards/esp32_2432s028rv3/`

Mirror the jc4827w543 layout (`board.cpp`, `display.cpp`, `touch.cpp`,
`board/board_pins.h`, `CMakeLists.txt`, `idf_component.yml`,
`sdkconfig.defaults`, `target`).

* **`board_pins.h`** — *all* GPIOs from `docs/hardware.md` even if
  unused now (display SPI, touch SPI, SD SPI, RGB LED, audio, LDR, boot
  button, free expansion pins), plus `BOARD_LCD_BGR` and
  `BOARD_LCD_INVERT` toggles for the v3 ST7789 colour-order / inversion
  quirks.
* **`display.cpp`** — built-in `esp_lcd_panel_st7789` over
  `esp_lcd_panel_io_spi` on SPI2_HOST (MOSI 13 / SCK 14 / CS 15 / DC 2),
  320×240 RGB565, wired to `esp_lvgl_port` exactly like jc4827w543 but
  SPI instead of QSPI. Honour BGR/invert from `board_pins.h`.
  Single-buffer in internal SRAM (no PSRAM) — keep the draw buffer small
  (e.g. 40 lines) to fit the 520 KB budget.
* **`touch.cpp`** — managed component
  `espressif/esp_lcd_touch_xpt2046` on a **separate** SPI bus
  (SPI3_HOST: MOSI 32 / MISO 39 / CLK 25 / CS 33, IRQ 36). Report a
  single point (`TOUCH_MAX_POINTS` path returns ≤1); resistive, no
  multitouch.
* **`board.cpp`** — `board_init()` brings up both SPI buses, panel
  reset, backlight GPIO (default 21 with the "might be 27" note as a
  `board_pins.h` constant); `board_get_i2c_bus()` returns NULL (no I2C
  touch); plus this board's `platform_get()` (see §4).
* **`sdkconfig.defaults`** — `set-target esp32` implied by `target`;
  `CONFIG_ESPTOOLPY_FLASHSIZE_4MB`, **no** `CONFIG_SPIRAM`,
  `CONFIG_TOUCHY_PROTO_OVER_SERIAL=y` on
  UART0, `CONFIG_TOUCHY_LOG_OVER_PROTO=y` + `CONFIG_TOUCHY_LOG_TO_UART=n`
  (keep the IDF console off the protocol UART — Stage 64.3's rule),
  partition table `partitions/4M.csv`. (No USB flag needed — the absence
  of native USB is already implied by `set-target esp32`.)

#### 3. Make the USB/HID stack optional — keyed off the IDF target

No custom Kconfig flag: native-USB capability is implied by the IDF
target, which ESP-IDF already exposes as `CONFIG_SOC_USB_OTG_SUPPORTED`
(set for the esp32s3, unset for the classic esp32). On a no-USB target:

* `main/CMakeLists.txt`: swap `usb_hid.cpp` for `usb_hid_stub.cpp` in
  `SRCS` (`if(CONFIG_SOC_USB_OTG_SUPPORTED)`), and make the
  `espressif/esp_tinyusb` dependency conditional via an
  `idf_component.yml` rule `if: "target in [esp32s2, esp32s3]"`
  (TinyUSB needs USB-OTG; classic ESP32 has none, so the component must
  not be required).
* `host_api.cpp`: `#if CONFIG_SOC_USB_OTG_SUPPORTED` around the TinyUSB
  includes (`tinyusb.h`/`tusb.h`) and `VendorLink`; the CDC `SerialLink`
  likewise stays USB-only.
* **HID emitters become stubs.** `usb_hid.{h,cpp}` keep their
  signatures; `usb_hid_stub.cpp` (compiled on no-USB targets) provides
  `usb_hid_mouse_*` / `usb_hid_keyboard_report` that log once-at-most +
  no-op, so `macros.cpp` and `trackpad_widget.cpp` link unchanged. (Per
  the project convention: log + sane default, don't crash.)
* **`host_api_start()` no longer lives inside `usb_hid_init()`.**
  Decouple: `usb_hid_init()` only does USB; `app_main` calls
  `host_api_start()` explicitly after transports are configured.
  `host_api_start()` selects links by config: vendor (+ optional CDC)
  on native-USB targets, the UART link otherwise.

#### 4. UART host_api link (protocol over UART0)

* New `UartLink : HostApiLink` (in `host_api.cpp`, `#if
  CONFIG_TOUCHY_PROTO_OVER_SERIAL && !CONFIG_SOC_USB_OTG_SUPPORTED`)
  backed by
  `uart_driver_install` + `uart_read_bytes` / `uart_write_bytes` on
  `UART_NUM_0` at the fixed 115200 baud. `connected()` is always true
  (a UART has no link-up signal). Reuses the existing
  `read_frame`/`write_frame` framing verbatim — the whole point of
  Stage 64.3.
* Boot-noise tolerance: the ROM/bootloader banner on UART0 at reset is
  exactly what the MAGIC+CRC resync in `read_frame` already discards, and
  the host's open-time retry in `touchy_open` re-establishes sync.
* Device logs reach the host via the Stage 64.1 `LogRecord` tunnel
  (already enabled), never as raw UART text on the protocol port.

#### 5. `platform.h/.cpp` capability abstraction

* `main/platform.h`: abstract `struct Platform { virtual bool
  is_multitouch() const; virtual bool has_usb() const; }` and `Platform
  *platform_get();`.
* Each board defines a concrete subclass + `platform_get()` in its
  `board.cpp`: jc4827w543 & waveshare → `{multitouch:true, usb:true}`;
  esp32_2432s028rv3 → `{multitouch:false, usb:false}`. (Defaults can key
  off `CONFIG_SOC_USB_OTG_SUPPORTED` / `TOUCH_MAX_POINTS` to avoid drift.)
* `fill_board_info()` reads `platform_get()` to populate the new proto
  fields.

#### 6. Proto capability fields + host plumbing

* `proto/touchy.proto`: add `bool is_multitouch = 7;` and `bool has_usb
  = 8;` to `SysBoardInfoResponse`; add `V6` + move `CURRENT = 6` (adding
  optional scalar fields is technically wire-compatible, but the project
  bumps the version on any schema change). Update firmware
  `TOUCHY_PROTOCOL_VERSION` automatically (it aliases `CURRENT`).
  Regenerate Python + nanopb bindings via `just build-proto`.
* Host: surface `is_multitouch` / `has_usb` on the Python
  `TouchyClient` board-info result and the Rust equivalent.
* **Sim trackpad**: the host-side trackpad input handler honours
  `is_multitouch` — only synthesises multi-finger / non-left gestures
  when the connected device reports multitouch; otherwise restricts to
  single-point left press/drag. The simulator's own `Platform` reports
  `{multitouch:true, has_usb:true}` (configurable) so existing sim tests
  are unaffected.

#### 7. Tests / validation

* **Firmware:** builds for all three boards (`esp32`, two `esp32s3`)
  flag-on and flag-off; the no-USB path links without TinyUSB.
* **Host:** existing 143 Python + Rust suites stay green after the proto
  bump; add a unit test that a board-info with `is_multitouch=false`
  makes the sim trackpad refuse multi-point input.
* **Manual hardware (documented, not CI):** flash over UART
  (`/dev/ttyUSB0`), confirm the built-in default screen renders and
  single-touch works, then `touchy --port /dev/ttyUSB0 screens push ...`
  round-trips over the UART transport.

#### 8. Docs to update

* `docs/hardware.md` (mark the board supported, resolve the
  ST7789/backlight-pin uncertainties once validated), `docs/host-api.md`
  (UART as a physical layer; capability fields), `proto/touchy.proto`
  header + `ProtocolVersion`, `AGENTS.md`/`CLAUDE.md` (new board,
  `ProtocolVersion.CURRENT == 6`, target-implied USB
  (`CONFIG_SOC_USB_OTG_SUPPORTED`), per-board target),
  `firmware/README.md` (multi-target build).

#### 9. Out of scope for 65

* SD-card, RGB-LED, audio, LDR drivers (pins defined in `board_pins.h`,
  no drivers yet).
* Haptics. BLE-HID as an alternative to USB-HID on this board.
* The 2.4"/other CYD variants (only `esp32_2432s028rv3` here).

#### Open items to confirm during implementation

* Backlight GPIO (21 vs 27) and ST7789 BGR/inversion flags — resolve on
  real hardware; exposed as `board_pins.h` constants so flipping them is
  a one-line change.
* Whether to keep a temporary "logs on UART0, protocol off" bring-up
  toggle in the board `sdkconfig.defaults` (commented) for first-light
  debugging before the protocol takes over the port.

### Status: done

All nine work items above are implemented and validated.

* **Per-board target.** `firmware/boards/<board>/target` (one line, e.g.
  `esp32`) is read by `just firmware-reconfigure [board]` and passed to
  `idf.py -DBOARD=<board> set-target <target>`. The `-DBOARD` on the
  `set-target` line is required — without it `set-target` rewrites the
  *default* board's sdkconfig and never applies the chip to the target
  board. `firmware/sdkconfig.<board>` is per-board and gitignored; always
  `rm -f firmware/sdkconfig firmware/sdkconfig.<board>` before switching
  chips so a stale target isn't reused.
* **USB gating.** Keyed off `CONFIG_SOC_USB_OTG_SUPPORTED` (set for
  esp32s3, unset for classic esp32) — in CMake `if(...)`, in C `#if ...`,
  and in `idf_component.yml` rules (`if: "target in [esp32s2, esp32s3]"`)
  so TinyUSB is only required where a USB-OTG core exists.
* **Board component.** `boards/esp32_2432s028rv3/board/` —
  `display.cpp` drives ST7789 over SPI2 via `esp_lcd` + `esp_lvgl_port`
  (double-buffered, 40-line buffers in internal SRAM, RGB565 byte-swap);
  `touch.cpp` drives XPT2046 over SPI3 via the managed component
  `atanisoft/esp_lcd_touch_xpt2046 ^1.0.6` (the de-facto community
  driver; the `espressif/` namespace has no such package), single point,
  no multitouch. `board_pins.h` carries every GPIO from `hardware.md`
  plus the ST7789 BGR/INVERT/SWAP/MIRROR and backlight-pin constants.
  Note: in IDF v6 `esp_lcd_panel_dev_config_t::reset_gpio_num` is
  `gpio_num_t` — assign the `BOARD_LCD_GPIO_RST` enum directly.
* **UART transport.** `UartLink` (gated `#if
  CONFIG_TOUCHY_PROTO_OVER_SERIAL && !CONFIG_SOC_USB_OTG_SUPPORTED`) runs
  the Stage 64.3 framing over `UART_NUM_0` at 115200; `main` REQUIRES
  `esp_driver_uart` (IDF v6 split `driver/uart.h` out). One gotcha worth
  recording: in `sdkconfig.defaults`, `# CONFIG_X is not set` is **not** a
  comment — it is the `X=n` directive and will silently override an
  earlier `CONFIG_X=y`; the board's bring-up notes are therefore plain
  prose, not `# CONFIG_…` lines.
* **No PSRAM, images still supported.** `RamFs` already prefers
  `MALLOC_CAP_SPIRAM` and falls back to internal `malloc`, and the
  `esp_cache_msync` calls are `#if CONFIG_SPIRAM`-gated, so the `R:`
  ramdisk and image assets work on the CYD bounded by ~520 KB internal
  SRAM — no code change was needed, only a corrected sdkconfig comment.
* **Proto + host.** `ProtocolVersion.CURRENT == 6`;
  `SysBoardInfoResponse` gained `is_multitouch` / `has_usb`. The sim
  emits them (default true, constructor-configurable to emulate the CYD),
  `touchy board-info` shows display size + multitouch/usb rows, and the
  raw proto already surfaces them on `TouchyClient` and the Rust client.
* **Builds.** All three boards build green — `jc4827w543` (esp32s3),
  `waveshare_s3_lcd_7b` (esp32s3), `esp32_2432s028rv3` (esp32). Host
  suite: 145 Python tests pass (two new sim-capability tests).

## Stage 65.1: ESP32-2432S024 support

This is a board that is only slightly different from the ESP32-2432S028v3 you've already added support for.  Please refactor so it can share code (there are other 'CYD' devices coming soon).  The only difference vs 028v3 is that it uses a ILI9341 display controller.

**Status: done.** Refactored so the whole CYD family shares one set of
sources, then added the 2.4" ILI9341 board on top of that:

- All CYD board C++ now lives in `firmware/boards/cyd_common/`
  (`board.cpp`, `display.cpp`, `touch.cpp`). Each board directory keeps only
  its `board_pins.h` plus a tiny `board/CMakeLists.txt` and
  `idf_component.yml`; the shared `.cpp` files are referenced by relative
  path (`../../cyd_common/*.cpp`) so they compile against each board's own
  pin map (`PRIV_INCLUDE_DIRS "."` puts the board's `board_pins.h` first).
  No symlinks — just a shared component plus per-board pin headers.
- `cyd_common/display.cpp` is controller-agnostic: a board selects its panel
  driver from `board_pins.h` via `BOARD_LCD_CONTROLLER_ILI9341` /
  `BOARD_LCD_CONTROLLER_ST7789`. The ILI9341 branch pulls the
  `espressif/esp_lcd_ili9341` managed component and calls
  `esp_lcd_new_panel_ili9341`; the default ST7789 branch uses the in-tree
  `esp_lcd_new_panel_st7789`. Bring-up (reset/init/invert/swap/mirror) is
  otherwise identical.
- New board `firmware/boards/esp32_2432s024/` (classic ESP32, `target` =
  `esp32`, no USB, protocol over the CH340 UART exactly like the 028v3). Its
  `board_pins.h` mirrors the 028v3 pin map but selects ILI9341 and starts
  with `BOARD_LCD_INVERT_COLOR=0` (ST7789's invert quirk doesn't apply);
  colour/orientation flags are flip-on-hardware constants pending validation
  on a real unit.
- `esp32_2432s028rv3` now also builds from `cyd_common` (its own `.cpp` files
  were deleted) and declares `BOARD_LCD_CONTROLLER_ST7789` in `board_pins.h`.
  Both CYD boards build green for the `esp32` target.

Open hardware-validation items for the 024 (same as any new CYD): confirm
backlight GPIO (21 vs 27), BGR/INVERT/SWAP/MIRROR flags, and XPT2046 touch
calibration on a real panel.

## Stage 67: inline host-event callbacks — DONE

Cleaned up the public Python API so users can attach an event handler
right where they build a widget, instead of manually juggling numeric
host codes and a separate `on_host_event` registration:

```python
s += button("ping", on_click=host_action(on_event=lambda e: print(e.user_data)))
```

How it works:

- `touchy_pad.api.screens.host_action` gained a keyword-only
  `on_event` parameter. `code` is now optional. When `on_event` is
  supplied without an explicit `code`, a unique code is auto-allocated;
  when both are given the callback binds to the explicit code; the old
  `host_action(code)` form still works unchanged.
- Auto codes come from a reserved range starting at
  `AUTO_CODE_BASE = 0x10000` (a thread-safe `itertools.count`). Manual
  codes are expected to stay **below** `0x10000` so the ranges never
  collide; the base is small so the varint-encoded `ActionHost.code`
  stays compact on the wire.
- A new tiny, import-cycle-free module
  `app/src/touchy_pad/api/_events.py` holds the counter and a global
  `dict[int, callback]` of pending bindings (`alloc_code`,
  `register_binding`, `harvest`, `_reset` for tests). A protobuf can't
  carry a Python callable, so the callback is stashed here keyed by code.
- `Touchy.screen_save` / `Touchy.widget_save` now call
  `_register_inline_callbacks(msg)`, which walks the serialised proto
  tree (`_collect_host_codes`, a generic reflection-based recursion that
  also skips map-entry fields), harvests the pending bindings for exactly
  the codes that screen/widget references, and registers each via
  `on_host_event`. So callbacks light up automatically on upload, scoped
  to the right device.
- Migrated `build_demo()` and the `touchy demo --listen` CLI handler to
  the inline style (the demo's ping/slider/checkbox/smiley handlers now
  live inline as `on_event=` lambdas; the CLI no longer hand-registers
  `0x100..0x103`). `test_sim_window` asserts on widget identity / payload
  instead of fixed codes.
- Docs: `docs/python-api.md` "Event callbacks" now leads with the inline
  form and keeps explicit codes + `on_host_event` as the lower-level path.

## Stage 68: clean up screen switching — DONE

**Implemented.** Screens moved `host/screens/` → `host/s/` behind symbolic
path constants (`app/src/touchy_pad/paths.py`, `firmware/main/screens.h`
macros, Rust `lib.rs` consts). The prev/next chrome is now a single default
screen `host/s/default.pb` built by
`touchy_pad.api.screens.build_default_screen()` — a vertical flex `col`
holding a content-sized prev/next chrome row plus a flex-growing body
`widget_ref(id="page")`. The original additive `int32 flex_grow` field on
`Rect` mapped to `lv_obj_set_flex_grow` (honoured only under flex parents)
and drove the growing body. (Stage 72 later replaced this with
`Widget.grow_x` / `grow_y`; see that stage.) User page bodies live under `host/uscr/` and are uploaded
with `Touchy.user_screen_save(name, widget)`; `build_user_pages()` returns
the `trackpad` + `test` bodies. The `touchy screen init` CLI provisions the
chrome + a trackpad page; `screen demo` reuses `_do_screen_init` then adds
the smiley asset + `test` page. The firmware's built-in fallback is now
generated from the same DSL: `proto/gen_default_screen.py` →
`proto/default_screen.json` (pure JSON — `json_format.Parse` rejects
comments) → `embed_screen_json.py` → `firmware/main/default_screen_pb.h`,
wired through `just gen-default-screen` / `build-default-screen`; the
simulator reads `proto/default_screen.json` at runtime. Boot (device + sim)
prefers `*:host/s/default.pb`, else first-discovered, else the compiled
fallback. Validated: `just build-proto`, `just app-test` (157 passed),
`just app-lint` (clean), `just firmware-build` (clean), `cargo build`
(clean).

* Change existing places that were using F:host/screens to use a symbolic constant (for code cleanliness) instead.  And that constant should have the value "F:host/s" (I'm picking 's' as the new shorter filepath for the 'screens' directory)
* move the next/prev buttons into standard widgets at the top of all screens (by default - even custom user screens).  It was a mistake that I started putting that row at the top of every 'screen'.pb file.  Really - most of the time users should just be updating content in a (new host/uscr) directory.  With one file for each top level widget that fills the bottom portion of the screen.
*  Do this by putting it in a single default.pb screen file and having it work with files in host/uscr/foo.pb widget layout files (to fill the entire bottom portion of the screen).  Most of the time users will just populate uscr (user screens) files.  If users want a true completely custom experience they can replace host/s/default.pb with whatever they want (though this is not recommended).
* change that default screen layout to use a vertical flow layout so the top row automatically shrinks to cover just the sizes needed for those botton.
* in the python code move the next/prev screen buttons out of the "screen demo" cli.  Move them into a new "screen init" cli command which is intended to write any config files (in this case host/s/default.pb).  Calling "screen demo" should imply that it should also screen init.

### Decisions (locked in with the user)

* **Clean break** on the screens directory: the on-disk dir moves from
  `host/screens/` → `host/s/`. No backward-compat read of the old path;
  existing devices need a flash wipe + re-push. (Same for the sim's
  pseudo-fs.)
* **New `host/uscr/` directory** ("user screens") holds one widget-layout
  `.pb` per top-level page that fills the *bottom* of the default screen.
  The default screen's prev/next + `widget_ref` page through `host/uscr/`.
  `host/w/` is **unchanged** and still holds arbitrary widget-refs
  (TouchyDeck keys, `widget_save`).
* **`trackpad` is a baseline page written by `screen init`**, not by
  `screen demo`. So after `screen init` the device already has a usable
  trackpad page. `screen demo` adds the `test` showcase page (and the
  smiley image asset) on top.
* **Boot prefers `*:host/s/default.pb`** when present, else falls back to
  the first-discovered screen, else the built-in compiled fallback.
* **The built-in fallback is generated from the same Python chrome
  builder.** A build step runs `build_default_screen()` to (re)emit
  `proto/default_screen.json`, which `embed_screen_json.py` turns into
  `firmware/main/default_screen_pb.h` and which the sim loads at runtime.
  Device, sim, and host therefore all share one definition of the chrome.

### Names / constants

| Meaning | On-disk (relative) | Drive-prefixed | Constant |
|---|---|---|---|
| Screens dir | `host/s/` | `F:host/s/` | `SCREENS_DIR` |
| Default screen file | `host/s/default.pb` | `F:host/s/default.pb` | `DEFAULT_SCREEN_PATH` |
| User-screen page bodies | `host/uscr/` | `F:host/uscr/` | `USER_SCREENS_DIR` |
| Widgets (generic refs) | `host/w/` (unchanged) | `F:host/w/` | `WIDGETS_DIR` |
| Images (unchanged) | `host/images/` | `F:host/images/` | `IMAGES_DIR` |

Define these once per language and import everywhere instead of literals:

* **Python** — new internal module `app/src/touchy_pad/paths.py` (top-level
  so `api/`, `sim/`, `client.py`, `cli.py` can all import it without a
  cycle). Re-export `SCREENS_DIR`, `DEFAULT_SCREEN_PATH`, `USER_SCREENS_DIR`
  from `touchy_pad.api.__init__` for public use.
* **Firmware C++** — in `firmware/main/screens.h`:
  `inline constexpr const char *HOST_SCREENS_SUBDIR = "host/s";` and
  `inline constexpr const char *DEFAULT_SCREEN_FILE = "default.pb";`.
* **Rust** — `pub const SCREENS_DIR: &str = "F:host/s/";` etc. in
  `rust/touchy-pad/src/lib.rs`.

### Implementation plan (do in this order)

**Phase 1 — proto + generated bindings (comment/string only).**
Update the doc comments mentioning `F:host/screens/...` in
`proto/touchy.proto`, `proto/widgets.proto`, `proto/preferences.proto`
(and their copies under `rust/touchy-pad/proto/`) to `F:host/s/...`. These
are comments only — no field changes, so the wire format is unchanged
(`Screen.Version.CURRENT` and `ProtocolVersion.CURRENT` stay put). Run
`just build-proto` to regenerate `app/.../_proto`, the firmware
`firmware/main/proto/*.pb.h`, and the Rust bindings. Do **not** hand-edit
the generated `*.pb.h`.

**Phase 2 — Python paths module + DSL builders.**
1. Create `app/src/touchy_pad/paths.py` with the constants table above
   plus small helpers if useful (e.g. `screen_path(name) -> SCREENS_DIR +
   name + ".pb"`, `user_screen_path(name)`, `widget_path(name)`).
2. In `app/src/touchy_pad/api/screens.py`, refactor `build_demo()`:
   * Add **`build_default_screen() -> Screen`** — the chrome. It must be a
     **vertical flex column** (`col(...)` / `flex(..., column)`) where the
     **top chrome row** (`< Prev`, `Next >`, optionally an `fps`) is sized
     to its content and the **body** is a `widget_ref(id="page")` that
     **grows to fill** the remaining height. Verify the flex DSL: the body
     cell/widget needs `flex_grow=1` (inspect `flex`/`col`/`cell` and the
     `Layout`/`LayoutFlex` proto for the grow field; the chrome row gets no
     grow so it shrinks to content). Initial body path =
     `USER_SCREENS_DIR + "trackpad.pb"`; prev/next use
     `prev_widget_action("page", USER_SCREENS_DIR)` /
     `next_widget_action("page", USER_SCREENS_DIR)`. Name the screen
     `"default"` so it serialises to `host/s/default.pb`.
   * Add **`build_user_pages() -> list[tuple[str, Widget]]`** returning the
     `("trackpad", pad_widget)` and `("test", showcase_widget)` page bodies
     (lifted from today's `build_demo`). Keep the inline
     `host_action(on_event=...)` callbacks from Stage 67 on the `test` page.
   * Keep `build_demo()`/`build_demo_screen()` as thin shims if anything
     still imports them, or update call sites; prefer removing the
     prev/next chrome from per-page widgets entirely (that was the
     "mistake" the user called out).
3. In `app/src/touchy_pad/api/device.py`:
   * Replace the `f"F:host/screens/{name}.pb"` literals in `screen_save`
     and the default in `screen_load`'s docstring with `SCREENS_DIR`.
   * Add **`user_screen_save(name, widget, *, drive="F") -> str`** writing
     to `{drive}:host/uscr/{name}.pb` (mirror `widget_save`, including the
     Stage-56 version stamp and the Stage-67
     `_register_inline_callbacks`). `widget_save` (→ `host/w/`) is
     unchanged.

**Phase 3 — CLI (`app/src/touchy_pad/cli.py`).**
* Add **`screen init`**: opens the pad, writes the chrome via
  `pad.screen_save(build_default_screen())` (→ `host/s/default.pb`), writes
  the baseline `trackpad` page via `pad.user_screen_save("trackpad", ...)`,
  then `pad.screen_load(DEFAULT_SCREEN_PATH)`. No prev/next wiring here —
  that now lives inside the chrome.
* Rework **`screen demo`** to call the same init path first (factor a
  helper `_do_screen_init(pad)` so `demo` reuses it), then upload the
  smiley image and the `test` page via `user_screen_save`, and load the
  default. The `--listen` block stays as-is (Stage 67 inline callbacks).
  `--json` should print the chrome + the user pages.
* Update the `screen load` help string example to `F:host/s/home.pb`.

**Phase 4 — built-in fallback generation.**
* Add `proto/gen_default_screen.py` (or extend `embed_screen_json.py`):
  imports `touchy_pad.api.screens.build_default_screen`, serialises to
  canonical proto JSON via `google.protobuf.json_format.MessageToJson`,
  and writes `proto/default_screen.json`.
* Add a `just gen-default-screen` recipe and chain it: `build-proto-py`
  → `gen-default-screen` → the existing `embed_screen_json` step that
  produces `firmware/main/default_screen_pb.h`. Keep
  `proto/default_screen.json` tracked in git (the sim reads it at runtime;
  see Phase 5), and note in a header comment that it is generated.
* Sanity: the chrome's `widget_ref` points at `host/uscr/trackpad.pb`; on a
  truly empty fs that body is blank until `screen init` runs — acceptable.

**Phase 5 — firmware C++ (`firmware/main/screens.cpp` / `screens.h`).**
* `is_screen_path()`: compare against `"host/s/"` (length 7, **not** 13)
  using `HOST_SCREENS_SUBDIR`. Keep tolerating an optional leading `/`.
* `screens_init()` scan: `fs.list("host/s", ...)` and
  `full.append(":host/s/")` (build from the constant).
* Boot preference: when a freshly registered or discovered screen's key
  ends with `:host/s/default.pb` (use `DEFAULT_SCREEN_FILE`), set it as
  `g_default_screen_path` even if another screen arrived first. Simplest:
  in `screens_register_from_file`/`screens_init`, prefer a `default.pb`
  match over "first discovered". `screens_load(NULL)` already follows
  `g_default_screen_path`, so only the selection rule changes.
* No change needed to `change_widget_ref` next/prev: the directory comes
  from the action's `path`, so `host/uscr/` flows through as data.

**Phase 6 — simulator.**
* `sim/fs.py`: `scan_screens()` and the screen-path glob use `host/s`
  instead of `host/screens` (import the Python constant; strip the `F:`
  prefix to the relative `host/s`). `_iter_pb` / `list_pb` already take a
  directory arg, so `host/uscr/` works unchanged.
* `sim/device.py`: still loads `proto/default_screen.json` (now generated)
  as the embedded fallback; add the same "prefer `host/s/default.pb`"
  selection when scanning a non-empty fs.
* `sim/__init__.py`: update the `screens/home.pb` example path.

**Phase 7 — Rust.**
* `rust/touchy-pad/src/lib.rs`: add the `SCREENS_DIR` etc. consts; update
  `touchy-demo/src/main.rs` (`F:host/s/rust_demo.pb`) and
  `touchy-opendeck/src/layout.rs` (`R:host/s/opendeck_{id}.pb`).
* Update `rust/touchy-opendeck/README.md` path reference.

**Phase 8 — tests + docs.**
* Python tests: update `test_sim_transport.py`, `test_sim_window.py`,
  `test_screens.py` literals (`F:host/screens/` → `F:host/s/`,
  `F:host/w/` page-body paths → `F:host/uscr/`). Add coverage:
  `build_default_screen()` is a vertical-flex chrome with a growing
  `widget_ref(id="page")` into `host/uscr/`; `user_screen_save` writes to
  `host/uscr/`; `screen init` produces `host/s/default.pb` +
  `host/uscr/trackpad.pb`; the regenerated `default_screen.json` round-trips
  to a Screen named `default`.
* Docs: `docs/host-api.md`, `docs/why-not-xml.md`, `docs/simulator.md`,
  `docs/python-api.md`, `proto/*.proto` comments, and `AGENTS.md`
  (filesystem-paths bullet + a Stage 68 highlight). Add the Stage-68 "DONE"
  writeup here and flip the heading.
* Run `just build-proto`, `just app-test`, `just app-lint`, and at least
  one `just firmware-build` (default board) to confirm the C++ compiles;
  `cargo build` for the Rust crates.

### Gotchas

* `firmware/main/proto/*.pb.h` and `app/.../_proto` are **generated** —
  edit `proto/*.proto` then `just build-proto`, never the outputs.
* `proto/default_screen.json` flips from hand-maintained to generated;
  don't hand-edit it after Phase 4. The sim reads it at runtime via a
  dev-tree relative path (`parents[4]/proto/default_screen.json`).
* The `is_screen_path` length constant (13 → 7) is easy to miss — it's a
  raw `strncmp` count.
* Keep `Screen.Version` / `ProtocolVersion` unchanged: this stage moves
  files and comments, not wire fields.
* `CLAUDE.md` is a symlink to `AGENTS.md` — edit once.

## Stage 70: make OpenDeck device enumeration/registration actually work — DONE

### Motivation

Stage 62 shipped the `touchy-opendeck` plugin with a full hot-plug /
enumerate / `register_device` path on paper, but **in practice it was
never observed successfully telling OpenDeck about a connected
Touchy-Pad** — no device ever appeared in OpenDeck's device list. This
stage is a debugging-and-hardening pass: instrument every step heavily,
align the plugin to the *proven* reference implementation
(`4ndv/opendeck-akp153`, now vendored read-only at
`tools/reference/opendeck-akp153/`), and refresh the
[opendeck-device-plugin.md](opendeck-device-plugin.md) write-up with
what we learn.

**Be clear about the starting point:** the scaffolding already exists.
`rust/touchy-opendeck/src/plugin.rs` already has `run_hotplug_loop`
(1 Hz USB poll diffing attached devices), `attach` (open transport →
`sys_board_info_get` → build grid screen → spawn event task →
`device_plugin::register_device`), `detach`
(`device_plugin::unregister_device`), and per-event forwarding
(`device_plugin::key_down` / `key_up`). Stage 70 does **not** rewrite
this from scratch — it figures out *why the registration is invisible*
and makes the whole lifecycle observable.

### Findings from the reference example

The reference plugin (`tools/reference/opendeck-akp153/`) is the plugin
the OpenAction `device_plugin` surface was designed around, so it is the
ground truth for "what a working registration looks like". Key
differences vs. our crate:

| Aspect | Reference (`opendeck-akp153`) | Our `touchy-opendeck` |
|---|---|---|
| openaction version | `openaction = "1.1.5"` | `openaction = "2"` (resolves to 2.6.0) |
| Entry point | `init_plugin(GlobalEventHandler, ActionEventHandler)` inside `tokio::select!` w/ SIGTERM | `set_global_event_handler(&leaked)` + `openaction::run(args)` |
| Register call | `OUTBOUND_EVENT_MANAGER.lock().await.as_mut()` → `outbound.register_device(id, name, rows, cols, encoders, device_type)` | `device_plugin::register_device(id, name, rows, cols, 0, type)` free fn |
| Log sink | `TermLogger::init(Info, …, TerminalMode::Stdout, …)` | `TermLogger::init(Info, …, TerminalMode::Stderr, …)` |
| Per-device model | one `device_task(candidate, CancellationToken)` per device, tracked in a `TaskTracker`; events read in a loop, each `key_down/up` via `OUTBOUND_EVENT_MANAGER` | one spawned `event_task` per device storing a `JoinHandle`; hot-plug diff loop owns lifecycle |
| Logging density | `log::info!` at nearly every step (candidate found, registering, reader ready, each update) | sparse — mostly one `info` per attach, rest `debug`/`warn` |

### How OpenDeck actually transports + captures plugin output (verified)

Before guessing, the openaction crate and the OpenDeck source were read
directly to settle the stdout/stderr question:

* **The OpenAction transport is a real TCP WebSocket, not stdio.**
  `openaction::run` parses a `-port <n>` CLI arg and calls
  `connect_async("ws://localhost:{port}")` (openaction 2.6.0
  `src/lib.rs:63`; v1.1.5 is the same shape). OpenDeck runs a WebSocket
  *server* (`init_websocket_server`) and passes the port on argv. So the
  plugin's own stdin/stdout/stderr are **not** the link — writing to
  them cannot corrupt the protocol.
* **OpenDeck redirects the plugin's stdout *and* stderr to a log file.**
  When it spawns a native plugin it does
  `.stdout(Stdio::from(log_file))` / `.stderr(Stdio::from(log_file))`
  where `log_file = log_dir()/plugins/<plugin-uuid>.log`
  (`OpenDeck/src-tauri/src/plugins/mod.rs`). With the dev build from
  `just opendeck-run` (Tauri identifier `opendeck`) that path is
  `~/.local/share/opendeck/logs/plugins/com.geeksville.touchypad.log`;
  a packaged OpenDeck may use a different identifier.

**Consequence:** the original "stdout corrupts the JSON socket" comment
in our `main.rs`/README is simply wrong, and **stderr vs stdout doesn't
matter** — OpenDeck funnels both into the same per-plugin log file. The
most likely reason "I never saw it notifying the app" is that the logs
were going to that file (not the terminal), combined with one of the
registration/enumeration faults below. Per the user's instruction we
will still match the reference exactly (simplelog `TermLogger` → Stdout,
`Info`), purely to remove any doubt and stay byte-for-byte aligned with a
known-good plugin.

### Suspected root causes (investigate in order)

1. **Looking in the wrong place / log level.** Because OpenDeck captures
   stdout+stderr to `log_dir()/plugins/<plugin-uuid>.log`, nothing shows
   in the terminal that launched OpenDeck. First step in debugging is
   `tail -f` that file. Ensure the plugin logs at `Info` and emits a
   line at *every* lifecycle step (Phase 1) so the file tells the whole
   story.

2. **`device_plugin::register_device` (v2) actually reaching the
   socket.** Confirm the openaction-2 free function maps onto the same
   outbound `registerDevice` event the v1 `OUTBOUND_EVENT_MANAGER` path
   sends. If it is a no-op / not yet wired in the version we pull,
   switch to the reference's proven pattern (acquire the outbound
   manager and call `register_device` on it). Pin/verify the exact
   openaction version in `Cargo.lock`.

3. **Registration timing vs. the outbound manager being ready.** We
   spawn `run_hotplug_loop` from `plugin_ready`; the first
   `register_device` fires ~1 s later. Verify the outbound channel /
   WebSocket handshake is fully up before the first registration (the
   reference registers from inside the per-device task spawned *after*
   `plugin_ready`, which is safe). Log the moment registration is
   attempted and its `Result`.

4. **Enumeration returning empty.** If `enumerate(vid, pid)` finds
   nothing (nusb backend missing, udev permissions, or the device held
   open by another process), `attach` never runs and nothing registers.
   Today an enumerate error is logged at `debug` and an empty result is
   silent. *Fix:* `log::info!` the candidate count and each candidate's
   VID/PID/bus/addr every poll (rate-limited), and log when the set is
   empty.

5. **Device-ID namespace.** OpenDeck routes by the manifest's
   two-char `DeviceNamespace` (ours is `"tp"`), and every registered ID
   must start with it. `layout::device_id_for` returns `"tp-<bus><addr>"`
   — consistent — but log the exact ID string we register so a prefix
   mismatch is obvious.

### Plan

**Phase 1 — Instrumentation (do this first, before changing behaviour).**
Add `log::info!` at every lifecycle step so a single run shows the whole
story, mirroring the reference's density:
* `plugin_ready`: "plugin_ready — outbound manager ready, starting watcher".
* each poll: candidate count + per-candidate VID/PID/bus/addr; an explicit
  "no Touchy-Pad devices found" when empty.
* `attach`: start, board-info result (`board_name`, `display_width × height`,
  `protocol_version`), computed `cols × rows`, screen upload, event-task
  spawn, and the `register_device(id, name, rows, cols, encoders, type)`
  call **with its `Result`** logged.
* `detach`: which id, task cancellation, `unregister_device` result.
* event forwarding: each `LvEvent` (code/host_code/key) → `key_down`/`key_up`.
* `set_image` / `set_brightness` inbound events: log device id + position.

**Phase 2 — Logging init exactly like the example.** Match the reference
verbatim: `simplelog::TermLogger::init(LevelFilter::Info,
Config::default(), TerminalMode::Stdout, ColorChoice::Never)`. Keep an
env override (e.g. `RUST_LOG` / `TOUCHY_LOG`) so `Debug` is one var away.
Delete the inaccurate "stdout corrupts the socket" comment from `main.rs`
and the README — OpenDeck captures both stdout and stderr into
`log_dir()/plugins/<plugin-uuid>.log`, so the sink is irrelevant to
correctness.

**Phase 3 — Verify / fix the registration path.** Trace whether
openaction-2 `device_plugin::register_device` emits the outbound
`registerDevice` event. If not (or unreliable), adopt the reference's
`OUTBOUND_EVENT_MANAGER`-based call. Either way, ensure registration
happens only after `plugin_ready`/outbound is live, and log success.

**Phase 4 — Restructure per-device tasks to mirror the example
(optional but recommended).** Split `plugin.rs` into modules analogous
to the reference (`watcher.rs` for enumerate + hot-plug, `device.rs` for
the per-device task + event loop), use a `CancellationToken` per device
plus a `TaskTracker` for clean shutdown, and register from inside the
device task (as the reference does). This makes "device threads" map
one-to-one to the example and gives graceful SIGTERM teardown. Reuse the
rust-api patterns already exercised by `touchy-demo`
(`Touchy::from_transport`, `file_save`, `screen_save`/`screen_load`,
`events()`).

**Phase 5 — Documentation.** Update
[opendeck-device-plugin.md](opendeck-device-plugin.md) from these
findings (see below) and substantially expand
[rust/touchy-opendeck/README.md](../rust/touchy-opendeck/README.md):

* Fix the architecture diagram + remove the wrong "OpenDeck reads stdout
  as JSON" claim (the link is a TCP WebSocket; stdout/stderr are both
  captured to a log file).
* Add a **"Running a debug build under OpenDeck"** section so a
  contributor can iterate without `opendeck-package` + GUI "install from
  file" each time: symlink the `.sdPlugin` bundle into OpenDeck's
  `config_dir()/plugins/`, symlink the `cargo build` (debug) binary into
  the bundle's `CodePath`, run `just opendeck-run`, and
  `tail -f log_dir()/plugins/<plugin-uuid>.log` to watch the plugin's
  output. Document the exact dev-build paths (`~/.config/opendeck/plugins/`,
  `~/.local/share/opendeck/logs/plugins/com.geeksville.touchypad.log`).

### Documentation update (opendeck-device-plugin.md)

Add/extend sections covering what the reference example taught us:

* **Outbound calls.** Document both shapes: openaction-1
  `OUTBOUND_EVENT_MANAGER.lock().await.as_mut().register_device(id, name,
  rows, cols, encoders, device_type)` and openaction-2
  `device_plugin::register_device(...)`, plus `unregister_device(id)`,
  `key_down(id, key)`, `key_up(id, key)`, and the encoder variants. Note
  which version this repo targets and why.
* **Device identity & namespace.** Reiterate that every registered ID
  must start with the manifest's two-char `DeviceNamespace`; show the
  reference's `"{NAMESPACE}-{serial}"` scheme and ours
  (`"tp-{bus}{addr}"`), and the serial-stability caveat.
* **`device_type` byte.** Document what the trailing byte means and why
  we pass `0` (StreamDeck-Original-shaped 3×5 grid, no encoders).
* **Logging.** Spell out that the OpenAction transport is a TCP
  WebSocket (`ws://localhost:<port>` from the `-port` argv), **not** the
  plugin's stdio, and that OpenDeck redirects the plugin's stdout *and*
  stderr into `log_dir()/plugins/<plugin-uuid>.log`. So logging to
  stdout is safe (the reference does exactly that) and is where you go
  to debug "my plugin does nothing and I can't tell why". Recommend
  `simplelog`/`Info` to stdout to match the reference.
* **Lifecycle / hot-plug.** Document the watcher pattern: enumerate on
  `plugin_ready`, then a hot-plug watcher (`DeviceWatcher` in the
  reference; our 1 Hz `enumerate` diff), one cancellable task per
  device, and `unregister_device` on disconnect.

### Acceptance

1. With OpenDeck running and a Touchy-Pad plugged in, the plugin's log
   (visible to the user) shows the full chain: enumerate → candidate →
   attach → board info → grid → `register_device` → **Ok**.
2. The Touchy-Pad appears in OpenDeck's device list.
3. Pressing a key on the pad shows up as a key event in OpenDeck;
   setting a key image shows on the pad.
4. Unplug logs `detach` + `unregister_device` and removes the device;
   replug re-registers without restarting OpenDeck.
5. `cargo build` / `just rust-build` clean; `opendeck-device-plugin.md`
   updated with the sections above.

### Out of scope

* Encoder/dial support (no rotary inputs on current boards).
* Windows/macOS hot-plug semantics beyond enumerate-on-start.
* Marketplace submission (separate packaging stage).

### What was implemented

* **Logging init (Phase 2).** `main.rs` now logs to **stdout** (matching
  the reference `opendeck-akp153` plugin) via
  `simplelog::TermLogger::init(level, Config::default(),
  TerminalMode::Stdout, …)`, with the level read from `TOUCHY_LOG` /
  `RUST_LOG` (default `Info`). The previous "log to stderr so we don't
  corrupt the JSON socket" comment was wrong and was removed: the
  OpenAction link is a *separate* TCP WebSocket (`ws://localhost:<port>`),
  and OpenDeck redirects the plugin's stdout+stderr into
  `<log-dir>/plugins/<plugin-uuid>.log`.
* **Instrumentation (Phase 1).** `plugin.rs` now emits `log::info!` at
  every lifecycle step: watcher start, each enumeration poll (logging the
  candidate count + per-candidate VID/PID/bus/addr, and an explicit
  "no Touchy-Pad devices found" when empty, de-duplicated so the 1 Hz
  poll doesn't flood), attach (USB open → board info → computed grid →
  screen upload → event-task spawn), the `register_device` call **and its
  result**, every forwarded key event (`keyDown`/`keyUp`), detach/task
  cancellation/`unregister_device`, and inbound `set_image`/`set_brightness`.
* **Registration path verified (Phase 3).** Traced openaction 2.6.0:
  `run()` installs the outbound manager (`set_outbound_manager`) *before*
  `plugin_ready` fires, so `device_plugin::register_device` (which silently
  no-ops if the manager is unset) is always live by the time the hot-plug
  loop runs. The existing path is correct; no structural change was needed
  beyond logging the result. The per-device watcher/attach/detach structure
  already mirrors the reference's task model, so it was kept single-file
  rather than over-split.
* **Docs (Phase 5).** `README.md` and `docs/opendeck-device-plugin.md`
  corrected: TCP-WebSocket transport, stdout/stderr→logfile capture, the
  `tail -F` log path, v1 (`OUTBOUND_EVENT_MANAGER`) vs v2
  (`device_plugin::*` free fns) outbound APIs, and the "register only after
  `plugin_ready`" gotcha.

## Stage 71: touchy-opendeck → uscr, unified enumeration, device serials, RunActions

The `touchy-opendeck` plugin works but needs four refinements. Original
request:

> The touchydeck tool works pretty well but two refinements:
>
> **Change to uscr** — It is still using the old mechanism of writing a
> 'screen' file to host/s. Instead I want it to write its array of buttons
> to `host/uscr/opendeck.pb` (this new feature added in stage 68). One
> wrinkle is that currently there is no way for the host PC to know the
> vertical height that it has to work with when planning a widget layout —
> it only knows the display height. So for now (since touchy-opendeck NEEDS
> to know how much vertical space it has — so it can calculate # of rows of
> 'opendeck' buttons) — assume vertical space for its screen area of
> `display_height (from board-info) - TOP_ROW_HEIGHT`. Where TOP_ROW_HEIGHT
> is a symbolic constant that you set at 32 pixels for now...
>
> **Enumeration doesn't see sim devices** — The existing enumeration in this
> component only looks based on USB vid/pid, instead have it also check for
> the TOUCHY_SIM_URL env var and if that is set create an appropriate api
> client to reach that sim device. Preferably move this enumeration into our
> rust api library so that sim devices show up in the list of devices just
> like USB or serial devices...

Follow-up tweaks (this stage):

* **RAM-disk images are volatile.** Image assets live on the `R:` ramdisk
  (good — saves flash wear), so they vanish on a device reboot. The plugin
  must therefore **rewrite every key image (and, for good measure, the
  `uscr/opendeck.pb` body) every time it (re)connects** to a device or sim,
  not just on the first attach.
* **Every device has a serial; use it as the stable id.** Replace the
  bus/addr-derived id with a real serial number reported by the device.
* **Force the page to the front via a new `RunActionsCmd`** instead of the
  old `screen_load` trick.

### Part 1 — Write the button layout to `host/uscr/opendeck.pb`

Today [`layout::build_screen`](../rust/touchy-opendeck/src/layout.rs) returns
a full `Screen` (a `LayoutGrid` wrapped in `Screen.active`), uploaded to
`R:host/s/opendeck_{device_id}.pb` and activated with `screen_save` +
`screen_load` in [`plugin::attach`](../rust/touchy-opendeck/src/plugin.rs).

Changes:

* `layout.rs`: add `build_page(cols, rows) -> Widget` returning just the
  `LayoutGrid` widget (the current `active` widget, `version =
  Version::Current`). The page is now **shared** (one `opendeck.pb`), not
  per-device, so:
  * Drop `device_id` from the asset path:
    `asset_path_for(key) -> "R:host/opendeck/key_{key}.bin"`.
  * Replace `screen_path_for(device_id)` with `pub const PAGE_NAME:
    &str = "opendeck";` (on-device path `F:host/uscr/opendeck.pb`).
* `plugin.rs attach`: upload with `pad.user_screen_save(layout::PAGE_NAME,
  &page)` (already exists in [`pad.rs`](../rust/touchy-pad/src/pad.rs))
  instead of `screen_save`/`screen_load`. Remove `DeviceCtx.screen_path`.

### Part 2 — Available vertical space (`TOP_ROW_HEIGHT`)

The host only knows the *display* height, but the default chrome eats the
top row. Add `const TOP_ROW_HEIGHT: u32 = 32;` in `plugin.rs` and compute:

```rust
let usable_h = board.display_height.saturating_sub(TOP_ROW_HEIGHT);
let cols = display_width / KEY_PX;          // unchanged
let rows = usable_h     / KEY_PX;           // was display_height / KEY_PX
```

Keep the existing zero-dimension / smaller-than-one-key guards.

### Part 3 — Always rewrite images + page on (re)connect

Because the `R:` ramdisk is wiped on reboot, `attach` must re-push **all**
state every time it connects, and must not assume OpenDeck will resend
images:

* After `user_screen_save`, the plugin re-pushes whatever key images it
  has cached. OpenDeck owns the image bytes (it sends them via
  `set_image`), so the plugin keeps a per-key `HashMap<u8, Vec<u8>>` of the
  last bytes it received and, on attach, writes every cached entry back to
  `R:host/opendeck/key_{k}.bin`. (Before OpenDeck has sent any image the
  cache is empty and cells render blank — acceptable; OpenDeck repaints on
  profile select.)
* The `uscr/opendeck.pb` body is likewise (re)written on every attach.
* The device/sim **already** auto-redraws a widget when its backing file on
  the device FS changes, so simply re-saving the `.bin` files is enough to
  refresh the screen — no explicit reload call needed. `schedule_reload`
  and its debounce can be deleted along with `screen_load`.

### Part 4 — Device serial numbers + stable ids

All Touchy devices (real or sim) must report a serial, used directly as the
enumeration stable id.

* **Proto:** add `string serial = 9;` to `SysBoardInfoResponse` in
  [`proto/touchy.proto`](../proto/touchy.proto) (next free field after
  `has_usb = 8`). Regenerate Python + nanopb + Rust bindings
  (`just build-proto`). This is an additive scalar field — bump
  `ProtocolVersion.CURRENT` to `7` and add a `V7` doc line.
* **Firmware:** populate `serial` from `esp_read_mac()` formatted as
  `"t%02x%02x%02x%02x%02x%02x"` (leading `t` + 12 hex digits, no
  separators). Wire the **same** string into the USB descriptor
  `iSerialNumber`
  (`firmware/main/usb_hid.cpp`, the string-descriptor table) so the OS and
  the vendor transport agree. Compute it once at boot into a shared helper
  (e.g. `platform_serial()` in `firmware/main/platform.{h,cpp}`) consumed by
  both `host_api.cpp` (board-info response) and `usb_hid.cpp` (descriptor).
* **Simulator:** the sim's `sys_board_info_get` returns the constant
  `"tsim001"` for now (Python sim device + any Rust/host sim shim).
* **Stable id:** `enumerate`/`discover` use the reported `serial` as the id.
  touchy-opendeck still prepends its two-char OpenDeck `NAMESPACE`
  (`"tp"`), so registered ids become `tp-{serial}` (e.g.
  `tp-tsim001`). Fetching the serial requires opening the transport and
  calling `sys_board_info_get`, which the watcher already does in `attach`;
  the hot-plug diff key therefore becomes the serial rather than bus/addr.

### Part 5 — Unified enumeration in the rust api library (incl. sim)

Today [`transport_usb::enumerate(vid, pid)`](../rust/touchy-pad/src/transport_usb.rs)
returns only `Vec<nusb::DeviceInfo>`; the sim (`TOUCHY_SIM_URL`) is honoured
only by single-client `open`, never listed. Python already does combined
discovery in
[`touchydeck/discovery.py`](../app/src/touchy_pad/touchydeck/discovery.py).

New module `rust/touchy-pad/src/discover.rs` (re-exported from `lib.rs`):

```rust
pub enum DiscoveredDevice {
    Usb(nusb::DeviceInfo),
    Sim { host: String, port: u16 },
    // (Serial transport: future arm.)
}

impl DiscoveredDevice {
    pub fn describe(&self) -> String;                 // for logging
    pub async fn open(&self) -> Result<Arc<dyn Transport>>; // USB or TCP
}

/// Enumerate every locally reachable Touchy device: USB (proto vid/pid,
/// guarded against missing backend) plus a single sim entry when
/// `TOUCHY_SIM_URL` is set.
pub async fn discover() -> Result<Vec<DiscoveredDevice>>;
```

* `Usb::open` → `UsbTransport::open_info`; `Sim::open` →
  `TcpTransport::connect` (host/port from
  [`transport_net::sim_url_from_env`](../rust/touchy-pad/src/transport_net.rs)).
* The **serial** (Part 4) is the canonical stable id, so `discover()`
  intentionally does **not** invent ids from bus/addr; callers read the
  serial after opening. (`DiscoveredDevice` only needs enough to *open* the
  transport.)

touchy-opendeck rewire:

* `run_hotplug_loop` calls `touchy_pad::discover()` instead of
  `enumerate(vid, pid)`; the diff map is keyed by serial (obtained in
  `attach`). To keep the 1 Hz poll cheap it diffs on a lightweight key
  derived from `DiscoveredDevice` (USB bus/addr, sim host/port) to decide
  *whether to attach*, then resolves the true serial id during `attach`.
* `attach(dev: &DiscoveredDevice)` opens via `dev.open()` (USB and sim share
  one path), calls `sys_board_info_get`, derives `device_id =
  format!("{NAMESPACE}-{}", board.serial)`, then proceeds as today.

### Part 6 — `RunActionsCmd` (run actions as if locally sourced)

Add a new `Command` variant so the host can ask the device/sim to execute a
list of `Action`s exactly as if a local widget had fired them. This unlocks
(a) future test automation and (b) — the immediate need — letting the API
library force the `host/uscr/opendeck` body to the front by running an
`ActionChangeWidgetRef(BY_PATH, "F:host/uscr/opendeck.pb", target_id="page")`.

* **Proto** ([`proto/touchy.proto`](../proto/touchy.proto)):

  ```proto
  // Run a list of Actions device-side, exactly as if a local widget
  // had just triggered them. Enables host-driven automation and lets
  // the host retarget the active WidgetRef (e.g. show a uscr page).
  message RunActionsCmd {
      repeated Action actions = 1;
  }
  ```

  Add `RunActionsCmd run_actions = 11;` to `Command.oneof cmd`. `Action`
  already lives in `widgets.proto`; ensure `touchy.proto` imports it (it
  already pulls widget types in for `Screen`). Mark `actions` `FT_POINTER`
  in `touchy.options` (repeated message → heap, per the Stage 17 rule).
  Response is a plain `Response` with `RESULT_OK` / error.

* **Firmware:** `host_api.cpp` dispatches `run_actions` by iterating the
  decoded `actions` and feeding each into the **existing** action runner
  used when a widget fires (the same path `screens.cpp` / `macros.cpp`
  already call for `ActionHost` / `ActionMacro` / `ActionDevice`). Factor
  out a `run_action(const touchy_Action&)` entry point if one isn't already
  cleanly callable. `ActionDevice::ActionChangeWidgetRef` already mutates
  the live screen's ref, so this immediately works for paging.

* **Simulator:** mirror the same dispatch in the Python/Rust sim action
  runner so `RunActionsCmd` works headless.

* **Host API:** add `client.run_actions(actions)` (Rust `client.rs`,
  Python `client.py`) plus a convenience on the high-level pad, e.g.
  `pad.show_user_screen("opendeck")` →
  `run_actions([ActionDevice(ActionChangeWidgetRef(BY_PATH,
  "F:host/uscr/opendeck.pb", target_id="page"))])`. touchy-opendeck calls
  this after `user_screen_save` so the grid jumps to the front on attach.

* **Docs (be careful here):** document `RunActionsCmd` in
  [docs/host-api.md](host-api.md) (new command, payload shape, that actions
  run identically to locally-sourced ones, and the `ActionChangeWidgetRef`
  paging use case) and in [docs/python-api.md](python-api.md) /
  [docs/rust-api.md](rust-api.md) for the new client method. Bump the
  protocol-version note.

### Tests / build / docs

* Rust: `discover.rs` unit tests (sim-from-env set/unset, describe);
  `layout.rs` tests updated to `build_page` (assert `LayoutGrid` widget +
  rows reflect the `-32 px` reduction); `cargo test` / `just rust-build`
  green.
* Python: sim returns `"tsim001"` serial; `run_actions` round-trip test;
  `just app-test` green.
* Firmware: builds for current board with the new serial + `RunActionsCmd`
  paths.
* Docs: update [opendeck-device-plugin.md](opendeck-device-plugin.md) (uscr
  page model, serial-based ids, unified enumeration incl. sim, always-rewrite
  on reconnect), [host-api.md](host-api.md), [python-api.md](python-api.md),
  [rust-api.md](rust-api.md), and append a "what was implemented" block here.

### Acceptance

1. With a Touchy-Pad (or sim via `TOUCHY_SIM_URL`) connected, OpenDeck shows
   the device; its id is `tp-<serial>` and the serial matches both
   `SysBoardInfoResponse.serial` and the USB `iSerialNumber`.
2. The button grid lands in `F:host/uscr/opendeck.pb` and is brought to the
   front via `RunActionsCmd` (no `screen_load`).
3. Rebooting the device and letting the plugin reconnect re-pushes the page
   and every cached key image with no user action.
4. Sim devices appear in `discover()` exactly like USB devices.
5. `RunActionsCmd` documented in host-api / python-api / rust-api and works
   for both a real device and the sim.

### Out of scope

* Serial-port transport arm of `DiscoveredDevice` (future).
* Per-device (vs. shared) uscr pages — one `opendeck.pb` for now.
* Encoder/dial support.

### What was implemented

Done as planned, with these concrete landing points:

* **Proto.** `RunActionsCmd { repeated Action actions = 1; }` (oneof
  field `run_actions = 11`) and `string serial = 9` on
  `SysBoardInfoResponse`; `ProtocolVersion` gained `V7` /
  `CURRENT = 7`. `touchy.proto` now `import "widgets.proto";` (it needs
  `Action`). `touchy.options` marks `RunActionsCmd.actions` `FT_POINTER`
  and caps `serial` at `max_size:24`. Because the new import generates a
  cross-file reference, `build-proto-py` gained a `sed` step rewriting the
  flat `import widgets_pb2` to a relative `from . import widgets_pb2`, and
  `proto/embed_screen_json.py` now imports the bindings via the package.
  `RunActionsCmd` is re-exported from `app/.../_proto/__init__.py`.
* **Firmware.** New `firmware/main/platform.cpp` `platform_serial()` derives
  the serial from `esp_read_mac(…, ESP_MAC_BASE)` as `"t"+12 hex` (cached
  static); `usb_hid.cpp` feeds it into the `iSerialNumber` string
  descriptor and `host_api.cpp`'s `fill_board_info()` copies it into
  `serial`. `widget_actions.{h,cpp}` were refactored to expose
  `widget_run_action()` (the single-action switch, formerly inline in
  `widget_event_cb`) and `widget_run_actions(actions, count)` which takes
  the LVGL port lock and runs each as a synthetic `LV_EVENT_CLICKED`;
  `host_api.cpp` dispatches `run_actions` through it. `CMakeLists.txt` adds
  `platform.cpp` + `esp_hw_support`.
* **Simulator.** `SimDevice` reports `serial = "tsim001"` and handles
  `RunActionsCmd` via a new `set_run_actions_callback`; the Qt window
  registers a dispatcher so paging actions repaint, and headless callers
  fall back to running `ActionHost`s inline (so host events still flow).
* **Host APIs.** `client.run_actions(actions)` (Python + Rust). Rust
  `Touchy::show_user_screen(name)` issues a `RunActionsCmd` carrying
  `ActionChangeWidgetRef(BY_PATH, "F:host/uscr/<name>.pb",
  target_id="page")`.
* **Unified discovery.** `rust/touchy-pad/src/discover.rs` adds
  `DiscoveredDevice { Usb, Sim }` with `describe()` / `open()` and
  `discover()` (USB + `TOUCHY_SIM_URL` sim), re-exported from `lib.rs`.
* **touchy-opendeck.** `layout::build_page(cols, rows) -> Widget`,
  `PAGE_NAME = "opendeck"`, shared `asset_path_for(key) ->
  R:host/opendeck/key_{key}.bin`, `device_id_for(serial) -> tp-<serial>`.
  `plugin.rs` now polls `discover()` (diffed on the transport-level
  `describe()` key), derives the OpenDeck id from the serial, uploads the
  page via `user_screen_save` + `show_user_screen`, caches received key
  bytes in a `HashMap<u8, Vec<u8>>` (re-pushed on reconnect since `R:` is
  volatile), and computes `rows` from `display_height - TOP_ROW_HEIGHT`
  (32 px). `schedule_reload` / `screen_load` were removed.

Firmware was **not** compiled in the implementing environment (no ESP-IDF
in PATH); all field names were cross-checked against the regenerated
`firmware/main/proto/touchy.pb.h`. Rust (`cargo build`/`test`/`clippy`),
Python (`just app-test`, 160 passing), and lint are all green.

## Stage 72: opt-in sizing via `Widget.grow_x` / `grow_y` — DONE

**Problem.** Flex/grid children stretched to fill by default: `apply_rect`
forced COLUMN flex children to `lv_pct(100)` width (an implicit cross-axis
fill) and `apply_grid_cell` always used `LV_GRID_ALIGN_STRETCH`. Buttons in
a row therefore ballooned to the full row width instead of shrinking to
their label. There was no way to ask for content-sizing.

**Decision (locked with the user).** Sizing becomes **opt-in**. Two new
`int32` fields — `grow_x` and `grow_y` — live on **`Widget` itself** (not on
`Rect`/`GridCell`), so they apply uniformly regardless of the `placement`
oneof. Default `0` = content-sized. The old `Rect.flex_grow` field
(introduced in Stage 68) is **removed** (pre-1.0, clean break — no
back-compat shim). `Widget.Version.CURRENT` bumped 19→20; the transport
`ProtocolVersion` is unchanged.

**Semantics** (depend on parent flow + axis):
* **Flex main axis** — `grow_x` under a ROW parent, `grow_y` under a COLUMN
  parent — maps to `lv_obj_set_flex_grow(obj, factor)`; the value is a
  proportional weight, so magnitude matters.
* **Flex cross axis** (`grow_y` in a ROW, `grow_x` in a COLUMN) and **grid**
  (both axes) treat any value `> 0` as "fill" (magnitude ignored):
  cross-axis flex → `lv_pct(100)`; grid → `LV_GRID_ALIGN_STRETCH`. Grid
  cells now **CENTER** by default (previously always stretched). The
  implicit COLUMN cross-fill is gone — callers must set `grow_x=1` for
  full-width COLUMN children.

**Implementation.**
* `proto/widgets.proto` (+ Rust copy): `Rect` reverted to 4 fields;
  `grow_x = 8` / `grow_y = 9` added to `Widget`; version bumped to 20.
* Firmware `firmware/main/widgets/screen_layout.cpp`: `apply_rect` now
  computes parent flow (`parent_layout` arg, also stored on `ActiveRef` for
  the Stage-57 widget-ref rebuild) and applies main-axis `lv_obj_set_flex_grow`
  + cross-axis `lv_pct(100)`; `apply_grid_cell` keys per-axis
  `LV_GRID_ALIGN_STRETCH` vs `CENTER` off `grow_x`/`grow_y`.
* Python DSL `app/src/touchy_pad/api/screens.py`: new `grow(widget, *, x=,
  y=)` helper; `cell(..., grow_x=, grow_y=)`; `build_default_screen()` uses
  `grow()` on the chrome row, the `chrome_gap` spacer, and the page body;
  `build_user_pages()` showcase grid cells set `grow_x=1, grow_y=1` to keep
  their filled look. Re-exported from `touchy_pad.api`.
* Simulator `sim/widgets.py`: grid uses per-axis Qt alignment (center unless
  grow>0); flex maps main-axis grow to a Qt stretch factor and pins the
  cross axis unless cross-grow is set.
* `proto/default_screen.json` + `firmware/main/default_screen_pb.h`
  regenerated from the DSL (`growX`/`growY` on the chrome/spacer/body).
* Rust `touchy-demo` Rect literal dropped the obsolete `flex_grow`.

Validated: `just build-proto`, `just app-test` (160 passed), `just app-lint`
(clean).

## Stage 80: GIF support — DONE

**Goal.** Let an animated GIF be used anywhere an `Image` widget can — most
visibly as a `touchpad image URL` background — without inventing a new
protobuf widget or a new host file format. A `.gif` source is uploaded to
the device filesystem *as-is* (no LVGL-bin conversion) and the firmware
renders it with LVGL's `lv_gif` widget when the image path ends in `.gif`.

**Why no new protobuf variant.** GIFs reuse the existing `Image` message
(`proto/widgets.proto`, `path`/`scale`/`rotation`/`align`). The path
extension (`.gif`) is the discriminator on both sides. `lv_gif` is a thin
wrapper around `lv_image`, so scale/rotation/align still apply. This keeps
`Widget.Version` unchanged and avoids touching the DSL `image()` factory's
signature.

### Reference
* Widget guide: https://lvgl.io/docs/open/9.5/widgets/gif
* API: https://lvgl.io/docs/open/9.5/API/widgets/gif/lv_gif_h.html

The vendored `lvgl__lvgl` managed component already ships
`lv_gif_set_src` / `lv_gif_set_color_format` /
`lv_gif_set_auto_pause_invisible`
(`firmware/managed_components/lvgl__lvgl/src/widgets/gif/lv_gif.c`), so no
component bump is needed — only the Kconfig switch.

### 1. Firmware — enable the decoder
* `LV_USE_GIF` is currently `0` everywhere (LVGL template default + every
  `firmware/sdkconfig.<board>`). Add `CONFIG_LV_USE_GIF=y` to
  `firmware/sdkconfig.defaults` so all boards pick it up, and refresh the
  per-board `sdkconfig.<board>` copies (the `# CONFIG_LV_USE_GIF is not set`
  line flips to `CONFIG_LV_USE_GIF=y`). `lv_gif` depends on the in-tree GIF
  decoder; no `LV_USE_LODEPNG`-style extra needed. RamFs already keeps the
  raw GIF bytes alive, so the decoder reads straight from `R:`/`F:`.
  **Gotcha (repo memory):** in `sdkconfig.defaults`,
  `# CONFIG_X is not set` is the `X=n` directive — set the positive form
  explicitly. Verify the on-device heap impact (RGB565 frame buffer ≈
  `w*h*2` bytes; 180×180 ≈ 65 KB — fits CYD internal RAM).

### 2. Firmware — render `.gif` as `lv_gif`
* `firmware/main/widgets/widget_builders.cpp`: in `build_image()` (and the
  pressed/released slots of `build_image_button()` if reused), branch on
  `path_ends_with(w.kind.image.path, ".gif")`:
  * create `lv_gif_create(parent)` instead of `lv_image_create`;
  * **before** setting the source, call
    `lv_gif_set_color_format(obj, LV_COLOR_FORMAT_RGB565)` (smaller RAM
    buffers, matches the panel) and
    `lv_gif_set_auto_pause_invisible(obj, true)` (stop animating when the
    page is hidden — important for the `widget_ref` page flipper);
  * set src via `lv_gif_set_src(obj, to_lvgl_path(path).c_str())` using the
    existing `to_lvgl_path()` helper (drive-prefix → LVGL `F:/…`). The
    Stage-52 mmap fast path in `apply_image_src_from_path()` is **skipped**
    for GIFs (it only handles `lv_image_dsc_t` LVGL-bin blobs).
  * scale/rotation/align: `lv_gif` exposes the underlying image via the same
    `lv_image_set_scale/rotation/align` setters (a gif *is* an image), so
    reuse `apply_image_attrs` minus the src step. Confirm at build time that
    these compile against `lv_gif_t`.
* Factor the "is this a gif" check into one helper so the plain-image and
  widget-ref rebuild paths (`ActiveRef`) agree.

### 3. Host — upload GIFs unconverted
* `app/src/touchy_pad/api/lvgl_image.py`:
  * GIF is already in `_IMAGE_MAGICS` (`GIF87a`/`GIF89a`) and
    `_CONVERTIBLE_EXTS`. Add an `is_gif(data)` / `looks_like_gif(path)`
    helper.
  * `rewrite_to_bin_path()` must **not** rewrite `.gif` → `.bin` (the device
    keeps the `.gif` extension as its discriminator). Special-case `.gif` to
    pass through unchanged, and drop it from the set of extensions that get
    rewritten.
* `app/src/touchy_pad/client.py` `file_save()`: when the payload is a GIF,
  skip `to_lvgl_bin()` and the path rewrite — write the raw bytes. If
  `max_width`/`max_height` are set and the GIF exceeds them, rescale **every
  frame** (preserving the animation, duration, loop, and disposal) via
  `PIL.ImageSequence` and re-encode as GIF, then upload that. If no
  size limit is given, upload byte-for-byte. Keep the existing
  `needs_image_conversion` transport guard.
* `app/src/touchy_pad/api/screens.py` `_fill_image()`: today it always calls
  `rewrite_to_bin_path(asset)`. With the `.gif` pass-through above this
  becomes correct automatically (`F:host/…/x.gif` stays `…/x.gif`), so the
  DSL `image(asset="…/foo.gif")` and the trackpad background just work.

### 4. CLI — `touchpad image URL` with GIFs
* `app/src/touchy_pad/cli.py` `touchpad_image()` already fetches arbitrary
  bytes and calls `pad.file_save(_USER_BG_IMG_PATH, data, max_width=180,
  max_height=180)`. Two changes:
  * `_USER_BG_IMG_PATH` is hard-coded to `…/user-background.bin`. Pick the
    on-device extension from the fetched bytes: `.gif` for GIF sources,
    `.bin` otherwise (so the firmware's extension check fires). Pass the
    chosen path to both `file_save()` and `_do_write_trackpad()`.
  * The 180×180 cap then flows through the GIF frame-rescale path in (3).
* Verify the fetched-bytes UA workaround (Cloudflare 403 fix) still applies.

### 5. Simulator
* `app/src/touchy_pad/sim/widgets.py` `_load_pixmap()` already falls through
  to `QPixmap.loadFromData()`, which renders the *first* GIF frame — good
  enough for a static preview. Optional polish: use `QMovie` for animation
  in `_build_image` when the asset ends in `.gif`. Minimum bar: the sim
  must not crash and should show frame 0. Ensure the `.bin`→source-ext
  candidate list in `_load_pixmap` keeps `.gif`.

### 6. Tests
* `app/tests/test_lvgl_image.py`: add cases that `looks_like_supported_image`
  accepts a tiny GIF, that `rewrite_to_bin_path("F:…/a.gif")` returns the
  `.gif` path unchanged, and that a GIF is **not** run through `to_lvgl_bin`.
* `app/tests/test_client.py` (or `test_images.py`): a fake-transport
  `file_save` of a small multi-frame GIF writes the raw GIF bytes to a
  `.gif` path (no conversion); with `max_width`/`max_height` smaller than the
  GIF it still decodes as an animated GIF with the same frame count, scaled.
* Build a small animated test GIF helper (mirror `make_smiley_png`) if a
  fixture is needed.

### Validation
`just build-proto` (no proto change expected), `just app-test`,
`just app-lint`, and a `just firmware-build` for one S3 board + one CYD board
to confirm `LV_USE_GIF` links and the heap budget holds. Manual smoke test:
`touchy touchpad image <gif-url>` then reload the default screen.

### What was implemented

Done as planned, with these concrete landing points:

* **Firmware Kconfig.** `CONFIG_LV_USE_GIF=y` added to
  `firmware/sdkconfig.defaults` and flipped on in every per-board
  `sdkconfig.<board>`. LVGL allocates the GIF frame buffer via its CLIB
  malloc, which prefers PSRAM on boards that have it (~65 KB for a
  180×180 RGB565 frame).
* **Firmware render — no copy-paste.** `widget_builders.cpp` gained
  `path_is_gif()` (case-insensitive `.gif` suffix, gated `#if LV_USE_GIF`).
  `build_image()` only differs in the object class
  (`lv_gif_create` vs `lv_image_create`); the GIF-specific setup —
  `lv_gif_set_color_format(RGB565)` (before src, to avoid a framebuffer
  realloc) + `lv_gif_set_auto_pause_invisible(true)` + `lv_gif_set_src` —
  lives in the **shared** `apply_image_src_from_path()`. So scale/rotation/
  align (`apply_image_attrs`) and the Stage 60 reload-on-overwrite registry
  (`widget_image_registry_notify`) both work for GIFs unchanged.
* **Re-upload over a playing GIF.** `lv_gif`'s decoder holds the source
  file open for the whole animation, so a flash commit (which renames the
  temp file over the destination) failed with EBUSY when re-uploading a GIF
  that was already on screen. `file_close` now calls
  `screens_prepare_file_overwrite()` →
  `widget_image_registry_release_gif()` (`lv_gif_set_src(img, NULL)`) just
  before `fs_close_write()`, closing the handle so the rename succeeds; the
  existing `screens_notify_file_changed()` re-applies the source afterward,
  reopening the freshly-written file.
* **Host.** `api/lvgl_image.py`: new `is_gif()` / `rescale_gif()`; `.gif`
  removed from `_CONVERTIBLE_EXTS` so `rewrite_to_bin_path()` preserves it.
  `client.file_save()` detects GIFs and uploads them verbatim (no
  `to_lvgl_bin`), rescaling every frame via `PIL.ImageSequence` only when
  `max_width`/`max_height` is given. `screens.py::_fill_image()` keeps `.gif`
  paths automatically via the pass-through.
* **CLI.** `touchpad image URL` picks `…/user-background.gif` for GIF
  sources (else `…/user-background.bin`), passing the chosen path to both
  `file_save()` and `_do_write_trackpad()`. The 180×180 cap flows through
  `rescale_gif`.
* **Simulator.** Unchanged — `_load_pixmap` already falls through to
  `QPixmap.loadFromData()`, which renders the GIF's first frame as a static
  preview; the `.bin`→source-ext candidate list still includes `.gif`.
* **Tests.** `test_lvgl_image.py` (GIF detection, `.gif` path passthrough,
  frame-preserving rescale) and `test_client.py` (GIF uploaded verbatim to a
  `.gif` path). `just app-test` 165 passing, `just app-lint` clean.

Firmware was **not** compiled in the implementing environment (no ESP-IDF in
PATH); the `lv_gif_*` APIs were verified present in the vendored
`lvgl__lvgl` component and exported via the `lvgl.h` umbrella.

## Stage 81: boardinfo improvements
include free RAM, PSRAM and flash-file-system numbers in the boardinfo protobuf.  have python tool print it

## stage 82: preferences improvements
The current approach for changing preferences is brittle - it requires a new message type every time we want to let a preferences value be updated.  Change it.  Instead add a new SetPreferencesCmd(PreferencesFile prefs).

* Change all the fields in PreferencesFile protobuf to be "optional"
* The device or sim will respond to this message by setting any specified field in the master settings used on the device. The host should never include a file_version" field in messages it sends.  It should only set entries it wants changed.
* On the device code if the host changes a preferences entry make sure to trigger any necessary state changes (e.x. backlight level)
* remove ScreenSleepTimeoutCmd - instead use our new SetPreferencesCmd
* Allow setting a persistent device pref for 'min-log-level' logs with lower pri than this will not be queued for the host, just drop em.  default threshold is ERROR. 
* Update python cli to add a "set-log-level FOO" cmd.  
* Also add a persistent "boot-delay(num_seconds)" preferences, to cause a sleep early on - to allow time for debug logging connection establishment.
* Remove ScreenLoadCmd, because the host should instead include API glue that uses the SetPreferencesCmd to change that field

# Old/Existing projects

In the very early days of this project I looked into these ideas/implementations:

FreeTouchDeck - code seems a bit yucky and limited to just a button array, possibly not reuse...
* Does the button portion already?  semi abandoned?  https://github.com/DustinWatts/FreeTouchDeck https://hackaday.io/project/175827/instructions 
* Newer platformio version of that project: https://github.com/dejavu1987/FreeTouchDeck 
* This old abandoned helper app: https://github.com/DustinWatts/FreeTouchDeck-Helper 

BLE mouse platform IO lib
https://registry.platformio.org/libraries/hijelhub/HijelHID_BLEMouse
Or this more bare BLE lib that works more like the USB mouse lib:
https://registry.platformio.org/libraries/leollo98/ESP32%20BLE%20Mouse%20With%20Precision%20Scroll

USB mouse library - probably use this for first test then extend it as needed to become a real touchpad lib.
https://registry.platformio.org/libraries/arduino-libraries/Mouse

Also this gamepad lib
https://github.com/lemmingDev/ESP32-BLE-Gamepad