# Creep3r and related projects
Notes about LED-ART variants

Probably best done as seperate projects:
* [lightbar](hardware/lightbar/lightbar.md): a hanging 4 sided LED column - based on touchypad-code, 12V highwatt in, drives multiple panels and talks protobuf over wifi
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
