# General design goals/requirements

* Use platform.io as the build environment (vscode based, with an 'arduinoish' api)
* Use LVGL as the rendering library (provides layers and GUI primitives)
* Primiarily use C++ as the programming lanuage
* Use C++ classes as needed for new big systems

# Target hardware

* The initially targeted hardware is https://docs.waveshare.com/ESP32-S3-Touch-LCD-7, eventually many other similar boards will be supported.  For more information on this board see https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7B

* jc4827w543
The JC4827W543 (often sold under names like Guition or Sunton) is an ESP32-S3 board paired with a 4.3" 480x272 RGB display (using an NV3041A or ST3401A controller) and usually a GT911 capacitive touch chip.
A word of caution on this specific board: Make absolutely sure you initialize the backlight pin (usually GPIO 2) to HIGH in your setup() loop immediately. Many people think their LVGL implementation is broken, but they just forgot to turn the backlight LED on.
for working example code see https://github.com/thelastoutpostworkshop/JC4827W543_radio_lvgl/blob/main/JC4827W543_radio_lvgl.ino
lvgl conf https://github.com/lsdlsd88/JC4827W543/tree/main/1-Demo/Demo_Arduino/3_3-3_TFT-LVGL-Widgets 
for detailed hw docs https://github.com/profi-max/JC4827W543_4.3inch_ESP32S3_board

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
* Host DSL: [`touchy_pad.screens`](../app/src/touchy_pad/screens.py)
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
* `app/src/touchy_pad/screens.py` exposes matching `trackpad(...)` and
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
* **Host DSL (`app/src/touchy_pad/screens.py`):** new
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
* **Host DSL** ([app/src/touchy_pad/screens.py](../app/src/touchy_pad/screens.py)):
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
* **Host DSL** ([app/src/touchy_pad/screens.py](../app/src/touchy_pad/screens.py)):
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

## Stage 21: Allow host PC to configure the button matrixes/screen layout
* Use protocol buffers (nanopb?) to communicate between the host/device (over a custom USB characteristic)
* Provide a simple python library to allow host applications to easily configure the button matrixes/screen layout
* Provide a simple python CLI tool to allow users to easily configure the buttons via that library

## Stage 30: development environment improvements
* Support running a sim on the linux host?
* Use https://lvgl.io/docs/open/debugging/gdb_plugin to faciltiate debugging
