Hi,

I've been working with other kind hobbyists and we've made a project we call 'touchypad'.  It is ESP32 based and has a fair number of happy users (we've been through a few months of releases it is seems to mostly work now).  We hope to find more friendly geeks to use/work-on this with us.  

The README has IMO decent docs to get started - and we have prebuilt binaries/pypi installers all ready...  

The goals:

  * **Easy** to install, just buy a cheap $20ish board from China, run "touchy update" to have it automatically program the firmware and config files.  And you'll immediately have a useful graphical USB macropad/pretty customizable multitouch touchpad.  No soldering or toolchain hell. (See the github README for more details)
  * Allows 'pc developers' to easily write python or rust code to have pretty custom displays controlled by USB or wifi.
  * Any device that can work with lvgl can be easily supported - initially we are targeting cheap 'CYD' LCD devices and large LED panels

Approximate development history:
  * A few months ago we chatted on r/ErgoMechKeyboards about making a nice graphical touchpad (nicer than the Apple thing, but open source with a built in display)
  * Made a protobuf based API (and ESP-IDF based firmware) to allow host PCs to do UIs on any ESP-32 that can run lvgl
  * Make a python and rust client API (with some example code/rough-early docs)
  * Use that API to make the graphical multitouch/gesture trackpad work on these cool $20ish clone boards
  * Use that API to make a '$20 Stream Deck (like)' - by using that API to build a plug-in for the open-source OpenDeck project.
  * Various r/ErgoMechKeyboards geeks kindly added support for new boards (it is fairly painless to support any ESP32, though the ESP32S3 with PSRAM are recommended)
  * Added support for large Neopixel panels in addition to the existing LCD screen drivers.  This is to allow use to make a semi-large art project (this is ongoing and related to a couple of other open source projects)

I'm happy to answer any questions and help if you find bugs ;-).  AMA in this thread?

* README here: https://github.com/geeksville/touchy-pad 
* Latest release notes: https://github.com/geeksville/touchy-pad/releases/tag/v0.3.4 
* We do release notes (and perhaps someday discussion) in r/touchypad.

Hopefully it might be useful for some? (I last posted in this sub like 10 yrs ago when I announced Meshtastic. omg I'm old.  Then I tried to post here last week but sorta faffed it up because old.reddit.com can't post correctly to this sub.)

TODO
Announce in
https://www.reddit.com/r/led/?screen_view_count=1
https://www.reddit.com/r/esp32projects/submit/?type=TEXT
https://www.reddit.com/r/ArduinoProjects/submit/?type=TEXT