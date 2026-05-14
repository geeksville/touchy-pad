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
* Screen_Reset - Discard all saved screens
* Screen_Save(screen_num, xml) - Set a screen layout
* Screen_SetCurrent(screen_num) - Set the currently displayed screen
* Screen_Wake - Turn backlight on
* Screen_SleepTimeout(msec) - Auto sleep after x ms of non-use
* Event_Consume - Pop an event from the device event queue (returns the event or none)
* Image_Reset - Discard all saved images
* Image_Save(name, bin_data) - Save an image file (so they can be used in screens etc...)

### Event endpoint messages

* Event - various subclasses of Event (ButtonEvent, TouchpadEvent, CheckboxEvent)?