# Creep3r and related projects
Notes about LED-ART variants

Probably best done as seperate projects:
* lightbar: based on touchypad-code, 12V highwatt in, drives panels and talks protobuf over wifi
* voicebot: use a local (coral based?) voice->text, text->voice AI.  possibly add homekit integration support so it can be an entirely local smart homekit controller.  Via MCP?
* creep3r: uses lightbar and voicebot to do its art.  probably runs on the same hw with voicebot

## creep3r

Listens to people talking and plays along with the idea that people are creeped out about their phones listening to them.  Use a 100% local AI (so never sending anything to the internet) to listen for people talking.  Once it hears a phrase rate it for 'interestingness'.  If considered interesting ask a different local AI 'write something witty about PHRASE'.  Scroll that witty phrase down the sides of a hanging LED block - about 3 ft long and 6" square on a side?

* Scrolling vertically - hanging art project
* Use multiple "WS2812B RGB Panel LED RGB IC addressable LED strip Flexible Digital 8x8 8x32 16x16 4x12 5050SMD independently addressable PCB 5V" panels.  Probably the the 8x32 variant.  Stripe down the sides of a 3d printed frame.  
ESP32-S3-ROOM inside.
* touchy-pad module will talk to host linux CPU that is doing (locally hosted) ai voice rec to find funny things to start scrolling.

### Voice input
Great thread https://www.reddit.com/r/homeassistant/s/wl02ow67MK
https://www.reddit.com/r/RockchipNPU/comments/1g3cetq/fast_and_accurate_speechtotext_on_rk3588_with/?share_id=5YA3sA8R8K1hXxF8YJaFh 

What about if I use this device over USB instead of esp32s3?  Can I hook it to my rpi and still take advantage of the built in vad?
Possibly this version reSpeaker USB 4-Mic Array XVF3000 v3.0 | Seeed Studio Wiki https://share.google/SmlZbrjco82vemvfg
Best mic module https://wiki.seeedstudio.com/respeaker_xvf3800_introduction/ has esp32s3 
Orangepi info https://www.reddit.com/r/SBCs/s/LBz8vA3nx5

## lightbar

* Update: ESP32-P4 is an even better processor choice
* Max of 4 bit lanes per box module, max of 4 modules (16 display panels - each panel 8x32 pixel).  Initially try just one lane per box module (4 displays daisy chained) and see how bad it is...
* 12V 200W in, internally use little 12V->buck converters to power the esp32 board and 5V device control
* include a small fan to help convection

https://github.com/Xylopyrographer/LiteLED - high perf esp driver
https://cdn-shop.adafruit.com/datasheets/WS2812B.pdf 
https://www.superlightingled.com/PDF/Addressable-Flex-LED-Pixel-Panel.pdf
https://github.com/AaronLiddiment/LEDText/wiki
https://docs.soldered.com/ws2812b/how-it-works/ 
https://github.com/fastled/fastled#-documentation--support

### Similar projects?

https://kno.wled.ge/features/effects/

### Firmware OTA
https://github.com/gibz104/SafeGithubOTA

### BOM

* https://www.aliexpress.us/item/3256802721579904.html 12V, 250W, $66

### Box module
design as an extendable platform.

4 of these panels, total cost $60 https://www.aliexpress.us/item/3256809608935627.html

**Size of the Box**
To fit four 80x320mm panels edge-to-edge, your box will be a rectangular pillar measuring roughly **80mm x 80mm x 320mm**.

**Current Consumption**
You just escalated from a cute desk toy to a blinding space heater. 205W

* One 8x32 panel contains 256 LEDs.
* Four panels equal 1,024 LEDs total.

You **absolutely need 12V** (like the WS2815) for a project this dense.

Pushing 60 Amps at 5V means wrestling with massive wires. Furthermore, 5V suffers from severe voltage drop, which will turn your crisp white animations into a muddy orange unless you inject power constantly.

Switching to a 12V WS2815 matrix solves this. The higher voltage allows each pixel to draw only about 15mA at full white. Your 1,024 LED array now pulls a much more manageable **15.3 Amps**. It is a lot easier to wire, and significantly less likely to melt your desk.

### esp32 selection

https://github.com/espressif/esp-idf/issues/18071 says PARLIO based approaches (see below) are probably fine on any supporting arch.  Therefore probably a ESP32-P4 + ESP32-C6 (to get wifi) module is best.  This one: https://www.aliexpress.us/item/3256810578584433.html

### frame rate
For a single, continuous chain of 1,024 WS2815 (or WS2812B) LEDs, your maximum theoretical framerate is **~32.2 FPS**.

Here is the exact math behind that hardware limit:

1. **Bit Time:** The WS2815 protocol runs at 800 kHz. This means transmitting a single bit of data takes **1.25 microseconds (µs)**.
2. **LED Time:** Each LED requires 24 bits of color data (8 bits each for Red, Green, and Blue). So, one LED takes `24 * 1.25 µs =` **30 µs**.
3. **Transmission Time:** For 1,024 LEDs, the MCU must push data continuously for `1024 * 30 µs =` **30,720 µs (or 30.72 milliseconds)**.
4. **The Reset Latch:** Once the data is pushed, the data line must be held low for the LEDs to "latch" the data and display it. For modern WS2815/WS2812B chips, this reset pulse requires a minimum of **280 µs (0.28 ms)**.

**Total Frame Time:** `30.72 ms + 0.28 ms =` **31.00 ms per frame.**
**Max FPS:** `1000 ms / 31.00 ms =` **32.25 FPS.**

This is exactly why the ESP32-P4 and PARLIO hardware we discussed earlier are so powerful. By splitting those 1,024 LEDs across four independent pins (256 LEDs per pin) and transmitting them in parallel, your transmission time drops to 7.68 ms, pushing your maximum framerate up to an absolutely blistering **125 FPS**.

### esp32 drivers

ai sez

Yes, absolutely. The ESP32 ecosystem has heavily embraced hardware-driven DMA (Direct Memory Access) solutions for WS2812B LEDs to solve the notorious "flickering" issues caused by FreeRTOS task switching, Wi-Fi, and Bluetooth interrupts.

Because the WS2812B requires incredibly strict microsecond timing, "bit-banging" (using the CPU to toggle pins) falls apart as soon as the ESP32 tries to process a network packet. To get around this, developers have written drivers that hijack various hardware peripherals to stream the data via DMA without CPU intervention.

Here are the main hardware DMA methods and the libraries that support them:

### 1. I2S / LCD Parallel Mode (The Powerhouse)

One of the most brilliant and widely used methods in the community is repurposing the ESP32's I2S (audio) peripheral. At its core, I2S flips a data line really fast alongside a clock. Developers figured out how to feed WS2812 data into the I2S hardware instead of sound.

* **Why it's great:** It uses DMA, meaning zero CPU overhead during transmission, and the CPU is free to calculate the next frame while the current one is drawing. It can also drive up to 24 separate LED strips simultaneously in parallel on a standard ESP32, and utilizes the LCD Parallel driver on the ESP32-S3.
* **Where to find it:** Supported by **FastLED** (by enabling the `FASTLED_ESP32_I2S` build flag) and **NeoPixelBus**.

### 2. RMT with DMA (The Modern Standard)

The RMT (Remote Control Transceiver) peripheral was designed by Espressif specifically to generate arbitrary waveforms like infrared signals. For a long time, RMT was interrupt-driven, which still suffered from Wi-Fi stuttering. However, modern ESP-IDF versions (v5+) and modern ESP32 chips now allow the RMT hardware to be assisted by DMA.

* **Why it's great:** It allows asynchronous, DMA-backed transmissions, and the hardware is specifically tailored for exact pulse widths.
* **Where to find it:** The official **ESP-IDF `led_strip**` component (by setting `with_dma` to true in the config) and the latest versions of **FastLED**.

### 3. SPI with DMA (The Reliable Fallback)

The SPI peripheral can be tricked into generating the WS2812 timing by clocking the bus extremely fast (e.g., 2.4MHz) and packing multiple SPI bits to represent a single WS2812 high/low pulse.

* **Why it's great:** SPI naturally uses DMA, making it rock-solid against interrupts. It is highly recommended if your chip doesn't have RMT DMA support.
* **Where to find it:** Supported natively in the **ESP-IDF `led_strip**` component (using the SPI backend) and in FastLED via the `FASTLED_ESP32_USE_CLOCKLESS_SPI` flag.

### 4. PARLIO with DMA (For the Newest Chips)

On the newest generations of ESP32 chips (like the ESP32-C6, ESP32-H2, and ESP32-P4), a new peripheral called PARLIO (Parallel IO) is used.

* **Why it's great:** It allows you to register up to 16 strips on independent bit-lanes, where all displays are updated in a single DMA transfer in perfect lock-step synchronization.
* **Where to find it:** The **LiteLED** library provides a specific `LiteLEDpioGroup` class to utilize this hardware on Arduino-ESP32 setups.

### Which Library Should You Use?

* **For WLED/Smart Home:** Use **NeoPixelBus**. It is the underlying engine for WLED and handles I2S DMA automatically to keep the web-server perfectly responsive.
* **For Custom Arduino Code:** Use **FastLED**. It is the gold standard for animations. It defaults to an asynchronous RMT DMA driver on modern frameworks, but you can force I2S for parallel output.
* **For Bare-Metal ESP-IDF:** Use Espressif's official **`led_strip`** component. It abstracts the hardware details so you just pass in your backend of choice (RMT or SPI) and it handles the memory allocation and DMA.