
* !generalize so that panel config comes from preferences
* fix background
* update readme
* add hw build instructions
* !add to ci
* post to reddit

https://docs.espressif.com/projects/esp-protocols/mdns/docs/latest/en/index.html

https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html

https://easyeda.com/ https://jlcpcb.com/ 
contact adafruit? lily?

buck converter - https://www.amazon.com/Converter-Voltage-Regulator-Microcontroller-Conversion/dp/B0GYJDP96Q/ref=sr_1_4?crid=3LI41I4FUUJHA&dib=eyJ2IjoiMSJ9.RiskZpgoNTqIyjVc-OXdTiZA9cddm3GH-TENmPgA7TalGsZFO3DccO3GHZEfatYGq881GTR7Z-jrJkIcohl2JHnLFU8vcEdh4oO2g7qMbUiyFMLL_PkmvIUofmOTvS91MXbwDDyfDngaKPc4bp0RpZ1MhEBMI2etXOeLrll6i6hOUq8GwXvMrAeexUhW35M3TL6cpBoE-RG4RqdVIq8XJNthPPLncXNiQ9_O9o56iSg.q-Aj61A7IeyrRtzX33yxXPsV3pRxkVh2g7O7PSTfI2A&dib_tag=se&keywords=12v%2Bto%2B5v%2Bbuck%2Bconverter&qid=1783886438&sprefix=12v%2Bto%2B%2Caps%2C181&sr=8-4&th=1
spare pads
8? led headers - sized based on level shifter - jumpers to select default or custom gpio?
fuse
barell jack
small caps on lv bus
extension power and fan connectors

Capacitor: Add a large capacitor (e.g., 1000 µF, 16V or higher) across the 12V power and ground lines, as outlined in the Adafruit NeoPixel Überguide. This absorbs initial inrush currents and protects the pixels.
Resistor: Place a 300 to 500 Ω resistor in series on the data line between the level shifter and the NeoPixel's DIN pin to prevent signal reflections.

* property or name lookup api for changing field contents on the fly - possibly html dom like?
* wifi from preferences
* https based auth?
* protocol api on multiple interfaces (only the one that shows client activity?)

The background color is a simple color to fill the display. It can be adjusted with lv_disp_set_bg_color(disp, color).
The opacity of the background color or image can be adjusted with lv_disp_set_bg_opa(disp, opa).
The disp parameter of these functions can be NULL to select the default display.