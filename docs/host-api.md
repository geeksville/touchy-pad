# Host API
This file is the design doc for the protocol used to talk between the device and the host PC.

## Legacy HID descriptors

Unrelated to the 'fancy' custom API, the device will also expose HID keyboard and mouse endpoints.  These endpoints are used to provide button press and touchpad events to the PC even if our helper app is not being used.  

## Touchy Host API

We will provide a custom API on custom USB endpoints. 

* Command Endpoint (from PC to device). A message pipe.  The host sends commands (as protocol buffers?) and reads back a response (one response per command)
* Event endpoint (from device to PC). A stream pipe.   Used to send async events from the device (user pressed button X, moved slider Y to Z etc...)

Note: the event endpoint max packet size is quite small so possibly we'll just have an event for "AsyncEventReady" and then the host will issue a "ConsumeEvent"

### Command messages

FIXME - possibly generalize how layout xml is used? let host write arbitrary filesystem paths rather than just 'screens'.  Then instead we could just use File_Write(path, payload) to do images, screens, any other required metadata.  Possibly providing a nice way to let the host swap out just small parts of the GUI 'on-the-fly'?  Read lvgl docs a bit more...

FIXME, store images as files (possibly with no filetype conversion on this host).  This [should](https://lvgl.io/docs/open/main-modules/images/decoders) allow LVGL caching to auto discard LRU images and reread from 'disk' as needed. 

* XML_Reset - Discard all saved xml
* XML_Save(filepath, xml) - Set a screen layout or other lvgl config file.  If that screen is already displayed screen, the screen will be refreshed based on this xml.
* Screen_Load(screen_name) - Set the currently displayed screen
* Screen_Wake - Turn backlight on
* Screen_SleepTimeout(msec) - Auto sleep after x ms of non-use
* Event_Consume - Pop an event from the device event queue (returns the event or none)
* Image_Reset - Discard all saved images
* Image_Save(name, bin_data) - Save an image file (so they can be used in screens etc...).  All images saved in this fashion will be registed with lv_xml_register_image() and then available in expressions such as ```<lv_image src="avatar" align="center"/>```
* Sys_Reboot_Bootloader - Reboot into bootloader
* Sys_Set_Lock(secret) - Assign a secret needed for any future communication.  Once set the device will need to be unlocked before it responds to any command (or a factory reset to clear the device to a virgin state)
* Sys_Unlock(secret) - Unlock device (device will respond to commands until the host PC disconnects)
* Sys_Version_Get - Get the firmware version info

### Event endpoint messages

* Event_LV - A wrapped version of an LVGL event.
FIXME, possibly instead just forward a lightly wrapped version of lv_event_type.  The host PC can populate event_cb nodes with a custom user_data so that they can then map the received lv_event_t/trigger/user_data and handle it as they wish (for any needed host side handling).  They would specify callback as "host_handled_event_cb".

```xml
<view>
    <lv_button width="200" height="100">
        <event_cb callback="host_handled_cb" trigger="clicked" user_data="someuniqueid-assigned-by-host"/>
        <lv_label text="Hello"/>
    </lv_button>
</view>
```

* If the host wants device side HID-event macros, that could possibly be done by just doing something like:
```xml
<view>
    <lv_button width="200" height="100">
        <event_cb callback="event_macro_cb" trigger="clicked" user_data="somebase64-string-of-event-codes"/>
        <lv_label text="Hello"/>
    </lv_button>
</view>
```

See https://lvgl.io/docs/open/xml/ui_elements/events and https://lvgl.io/docs/open/api/core/lv_event_h for more info