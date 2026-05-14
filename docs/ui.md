# UI design guide

The basic GUI is layed out via [LVGL xml](https://viewer.lvgl.io/?repo=geeksville%2Ftouchy-pad%2Ftree%2Fmain%2Fui).  

## General thoughts

NOTE: much of this section is now out-of-date because I suspect we'll just be able to use LVGL XML to define all layouts (with the full generality that provides).  We'll make a custom HIDButton widget that can also do 'macro playback' of HID events in addition to its regular button behavior.  And a custom TouchpadWidget.  Otherwise anything allowed by LVGL XML is fine.

* Allow host to send protocol buffer commands to add screens and widgets.  We will store that data in some TBD format (protobuf based?  lvgl xml?) flash filesystem (so the config is persistent without even an app provisioning)
* any interaction with widgets by the user will generate protobuf events on our private USB characteristic
* Some interactions (touchpad widget or button presses) might generate standard USB HID (mouse/keyboard) events (so many behaviors can be used by the user without even using our helper app)
* FIXME: possibly use lvgl xml for this?  Or just handcode in C? See https://lvgl.io/docs/open/xml/integration/xml for ideas.
* The device code is entirely agnostic wrt the layour of the xml provided by the host, which keeps the device code small.  This also lets us put most of the smarts into the sister python api/app.
* FIXME: Can TouchpadWidget instead be a Component rather than a Widget?  It sounds like the glue code is easier in that case.

## General layout
* A small number of buttons are standard and always visible on the screen (upper left by default?).  They include things like "Next screen", "Prev Screen".
* All other screen components are screen specific and provided based on the (xml?) sent by the host.  Most commonly it will include a large rectangular TouchpadWidget and possibly some number of app specific Widgets.  

## Image representation
Images sent from host are always in LVGL BIN format.  Converted on the host.  Some sort of compression will be supported, possibly https://lvgl.io/docs/open/libs/image_support/lz4?

## Widgets
* Button - Client can set a normal and pressed image.  If no pressed image is provided, device will default to just invert the button while pressed.  Might also send a series of HID events (to allow macro playback)
* Checkbox - Similar to button but also has a 'checked' state that can be set by the client
* Touchpad - A rectangular region, presses inside this region generate mouse/touch events on the host.  Can contain a background image and and optional touch image (to mark finger positions while touching)
* Slider

## Touchpad Widget

### Gestures

FIXME Perhaps the gesture stuff I've been hand coding could be do instead with https://lvgl.io/docs/open/examples/others/gestures?

## Special events
Some event codes are special and handled locally entirely within the device.

* SCREEN_NEXT, SCREEN_PREV
* SLEEP_NOW (turns backlight off but leaves touchscreen still working - if a touch occurs turn backlight back on)