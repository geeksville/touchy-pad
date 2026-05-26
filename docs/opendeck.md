# Opendeck

This is currently very raw notes with ideas about possibly implementing a plugin to OpenDeck so this device can be work there.

## Hints from https://github.com/nekename 

On how to add to https://github.com/nekename/OpenDeck

> I mainly suggest this because you'd get tested support for like at least 50 devices
https://marketplace.tacto.live/plugin/st.lynx.plugins.opendeck-akp03
https://marketplace.tacto.live/plugin/st.lynx.plugins.opendeck-akp153
https://marketplace.tacto.live/plugin/com.github.ambiso.opendeck-akp05
https://marketplace.tacto.live/plugin/com.glmagalhaes.ulanzi.d200
https://marketplace.tacto.live/plugin/com.github.ibanks42.opendeck-m18
https://marketplace.tacto.live/plugin/com.github.mean-ui-thread.opendeck-stream-dock-xl
https://marketplace.tacto.live/plugin/com.monkeykiller.plugins.opendeck-ss550
device plugins are currently 'learn by example', so take a look at the links above, and then [this](https://docs.rs/openaction/latest/openaction/device_plugin/index.html) is what there is in terms of library docs

## In particular

Use https://github.com/4ndv/opendeck-akp153 as my example driver

* A reference high level binding to receive set_image events and call handle_set_image https://github.com/4ndv/opendeck-akp153/blob/0366650d759ff798516a4013d8e41985e92312dc/src/main.rs#L44
* A reference handle_set_image impl: https://github.com/4ndv/opendeck-akp153/blob/0366650d759ff798516a4013d8e41985e92312dc/src/device.rs#L182

## Hints from TerrorWorf on the OpenDeck discord

If you use Rust, I recommend to use the OpenActionAPI Crate which does all the Websocket stuff with Opendeck and makes a few things a  lot easier. AFAIK its also the only API Crate yet, to have built in Support for the Device Plugins.