# Host API
This file is the design doc for the protocol used to talk between the device and the host PC.

## Legacy HID descriptors

Unrelated to the 'fancy' custom API, the device will also expose HID keyboard and mouse interfaces/endpoints.  These are used to provide button press and touchpad events to the PC even if our helper app is not being used.  

## Touchy Host API

We will provide a custom API on custom USB interface composed of three endpoints. 

* Command Endpoint (OUT from PC to device). A message pipe.   The host sends commands (as protocol buffers?).
* Response Endpoint (IN to the PC) reads back a response (one response per command)
* Event endpoint (IN to PC). An interrupt stream pipe.   Used to send async events from the device (user pressed button X, moved slider Y to Z etc...)

Note: the event endpoint max packet size is quite small so possibly we'll just have an event for "AsyncEventReady" and then the host will issue a "ConsumeEvent"

### Command messages

FIXME - possibly generalize how layout xml is used? let host write arbitrary filesystem paths rather than just 'screens'.  Then instead we could just use File_Write(path, payload) to do images, screens, any other required metadata.  Possibly providing a nice way to let the host swap out just small parts of the GUI 'on-the-fly'?  Read lvgl docs a bit more...

FIXME, store images as files (possibly with no filetype conversion on this host).  This [should](https://lvgl.io/docs/open/main-modules/images/decoders) allow LVGL caching to auto discard LRU images and reread from 'disk' as needed. 

* XML_Reset - Discard all saved xml
* XML_Save(filepath, xml_string) - Set a screen layout or other lvgl config file.  If that screen is already displayed screen, the screen will be refreshed based on this xml.
* Screen_Load(screen_name) - Set the currently displayed screen
* Screen_Wake - Turn backlight on
* Screen_Sleep_Timeout(msec) - Auto sleep after x ms of non-use
* Event_Consume - Pop an event from the device event queue (returns the event message or empty)
* Image_Reset - Discard all saved images
* Image_Save(filepath, binary_data) - Save an image file (so they can be used in screens etc...).  All images saved in this fashion will be registed with lv_xml_register_image() and then available in expressions such as ```<lv_image src="avatar" align="center"/>```
* Sys_Reboot_Bootloader - Reboot into bootloader (for firmware update)
* Sys_Version_Get - Get the protocol and firmware version info 

### Command responses

* Response(code) - 0 = okay, anything is some TBD error
* Sys_Version_Response(protocol_vernum, firmware_ver_num, firmware_ver_str)

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

See https://lvgl.io/docs/open/xml/ui_elements/events, https://lvgl.io/docs/open/common-widget-features/events#fields-of-lv_event_t and https://lvgl.io/docs/open/api/core/lv_event_h for more info