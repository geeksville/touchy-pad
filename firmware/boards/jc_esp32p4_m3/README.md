warnings! this doesn't work yet.  it fails during psram init (even before app)

```
the hello_world version worked but the other version gets stuck in a loop- the only relevant differences are those two sdkconfig files

heres the device log where the looping begins

D MSPI Timing: psram_freq_mhz: 200 mhz, bus clock div: 2
D MSPI DQS: set to best phase: 0
D MSPI DQS: set to best phase: 0
(repeats until the watchdog reboots the board)
```