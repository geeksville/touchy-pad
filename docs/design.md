# General design goals/requirements

* Use platform.io as the build environment (vscode based, with an 'arduinoish' api)
* Use LVGL as the rendering library (provides layers and GUI primitives)
* Primiarily use C++ as the programming lanuage

# Target hardware

* The initially targeted hardware is https://docs.waveshare.com/ESP32-S3-Touch-LCD-7, eventually many other similar boards will be supported.  For more information on this board see https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-7B

# Development stages

Development will proceeed in a series of stages.  Currently none of these stages are implemented, after implementing each stage update this document as appropriate.

## Stage 0: Stub appload builds under platformIO ✓ DONE

At this point a basic hello world app builds on plaformIO and runs on the target hardware.  It prints 'hello world' using the arduino console.

## Stage 1: Helloworld extended to support LCD display ✓ DONE

Prints 'hello world' on the LCD screen of the devboard
Use the https://registry.platformio.org/libraries/iamfaraz/Waveshare_ST7262_LVGL platformIO library to interface with the LCD screen, and use LVGL to render the text.

## Stage 10: Appears on the host as a USB HID mouse device.  

## Stage 11: Multitouch gestures supported
* one finger tap = left click
* two finger tap = right click
* three finger tap = middle click
* one finger drag = mouse move
* two finger drag = scroll (left/right = horizontal scroll, up/down = vertical scroll)

## Stage 20: Beginning of sim-keyboard supprt.  Appears on host as a USB HID keyboard device.  

Use lv_buttonmatrix to provide matrixes of buttons

## Stage 21: Allow host PC to configure the button matrixes/screen layout
* Use protocol buffers (nanopb?) to communicate between the host/device (over a custom USB characteristic)
* Provide a simple python library to allow host applications to easily configure the button matrixes/screen layout
* Provide a simple python CLI tool to allow users to easily configure the buttons via that library

## Stage 30: development environment improvements
* Support running a sim on the linux host?
* Use https://lvgl.io/docs/open/debugging/gdb_plugin to faciltiate debugging
