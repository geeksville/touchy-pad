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

## Stage 20: Beginning of sim-keyboard supprt.  Appears on host as a USB HID keyboard device.  

Use lv_buttonmatrix to provide matrixes of buttons

## Stage 21: Allow host PC to configure the button matrixes/screen layout
* Use protocol buffers (nanopb?) to communicate between the host/device (over a custom USB characteristic)
* Provide a simple python library to allow host applications to easily configure the button matrixes/screen layout
* Provide a simple python CLI tool to allow users to easily configure the buttons via that library

## Stage 30: development environment improvements
* Support running a sim on the linux host?
* Use https://lvgl.io/docs/open/debugging/gdb_plugin to faciltiate debugging
