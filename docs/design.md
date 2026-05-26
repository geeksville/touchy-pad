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

## Stage 80: development environment improvements
* Support running a sim on the linux host?
* Use https://lvgl.io/docs/open/debugging/gdb_plugin to faciltiate debugging

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