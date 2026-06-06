# TODO

## Alpha 3
Main new features? Much easier scripting than through StreamController - allow arbitrary Python snippets for button presses/slider moves etc...  Dynamic data displays from host to Touchypad (server stats, ZMK modes, whatever user wants to show)

* [x] move background drawing out of trackpad widget.  that was a mistake. 
* [x] allow arbitrary image backgrounds for standard trackpad layout.  for now use the touchy icon.
* [x] investigate hang on setting large gifs
* [x] board-info improvements, include free RAM, PSRAM and flash-file-system numbers
* [ ] add auto discovery of uart based touchys
* [ ] properly warn user if selected gif/file is too large to use (based on ram size)
* [ ] make sure opendeck plugin is solid
* [ ] add a reddit link for support/discussion
* [ ] support no-touch devices
* [ ] add photo of the small board
* [ ] add some of the example JSON to the API docs
* [x] shrink images slightly in opendeck plugin
* [ ] implement the StreamDeck background graphic API
* [ ] determine USB security issues (might need to set a secure key for future API operations - to prevent untrusted users from changing macro behavior)
* [ ] Support a few more board types
* [ ] Give user friendly error msg if firmware or py code is too old
* [x] Allow setting a persistent device pref for 'min-log-level' logs with lower pri than this will not be queued for the host, just drop em.  default threshold is ERROR. Update python cli to add a "set-log-level FOO" cmd.  Also add a "boot-delay" param, to cause a sleep early on - to allow time for debug logging connection establishment. (Stage 82: `touchy pref log-level` / `pref boot-delay`.)
* [ ] support eink displays in my test devices drawer
* [ ] investigate this appstore (or others) https://www.xda-developers.com/someone-created-an-esp32-app-store-and-it-lets-you-flash-apps-straight-from-your-browser/
* [ ] try turning off GPIO matrix for SPI display writes - per https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/spi_master.html#_CPPv416spi_bus_config_t might allow increase to 80Mhz.  "You can use this structure to specify the GPIO pins of the bus. Normally, the driver will use the GPIO matrix to route the signals. An exception is made when all signals either can be routed through the IO_MUX or are -1. In that case, the IO_MUX is used. On ESP32, using GPIO matrix will bring about 25ns of input delay, which may cause incorrect read for >40MHz speeds."
* [x] when using ch341 uart try to use 460800 for better speeds
* [ ] make knobs/dials with gesture overlays (share code with trackpad) ccw/cw/left/up etc...
* [ ] use gestures for left/right screen switching instead of buttons at top.
* [ ] support turn, back, forward, up gestures natively.  be careful to not confuse with drags.  add various slope/min-dist/max-time thresholds.
* [ ] allow setting brightness by putting that GPIO on a hw PWM output
* [x] add an animation to demo/test.pb https://lvgl.io/docs/open/9.5/main-modules/animation.html 
* [ ] Implement multitouch HID to support multitouch native apps
* [ ] explain plans w.r.t. 3d printing and knobs on the screen
* [x] Increase CPU, FLASH and RAM speeds to the max.  Currently the firmware picks slow/safe defaults

# Alpha 4

* [ ] make a registry of uploaded uscr files.  
* [ ] Extend those uscrs with triggers of "show when app X is in foreground".  Adopt that as the standard 'better than streamdeck' replacement.  so people can make screens with guis that do interesting things for any foreground app.
* [ ] make a 'builder' to help users with making new uscrs - walk them through running apps, icon selection etc...
* [ ] Stylus support for 'paintbrush mode'
* [ ] Built-in [StreamController](https://streamcontroller.github.io/docs/latest/) support.  Probably via the mock device proof of concept.
* [ ] expose device API via wifi/tcp (for arbitary smart signage applications).   Find a good esp32 lib for wifi settings/management/firmware update?
* [ ] Haptics (figure out best way to mount haptics for best effect)
* [ ] tactile precut sticker/3d printable case for screen overlay?
* [ ] Use my auto-populated per-app button standard page generator - Default to use the material design icons
* [ ] Possibly no need for the full StreamController app, just go straight from per app yaml to an optional Python helper app (use to set icons/shortcuts etc).

# Already completed

## Alpha 1
Early test release for OpenDeck/StreamController devs.

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
  * [x] request feedback/propose distribution
* [x] ensure no 10ms delay on loop polling
* [x] Pick a real USB VID/PID for our device via https://github.com/espressif/usb-pids/pull/315
* [x] fix multitouch gestures - the change to lvgl instead of polling broke them

## Alpha 2
First public release.

Main new features: Works with StreamController app to provide arbitrary user buttons

* [x] add aliexress/etc... links to recommended board
* [x] change DSL to allow callback functions attached to actions.  completely hide "event id" from the api consumer (just map it to the callback)
* [x] make switchable subscreens by using widgetref?
* [x] remove Screen.path?  I don't think we need it
* [x] test https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/lcd/index.html to see if it could help rendering performance - NO
* [x] investigate [opendeck](docs/opendeck.md) and possibly do a plugin for that instead-of or in-addition-to StreamController.
* [x] fix opendeck plugin not enumerating devices - figure out how to debug rust https://code.visualstudio.com/docs/languages/rust#_install-debugging-support
* [x] add opendeck plugin binary to release
* [x] make stand alone example.py
* [x] explain opendeck plugin install
* [x] make opendeck example video
* [x] send message to Opendeck geeks (include reverse engineer docs and video)
* [x] explain StreamController status
* [x] add a widget handle concept so host can say to just redraw one particular widget (only need to implement for screens/layouts/image)
* [x] make screen sleep default timeout
* [x] Add support for F:widgets/foo.pb widget files.  This would allow TouchyDeck to own a R:widgets/deck.pb.  Which could be nested into a F: screen.  So that dynamic StreamDeck emulated stuff could be updated independently of the configurable screens the user has selected.  Probably should add a way for the host to see dimensions of arbitrary widgets in screens/walk the screen list?
* [x] Turn off "Expensive debugging flags!" in sdkconfig.defaults
* [x] cleanup embedding of streamdeck ui into other widgets (via user screens)
* [x] update python code to be 'finished' for StreamController (make sure press and release events work)
* [x] reddit post
* [x] include prebuilt win/os-x exes in the opendeck release zip. "Cross-compiling for every target is the usual sticking point. CI matrices and cargo zigbuild / cross are the standard answers." per https://github.com/geeksville/touchy-pad/blob/main/docs/opendeck-device-plugin.md#3-manifest-essentials

# Rejected ideas

* [ ] NOT POSSIBLE - reddit api busted - publish release notes to reddit https://github.com/meysam81/reddit-scheduled-submit 