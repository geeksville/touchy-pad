# TODO

(Items preceeded by ! are completed alredy)

## For release Alpha 1

Main new features: works as a touchpad, with some basic ability to add buttons (streamdeck like) via the companion app

* !Decide between QMK/ZMK/platformio base... (anyone already support the full touchpad USB spec?) - for now I'm using platformio
* !make a mini PRD/design doc
* !add datasheets
* !remove platformio
* !Ping Chris Kirmse
* !Fix screen drawing on new device
* !Specify key classes
* !Make platformio shell
* !Make python helper app (share code with my streamdeck project)
* !make a CLAUDE.md file
* !test reverse engineer tool with latest sim firmware, make sure it is storing images in ram
* Add credits for BSD licenced https://github.com/moononournation/Arduino_GFX/blob/master/src/display/Arduino_NV3041A.cpp
* a!dd AI statement
* Make README not suck, mention goals, simulator, api, TouchyDeck, next steps
* Make a nice demo video
* !build in esp installer based on automated hw model name
* fix touchqscroll handle
* Update the default screen JSON
* Make nice easy install instructions
* Make streamcontroller support proof of concept
  * test current POC
  * make a little video
  * explain plans wrt 3d printing and knobs on the screen
  * request feedback/propose distribution
* !ensure no 10ms delay on loop polling
* Pick a real USB VID/PID for our device with https://github.com/espressif/usb-pids, send them a PR
* !fix multitouch gestures - the change to lvgl instead of polling broke them
* determine USB security issues (might need to set a secure key for future API operations - to prevent untrusted users from changing macro behavior)

## For release first public Alpha 2

Main new features: Works with StreamController app to provide arbitrary user buttons

* add some of the example JSON to the API docs
* add a widget handle concept so host can say to just redraw one particular widget (only need to implement for screens/layouts/image)
* make screen sleep default timeout
* Add support for F:widgets/foo.pb widget files.  This would allow TouchyDeck to own a R:widgets/deck.pb.  Which could be nested into a F: screen.  So that dynamic StreamDeck emulated stuff could be updated independently of the configurable screens the user has selected.  Probably should add a way for the host to see dimensions of arbitary widgets in screens/walk the screen list? 
* Stylus support for 'paintbrush mode'
* Implement multitouch hid to support multitouch native apps
* Built-in [StreamController](https://streamcontroller.github.io/docs/latest/) support.  Probably via the mock device proof of concept.
* Turn off "Expensive debugging flags!" in sdkconfig.defaults
* Support a few more board types
* implement the streamdeck background graphic API

## For release Alpha 3

Main new features? Much easier scripting than through streamcontroller - allow arbitrary python snippets for button presses/slider moves etc...  Dynamic data displays from host to Touchypad (server stats, ZMK modes, whatever user wants to show)

* support turn, back, forward, up gestures natively
* tactile precut sticker/3d printable case for screen overlay?
* figure out best way to mount haptics for best effect
* Increase CPU, FLASH and RAM speeds to the max.  currently the firmware picks slow/safe defaults
* Haptics
* Use my auto-populated per-app button standard page generator - Default to use the material design icons
* Possibly no need for the full stream-controller app, just go straight from per app yaml to an optional python helper app (use to set icons/shortcuts etc).