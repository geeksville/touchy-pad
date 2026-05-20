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
* CLI: `touchy screens push SCRIPT [--load NAME]` runs the Python file,
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
* `LvEvent.user_data` / `LvEvent.extra`: copied by value through the
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

## Stage 18: Touchpad widget cleanup

* Move our existing touchpad widget proof of concept device code into a python accessible/protobuf configured Touchpad widget.  Any user touches inside that widget will be used to send mouse/touchpad HID events to the host.

## Stage 20: Beginning of sim-keyboard supprt.  Appears on host as a USB HID keyboard device.  

Use lv_buttonmatrix to provide matrixes of buttons

## Stage 21: Allow host PC to configure the button matrixes/screen layout
* Use protocol buffers (nanopb?) to communicate between the host/device (over a custom USB characteristic)
* Provide a simple python library to allow host applications to easily configure the button matrixes/screen layout
* Provide a simple python CLI tool to allow users to easily configure the buttons via that library

## Stage 30: development environment improvements
* Support running a sim on the linux host?
* Use https://lvgl.io/docs/open/debugging/gdb_plugin to faciltiate debugging
