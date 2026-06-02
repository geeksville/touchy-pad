

# Touchy-pad

![cyd](docs/images/jc4827w543.jpg)

To see (and hear) a video demo click this:
[![Watch the video](https://img.youtube.com/vi/-RXaUL3E1Vk/hqdefault.jpg)](https://youtu.be/-RXaUL3E1Vk)

If you have Python installed and a [suitable device](docs/hardware.md), you can have this running on your hardware in less than a minute:
```bash
pip install touchy-pad
touchy update # This will automatically install the firmware on your board - prompting you as needed
```
More installation instructions [here](docs/installing.md).

<br clear="right" />

## Current features
<img src="docs/images/touchy.png" alt="Project Icon" align="right" width="140" />

(Note: the first public alpha release is not yet out - this project is **young**.  But the python tool and firmware are out on pypi/github if you are very brave.  The first public 'user' alpha will be the first week of June)

* A "premium feel" open-source multitouch USB touchpad with built-in customizable screen (for use with Mac/Linux/Windows/Android).  Even if you don't want to run our sister app.
* Works with cheap ESP32 based display boards ($15-$30 USD depending on features) - no soldering required, just connect USB, run the installer and go.
* When used as a touchpad provides a pretty water droplet touch/grab/turn/gesture visualization.
* Allows simple key/mouse shortcut/macros without the need for leaving the (optional) stream-controller app running.  Generated entirely natively as USB events from our device.
* Automated installer provides 'one-click' install for boards you purchased from wherever.
* Linux, macOS and Windows hosts are supported (in theory - currently only tested with Linux, please open a [GitHub issue](https://github.com/geeksville/touchy-pad/issues) if you see problems on your machine)

## Features coming soon

* Provides a [Stream-controller](https://streamcontroller.github.io/docs/latest/) compatible API so that a graphical button array can be selected instead of touchpad.  
* Support for more devices (including little tiny 3 or 4" displays - suitable for building into custom keyboards or PC cases)

I made this little video for the streamcontroller devs showing the current proof-of-concept (click on image to see/hear the video):
[![Watch the video](https://img.youtube.com/vi/U-vNR_TbUDM/maxresdefault.jpg)](https://youtu.be/U-vNR_TbUDM)

## For developers

This project is intended to be 'open' to make it easy for host side code to manage little widgets/behaviors on these great little devices.  No embedded development experience needed.

* A toolkit/API so that other projects can easily put custom widgets/screens on these little devices (with python or some other host-side language).  Customizable screen layouts (define with JSON or a python API), bind controls to built-in keyboard/mouse macros or host side python behaviors.  A full set of widgets are available (based on [LVGL](https://lvgl.io/)).
* There is a full [python simulator](docs/simulator.md) of the device code - so you can test and develop a fair amount code without having to reflash your device.
* This project is young and more details will be here soon - hopefully...  If you have questions just open a [GitHub issue](https://github.com/geeksville/touchy-pad/issues) where we can chat.

If you'd like to see a demo of what you can do run:
```
touchy screen demo --listen
```

## Eventual features

* For fun I kinda wanna put a CYD with this software into a custom [Steam Machine front-plate](docs/images/steam.png).
* Stylus recognition (on suitable device), for brush effects etc...
* Multitouch is currently supported entirely in the device (by emulating appropriate USB HID actions), but for some art applications we should also expose a multitouch HID endpoint
* A nice 3d printed case for the popular displays.
* A lasercut or 3d printed template to allow those critical buttons to have physical 'feel' separating them from the touchpad/other buttons.  
* If haptic hardware installed haptically render taps/clicks/buttons feels. (Leaves screen mechanically isolated from case (for better haptics))
* Add optional mouse left/right/middle click buttons anywhere in the screen layout
* Add an expand/shrink touchpad hotkey (possibly by using existing screen abstraction)

## Supported hardware

There are [lots](docs/hardware.md) of $15-ish USD "Cheap Yellow Displays" that work with this project.  See below for how to run the installer (it will flash the firmware onto your device).  After installing, just connect the device to USB and you should be good to go.

## Documentation

* Installing on [devices](docs/installing.md).
* Current [design documents](docs/README.md).
* [Developer setup](docs/development.md) — new-machine setup, `just` recipes, git hooks.
* Current rough [TODO list](TODO.md).

## AI slop and development
I'm okay with using AI tools to help make code.  In fact, I used them a fair amount so far on this project (one of my first experiments with not writing all my code 'by hand').  So far it has been pretty fun.

However, in some of my other open-source projects, I've seen the current hell PR management is becoming.  So I'd **love** any code contributions y'all want to make (and I promise to be kind) but:

* Please only send in PRs **you** are willing to sign off as 'nicely written' (using your experience as a software engineer).  If your little AI buddy made something a bit ugly, please iterate with it first to make it not ugly.
* Send in PRs that are fairly 'atomic' (touch just the code they need to touch for one nicely defined feature or bug-fix)
* Only send in tested code you've run on real hardware (not just the simulator)

## Credits/Thanks

* ESP-IDF LVGL didn't have a NV3041A driver, so I (with the help of my AI buddy) cribbed a lot from [Arduno_GFX](https://github.com/moononournation/Arduino_GFX/blob/master/src/display/Arduino_NV3041A.cpp) - huge thanks to [@moononournation](https://github.com/moononournation)(?) for writing it.
* Thank you to [Shyo Holguin Campos](mailto:shyo.holguin.campos@gmail.com) who made [the](https://www.figma.com/community/file/1508554010771921982) [CCBY04](https://creativecommons.org/licenses/by/4.0/) licensed [icon](https://www.figma.com/design/xVSEw1CnaRCy6pqe41ghYD/135-Free-Cute-Colored-Icons--Community-?node-id=2002-339&t=jaNuJU0GHwZkVNAI-4) we repurposed as the [touchy-icon](docs/images/touchy.png).
* So far the main dev is [@geeksville](https://github.com/geeksville/). However if this seems interesting/fun to you please join me! 

## License
Copyright © 2026 Kevin Hester.
Licensed under the [GNU General Public License v3.0 or later](LICENSE).
