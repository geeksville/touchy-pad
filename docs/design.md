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

## Stage 80: development environment improvements
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

## Stage 64.2: Make CDCACM optional
If CONFIG_TINYUSB_CDC_COUNT is zero, do not create cdcacm devices in usb_hid.  This will help us save endpoints in our device.  This will also work well with our stage 64.1 added ability to send our logs over our private protobuf based endpoints.

## Stage 64.3: add support for CYD2USB board

See [here](hardware.md) for specs.  Somethings to note about this board:

* Try to find 'built-in'/standard ESP32 drivers for the display and touch screen if you can
* This board is ESP32 (not ES32-S3 based) so make sure the sdkconfig for the board sets that up correctly (no direct USB access so no USB code to be included, no PSRAM)
* Because theres no USB you'll need to use the board UART for flash programming
* Initially, run the app debug output on that UART but once we've debugged the basics, I'm going to ask you to move our prioritary protobuf based protocol to be on that port instead (same wire encoding as we used for our TCP link to the simulator)
* The touchscreen is resistive and has no multitouch.  To support this (and anticipate boards of the future):
  * make a platform.h/.cpp class in the main code.   Boards will instantiate their own correct subclass which callers can access by platform_get().
  * Add a is_multitouch() method or property to that class.  The prior boards will return true, this ESP32-2432S028R board will return false.  Have our sim trackpad class check for that property and only try to do multitouch (or anything needing more than 'left' press/drag) on the older boards. 
  * Add a has_usb() method that indicates that this board has direct USB port access to the host.  The old boards do, this CYD2USB does not.


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