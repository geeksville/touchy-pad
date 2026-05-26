# TODO

## For release Alpha 1

Main new features: works as a touchpad, with some basic ability to add buttons (StreamDeck-like) via the companion app

* [x] Decide between QMK/ZMK/platformio base... (anyone already support the full touchpad USB spec?) - for now I'm using platformio
* [x] make a mini PRD/design doc
* [x] add datasheets
* [x] remove platformio
* [x] Ping Chris Kirmse
* [x] Fix screen drawing on new device
* [x] Specify key classes
* [x] Make platformio shell
* [x] Make Python helper app (share code with my streamdeck project)
* [x] make a CLAUDE.md file
* [x] test reverse engineer tool with latest sim firmware, make sure it is storing images in ram
* [x] Add credits for BSD licensed https://github.com/moononournation/Arduino_GFX/blob/master/src/display/Arduino_NV3041A.cpp
* [x] add AI statement
* [x] Make README not suck, mention goals, simulator, api, TouchyDeck, next steps
* [x] fix touchqscroll handle
* [x] Make a nice demo video
* [x] build in esp installer based on automated hw model name
* [x] Update the default screen JSON
* [x] Make nice easy install instructions
* [x] Make StreamController support proof of concept
  * [x] test current POC
  * [x] make a little video
  * [ ] explain plans w.r.t. 3d printing and knobs on the screen
  * [x] request feedback/propose distribution
* [x] ensure no 10ms delay on loop polling
* [x] Pick a real USB VID/PID for our device via https://github.com/espressif/usb-pids/pull/315
* [x] fix multitouch gestures - the change to lvgl instead of polling broke them
* [ ] determine USB security issues (might need to set a secure key for future API operations - to prevent untrusted users from changing macro behavior)

## For release first public Alpha 2

Main new features: Works with StreamController app to provide arbitrary user buttons

* [x] make switchable subscreens by using widgetref?
* [x] remove Screen.path?  I don't think we need it
* [x] test https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/lcd/index.html to see if it could help rendering performance - NO
* [ ] investigate [opendeck](docs/opendeck.md) and possibly do a plugin for that instead-of or in-addition-to StreamController.
* [ ] add some of the example JSON to the API docs
* [ ] add a widget handle concept so host can say to just redraw one particular widget (only need to implement for screens/layouts/image)
* [x] make screen sleep default timeout
* [x] Add support for F:widgets/foo.pb widget files.  This would allow TouchyDeck to own a R:widgets/deck.pb.  Which could be nested into a F: screen.  So that dynamic StreamDeck emulated stuff could be updated independently of the configurable screens the user has selected.  Probably should add a way for the host to see dimensions of arbitrary widgets in screens/walk the screen list?
* [ ] Built-in [StreamController](https://streamcontroller.github.io/docs/latest/) support.  Probably via the mock device proof of concept.
* [x] Turn off "Expensive debugging flags!" in sdkconfig.defaults
* [ ] Support a few more board types
* [ ] implement the StreamDeck background graphic API

## For release Alpha 3

Main new features? Much easier scripting than through StreamController - allow arbitrary Python snippets for button presses/slider moves etc...  Dynamic data displays from host to Touchypad (server stats, ZMK modes, whatever user wants to show)

* [ ] add an animation to demo/test.pb https://lvgl.io/docs/open/9.5/main-modules/animation.html 
* [ ] Implement multitouch HID to support multitouch native apps
* [ ] Stylus support for 'paintbrush mode'
* [ ] support turn, back, forward, up gestures natively
* [ ] tactile precut sticker/3d printable case for screen overlay?
* [ ] figure out best way to mount haptics for best effect
* [ ] Increase CPU, FLASH and RAM speeds to the max.  Currently the firmware picks slow/safe defaults
* [ ] Haptics
* [ ] Use my auto-populated per-app button standard page generator - Default to use the material design icons
* [ ] Possibly no need for the full StreamController app, just go straight from per app yaml to an optional Python helper app (use to set icons/shortcuts etc).