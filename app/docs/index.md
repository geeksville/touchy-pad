# touchy_pad.api

The supported Python API for the [Touchy-Pad](https://github.com/geeksville/touchy-pad)
USB multitouch device.

## Quick start

```python
from touchy_pad.api import touchy_open, Screen, button, row

with touchy_open() as pad:
    s = Screen("home", layout=row(gap=8))
    s += button("hi", text="Hi")
    pad.screen_save(s)
    pad.screen_load("home")
```

## Modules

- [`touchy_pad.api`](device.md) — lifecycle (`touchy_open`, `Touchy`,
  `touchy_get_pad_ids`).
- [`touchy_pad.api.screens`](screens.md) — host-side DSL for authoring
  screens and widgets.
- [`touchy_pad.api.macros`](macros.md) — keyboard / mouse macro DSL.
- [`touchy_pad.api.protobuf`](protobuf.md) — raw generated protobuf
  message classes.
- [`touchy_pad.api.images`](images.md) — image helpers used by demos.

See the
[Python API guide](https://github.com/geeksville/touchy-pad/blob/main/docs/python-api.md)
for a longer narrative introduction.
