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

The supported entry point is `touchy_pad.api`:

```python
from touchy_pad.api import touchy_open

with touchy_open() as pad:
    print("firmware:", pad.client.sys_version_get().firmware_version_str)

    pad.on_host_event(0x100, lambda e: print("event", e))
    # ...do work; events arrive on a background thread...
```

See [`docs/python-api.md`](../docs/python-api.md) for the full guide
and [`docs/python-api/`](../docs/python-api/) for auto-generated
reference docs (built with `just build-docs`).

### Uploading images

`Touchy.file_save` accepts any Pillow-readable image (BMP, PNG,
JPEG, GIF, WebP) and transparently converts it to LVGL's native `.bin`
format before uploading, so the firmware (which only ships LVGL's
built-in bin decoder) can render it without extra config:

```python
with touchy_open() as pad:
    with open("avatar.png", "rb") as f:
        pad.file_save("images/avatar.png", f.read())  # auto-converted

    # Already-converted `.bin` payload, or any non-image bytes, pass
    # through unchanged.
    pad.file_save("screens/home.pb", screen.to_bytes())
```

Refer to the asset by its upload path (`asset="images/avatar.png"`)
when constructing `image(...)` / `image_button(...)` widgets — both
support `scale=` and `rotation=` for runtime transforms, and
`image_button` also takes `pressed_asset` / `pressed_scale` /
`pressed_rotation` for a distinct pressed-state look.

## Development

See [docs/development.md](../docs/development.md) for full setup instructions
(dev container or manual), day-to-day `just` recipes, and git hook details.
