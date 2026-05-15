# TODO

(Items preceeded by ! are completed alredy)

## For release Alpha 1

Main new features: works as a touchpad, with some basic ability to add buttons (streamdeck like) via the companion app

* !Decide between QMK/ZMK/platformio base... (anyone already support the full touchpad USB spec?) - for now I'm using platformio
* !make a mini PRD/design doc
* !add datasheets
* remove platformio
* !Ping Chris Kirmse
* Fix screen drawing on new device
* !Specify key classes
* !Make platformio shell
* Make python helper app (share code with my streamdeck project)
* !make a CLAUDE.md file
* Add credits for BSD licenced https://github.com/moononournation/Arduino_GFX/blob/master/src/display/Arduino_NV3041A.cpp
* ensure no 10ms delay on loop polling
* Pick a real USB VID/PID for our device
* determine security issues (might need to set a secure key for future API operations - to prevent untrusted users from changing macro behavior)

## For release Alpha 2

Main new features: Works with StreamController app to provide arbitrary user buttons

* Built-in [StreamController](https://streamcontroller.github.io/docs/latest/) support.  I'll need to improve their plugin api a bit and send them a PR first

## For release Alpha 3

Main new features? Much easier scripting than through streamcontroller - allow arbitrary python snippets for button presses/slider moves etc...  Dynamic data displays from host to Touchypad (server stats, ZMK modes, whatever user wants to show)

* tactile precut sticker for screen overlay?
* figure out best way to mount haptics for best effect
* Increase CPU, FLASH and RAM speeds to the max.  currently the firmware picks slow/safe defaults