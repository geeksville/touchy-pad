# Creep3r

Listens to people talking and plays along with the idea that people are creeped out about their phones listening to them.  Use a 100% local AI (so never sending anything to the internet) to listen for people talking.  Once it hears a phrase rate it for 'interestingness'.  If considered interesting ask a different local AI 'write something witty about PHRASE'.  Scroll that witty phrase down the sides of a hanging LED block - about 3 ft long and 6" square on a side?

Notes about LED-ART variant

* Scrolling vertically - hanging art project
* Use multiple "WS2812B RGB Panel LED RGB IC addressable LED strip Flexible Digital 8x8 8x32 16x16 4x12 5050SMD independently addressable PCB 5V" panels.  Probably the the 8x32 variant.  Stripe down the sides of a 3d printed frame.  
ESP32-S3-ROOM inside.
* touchy-pad module will talk to host linux CPU that is doing (locally hosted) ai voice rec to find funny things to start scrolling.

https://github.com/AaronLiddiment/LEDText/wiki