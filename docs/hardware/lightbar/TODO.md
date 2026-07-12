
* fix background
* update readme
* add hw build instructions
* !add to ci
* post to reddit

https://easyeda.com/ https://jlcpcb.com/ 
contact adafruit?

buck converter
spare pads
8? led headers - sized based on level shifter - jumpers to select default or custom gpio?
fuse
barell jack
small caps on lv bus

Capacitor: Add a large capacitor (e.g., 1000 µF, 16V or higher) across the 12V power and ground lines, as outlined in the Adafruit NeoPixel Überguide. This absorbs initial inrush currents and protects the pixels.
Resistor: Place a 300 to 500 Ω resistor in series on the data line between the level shifter and the NeoPixel's DIN pin to prevent signal reflections.

* property or name lookup api for changing field contents on the fly - possibly html dom like?
* generalize so that panel config comes from preferences
* wifi from preferences
* https based auth?
* protocol api on multiple interfaces (only the one that shows client activity?)

The background color is a simple color to fill the display. It can be adjusted with lv_disp_set_bg_color(disp, color).
The opacity of the background color or image can be adjusted with lv_disp_set_bg_opa(disp, opa).
The disp parameter of these functions can be NULL to select the default display.