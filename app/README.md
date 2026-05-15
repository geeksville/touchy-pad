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

This project lives under [`app/`](.) inside the touchy-pad monorepo and is
built with [Poetry](https://python-poetry.org/), orchestrated by
[just](https://just.systems/) recipes at the repo root.

```sh
# from repo root
just app-install     # poetry install
just app-test        # regenerate proto, then poetry run pytest
just app-lint        # ruff check
just app-build       # build wheel + sdist into app/dist/
just app-run -- version   # poetry run touchy version
```

The generated protobuf bindings (`touchy_pb2.py`) are **not** checked in;
every `app-*` recipe depends on `build-proto-py`, which regenerates them
from [`../proto/touchy.proto`](../proto/touchy.proto). Run
`just build-proto` to regenerate both the Python and the embedded C
bindings.
