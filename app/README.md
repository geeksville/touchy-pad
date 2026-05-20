# touchy-pad

Host-side Python library and CLI for the [Touchy-Pad](../README.md) USB
multitouch device.

## Install

```sh
pip install touchy-pad
```

You will also need a working `libusb-1.0` runtime on your host:

| Platform | Command                                          |
|----------|--------------------------------------------------|
| Debian   | `sudo apt install libusb-1.0-0`                  |
| macOS    | `brew install libusb`                            |
| Windows  | bundle `libusb-1.0.dll` next to your interpreter |

On Linux you also need a udev rule so non-root users can open the device.
A starter rule is shipped at [`../bin/99-touchy-pad.rules`](../bin/99-touchy-pad.rules).

## CLI

```sh
touchy --help              # list subcommands
touchy version             # query device firmware/protocol version
touchy screen-wake         # turn the backlight on
touchy events              # live-tail events from the device
```

## Library

```python
from touchy_pad import TouchyClient

with TouchyClient.open() as client:
    info = client.sys_version_get()
    print(info.firmware_version_str)

    for event in client.stream_events():
        print(event)
```

## Development

See [docs/development.md](../docs/development.md) for full setup instructions
(dev container or manual), day-to-day `just` recipes, and git hook details.
