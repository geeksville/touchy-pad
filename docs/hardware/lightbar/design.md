this file is design plans for lightbar firmware

## stage LB1
add a new board to firmware/boards called jc-esp32p4-m3 it is a genric board based on a esp32p4nrw32.  it has 32M of PSRAM and 16M of flash.  note: this module includes two cpus a esp32p4 (for main processing) and a esp32c6 (for wifi).  i think espidf should already know how to handle this.  it includes real usb support.


it has no LCD display at all, instead:

* it supports a variable number of "8x32 WS2812B LED matrix" display panels.  abstract this out by making a LEDPanel (a subclass of a new Panel class) class.  one instace per panel.  each instace drives a particular GPIO.  for now just create one panel driving gpio 1.  pay attention to the serpentine note in lightbar.md to understand the odd pixel ordering
* use the official espressif/led_strip library https://espressif.github.io/idf-extra-components/latest/led_strip/index.html RMT with DMA per https://components.espressif.com/components/espressif/led_strip/versions/3.0.3/readme to drive eacn LEDPanel.
* Panels are used to create the actual output for lvgl displays.  for now just make a driver that renders one Panel to our current one display instance.  but eventually we will have multiple Panels that will either be tiled to make a single lvgl display or we will make multiple lvgl display objects

other changes:
* extend "struct Platform" with is_touchable.  prior boards all had touchscreens - this device has no touchscreen
* for !touchable board support firmware/main/default_screen_pb.h will need to contain both touchable and touchless variants.  /workspaces/touchy-pad/proto/gen_default_screen.py will need to be smarter.  for the touchless variant layout left to right a 4x4 red square, a 6x6 green square and a 8x8 blue square (to fit nicely in our 8x32 panel)

