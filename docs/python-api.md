# Python API guide

The official Python entry point for application code is
`touchy_pad.api`. Everything outside this package (e.g.
`touchy_pad._proto`, the simulator, the CLI) is considered internal and
may change without notice.

Auto-generated per-class / per-function reference docs live under
[python-api/](python-api/) (built by `just build-docs`).

## Install

```sh
pip install touchy-pad
```

You also need a working `libusb-1.0` runtime; see the
[main README](../README.md) and [`bin/99-touchy-pad.rules`](../bin/99-touchy-pad.rules)
for the Linux udev rule.

## Hello pad

```python
from touchy_pad.api import touchy_open, touchy_get_pad_ids

print("attached:", touchy_get_pad_ids())

with touchy_open() as pad:
    # Screens are addressed by drive-prefixed path, not by bare name
    # (``pad.screen_save`` writes ``F:host/s/<name>.pb``). "" loads the
    # device's default screen; ``Touchy.screen_load`` returns ``None``.
    pad.screen_load("F:host/s/home.pb")
```

`touchy_open()` opens the first connected device by default; pass a
serial number from `touchy_get_pad_ids()` to select a specific one. The
returned `Touchy` is a context manager; leaving the `with` block stops
the background event-poller thread and closes the USB transport.

## Checking the background reader

`touchy_open()` starts a background thread that polls the device for events
and runs your `on_host_event` callbacks. If the USB link drops, that thread
stops. `Touchy.event_thread_alive` reports whether it is still running:

```python
import time

from touchy_pad.api import DeviceNotFoundError, TouchyError, touchy_open

RETRY_S = 2.0

while True:                          # application-owned retry loop
    try:
        pad = touchy_open()
    except (DeviceNotFoundError, TouchyError):
        time.sleep(RETRY_S)          # unplugged or busy; wait, then retry
        continue
    try:
        while pad.event_thread_alive:
            time.sleep(1.0)
        # The reader stopped: the link failed or the stream ended.
        # The fatal traceback is in the ``touchy_pad.api.device`` logger.
    finally:
        pad.close()
    time.sleep(RETRY_S)
```

It is `True` only while the reader is alive and shutdown has not been
requested. It is `False` when the object was created with
`start_event_thread=False`, after `close()`, and after the reader exits for
any reason. It describes the **background reader**, not the remote device: a
reader can still be polling a device that has stopped responding. If you
disable the background thread and poll `event_consume()` yourself (as the
StreamDeck shim does), do not use this predicate as a connection-health
check.

Recovery is yours to write: closing and reopening is the supported path.
The library does not reconnect automatically, does not change `close()`
behaviour, and does not wrap transport exceptions.

## Authoring screens

The high-level **screen DSL** lives in `touchy_pad.api.screens` (and is
re-exported from `touchy_pad.api` for short imports):

```python
from touchy_pad.api import Screen, button, label, row, grid, style

s = Screen("home", layout=grid(cols=3, rows=4, gap=8))
s += label("title", text="Touchy-Pad")
s += button("go", text="Go",
            style=style(bg_color=0x1E90FF, text_color=0xFFFFFF))

with touchy_open() as pad:
    pad.screen_save(s)         # uploads to F:host/s/home.pb
    pad.screen_load("F:host/s/home.pb")
```

`Screen.to_proto()` is also exposed for advanced users who want the raw
protobuf message before upload.

### Rich text with spans (stage LB24) — the recommended way

To put **several colours in one text layout**, use a span group: a paragraph made
of styled *runs*.

```python
from touchy_pad.api import SpanOverflow, TextAlign, span, span_group

s += span_group(
    "quip",
    [
        span("cocks head", color=0xA020F0, font_size=30),
        span("I bet he's in a cave", color=0xFFFFFF),
    ],
    overflow=SpanOverflow.SPAN_OVERFLOW_ELLIPSIS,   # … on the last line that fits
    max_lines=3,                                    # 0 = unlimited
    text_align=TextAlign.LEFT,
    rect=rect(x=8, y=8, w=464, h=140),
)
```

`span(text, color=None, font_size=None)` is one run; `text` is **literal** (no
markup is interpreted inside it), `color=None` inherits the group's
`text_color` style, `font_size=None` the group's font. A bare `str` in
`spans=[...]` is shorthand for an uncoloured run. Size comes from the usual
placement (`rect` / a flex or grid parent): give it a fixed width to wrap, leave
the height unset to grow with the text.

**Live updates stay one RPC per frame.** A `text` property override whose value is
recolour markup is re-parsed into the span list on the device:

```python
from touchy_pad.api import build_property_override

pad.set_properties([build_property_override("quip", "text", "#A020F0 cocks head#I bet…")])
```

(If you'd rather author explicit spans host-side, `parse_recolor_markup(text)`
returns a `list[Span]` from that same markup.) A span group **cannot scroll** —
for a coloured *ticker*, keep using `label(..., recolor=True,
long_mode=LONG_MODE_SCROLL*)` below.

### Inline colour on a label (stage LB17) — compatible spelling

`label(..., recolor=True)` still means "this string carries recolour markup":

```python
s += label("marquee", text="#FF0000 Danger!# watch out", recolor=True,
           long_mode=LongMode.LONG_MODE_SCROLL_CIRCULAR)
```

A `#RRGGBB text#` run is drawn in that colour; anything *outside* a run uses the
label's `text_color` style (white by default). The wire format and the API are
unchanged since LB17 — but **since LB24 the device renders this markup as a span
group** rather than through LVGL's label recolour parser, which loses its command
state at a line start: a run that straddled an automatic wrap used to lose its
colour, swallow the next word, overrun the box and draw the seam twice. That is
fixed for every existing host with no code change (see
[`docs/plans/span-rich-text.md`](plans/span-rich-text.md)). The one exception is
the two scrolling long modes, which keep the label path (a span group cannot
marquee, and an unwrapped scrolling line never hit the bug).

Off by default (the markup then renders literally). Note `#` is the command
delimiter in *this* syntax: LVGL's `##` escape is dead code, so a literal `#` is
swallowed rather than shown — strip it host-side if it matters.

### Scrolling speed (stage LB19)

A label that scrolls — `long_mode=LongMode.LONG_MODE_SCROLL` (bounce) or
`LONG_MODE_SCROLL_CIRCULAR` (continuous marquee) — scrolls at **40 px/s** by
default. Two mutually exclusive kwargs set the speed:

```python
s += label("ticker", text="#00FF00 live# " * 3, recolor=True,
           long_mode=LongMode.LONG_MODE_SCROLL_CIRCULAR,
           scroll_speed_px_s=120,      # or: scroll_duration_ms=4000
           rect=rect(x=0, y=0, w=480, h=24))
```

| | Meaning | When to use it |
|---|---|---|
| `scroll_duration_ms=MS` | Exact LVGL-native knob: milliseconds for **one traversal** (a "lap"), honoured verbatim. Smaller = faster. | You know the lap time you want, or you want a `SCROLL` *bounce* that always takes the same time end-to-end. |
| `scroll_speed_px_s=PX` | Speed in pixels per second, `SCROLL_SPEED_MIN_PX_S`…`SCROLL_SPEED_MAX_PX_S` (10…10000). The device computes the exact lap duration from the label's real text width (no upper lap-time clamp), so a long label on a small display still moves at the requested px/s. | You want the text to *look* like it moves at a constant speed — including after a runtime `text` override changes the text length. |

The two differ only in what stays constant when the text changes underneath: a
*duration* keeps the lap time (longer text therefore scrolls faster), a
*speed* keeps px/s (longer text therefore takes longer per lap). Pass one or the
other — passing both raises `ValueError`.

**Neither field does anything unless the text overflows a fixed box**: the
scroll modes only animate when the label's width is set (via `rect` or a sizing
parent such as flex grow / grid stretch) and is narrower than the text.

#### What "the default" actually is

Everything above is LVGL's long-mode engine, so the defaults are LVGL's:

| Long mode | Behaviour | Default speed |
|---|---|---|
| `WRAP` / `DOTS` / `CLIP` | no animation | — |
| `SCROLL` (bounce) | slides out and back, pausing **300 ms** at each end | **40 px/s** → lap = `clamp(1000 × distance_px / 40, 300 ms, 10 000 ms)` |
| `SCROLL_CIRCULAR` | continuous loop, **3 space-glyphs** of gap between laps | same **40 px/s**, same 300 ms…10 s clamp |

Concretely (LVGL 9.5): `LV_LABEL_DEF_SCROLL_SPEED = lv_anim_speed_clamped(40,
300, 10000)` (`lv_label.c`), `LV_LABEL_SCROLL_DELAY = 300` ms and
`LV_LABEL_WAIT_CHAR_COUNT = 3`. The distance one lap covers is
`text_width + 3 × space_width` for `SCROLL_CIRCULAR`, or
`text_width − visible_width` for one leg of `SCROLL`. Since the lap time is a
function of both the speed *and* the text length, the equivalent conversion for
a host that measures its own text is:

```
duration_ms ≈ 1000 × distance_px / speed_px_s
```

At the default 40 px/s a 480 px-wide lap takes about 12 s — set
`scroll_duration_ms` / `scroll_speed_px_s` if you want it brisker.

> **Note:** the 300 ms…10 s clamp above is LVGL's *default* (unset) behaviour.
> An explicit `scroll_speed_px_s` does **not** use LVGL's speed encoding — the
> firmware computes the exact lap duration from the label's real text width, so
> there is no upper clamp. This matters on small displays: LVGL's encoding packs
> the lap into a 10-bit field and caps it at ~10.23 s, so a long label (e.g. 200
> chars on an 8×8 LED matrix, ~1000 px) at 10 px/s would otherwise scroll ~10×
> too fast.

#### Re-timing a live label (no re-upload)

The knob is LVGL's `anim_duration` **style property**, so the stage-lb14/lb12
runtime property path can retime a marquee in place — no screen re-upload, and
it survives screen reloads for the session:

```python
from touchy_pad.api import build_property_override

pad.set_properties([build_property_override("ticker", "anim_duration", 1000)])  # ms/lap
```

An `int` value is required: LVGL style properties are numbers, so send the
duration (a *speed* is an internal encoding meant for authoring time). From the
shell the same thing is
`touchy property set ticker anim_duration 1000 --int` — the `--int` flag is the
only way for the CLI to reach a style property, because a plain VALUE is sent as
a string.

## Raw protobuf

When you'd rather hand-build the message:

```python
from touchy_pad.api import protobuf, touchy_open

msg = protobuf.Screen()
# The wire-format version lives on `Widget` (there is no `Screen.Version`
# since Stage 56) and only the *root* widget of the file carries it — for a
# screen file that is `active`. The firmware validates it on load.
msg.active.version = protobuf.Widget.Version.CURRENT
msg.active.layout_flex.flow = protobuf.LayoutFlex.ROW
msg.active.layout_flex.layout.children.add(
    label=protobuf.Label(text="raw"),
)

with touchy_open() as pad:
    # A raw `protobuf.Screen` has no name field (only the host DSL's
    # `Screen("name")` does), so pass `name=` to pick the upload path
    # (`F:host/s/<name>.pb`).
    pad.screen_save(msg, name="raw")
```

`pad.screen_save()` accepts any of:

* a host-DSL `Screen`
* a raw `protobuf.Screen`
* a `dict` of JSON-shaped screen data (camelCase fields)
* a `pathlib.Path` or `str` pointing at a `.json` file

## Uploading arbitrary files

`pad.file_save(path, data)` writes one file to the device atomically.
Paths are **drive-prefixed** — pick the filesystem with the leading
letter:

* `F:host/...` — persistent flash (LittleFS). Use for screens, fonts,
  long-lived images. Survives reboot.
* `R:host/...` — transient PSRAM (`RamFs`). Use for assets that change
  often (e.g. StreamDeck-style key icons) — avoids wearing the flash.
  Lost on reboot.

```python
from pathlib import Path

with touchy_open() as pad:
    # 16x16 PNG → auto-converted to LVGL .bin and stored as
    # F:host/images/smiley.bin on flash.
    pad.file_save("F:host/images/smiley.png", Path("smiley.png").read_bytes())

    # A frequently-replaced key icon goes in PSRAM.
    pad.file_save("R:host/images/key_0.png", Path("key_0.png").read_bytes())
```

Behind the scenes `file_save` drives a streaming protocol
(`File_Open_Write` → `File_Write*` → `File_Close`) in ≤4 KiB USB-bulk
chunks, so arbitrarily large files upload without needing the device
to buffer the whole payload. From the caller's point of view it's a
single atomic operation — interruptions surface as exceptions and
leave no partial file behind.

`pad.file_delete(path)` removes one file or a whole subtree. The
convenience `pad.file_reset()` is shorthand for
`pad.file_delete("F:host")` and wipes the entire host-uploaded flash
area.

## Macros

The keyboard / mouse macro DSL is in `touchy_pad.api.macros`:

```python
from touchy_pad.api import macros, hid_keys, macro_action, button

go = button("paste", text="Paste!", on_press=macro_action([
    macros.key_tap(hid_keys.KEY_V, modifiers=hid_keys.MOD_LEFT_CTRL),
]))
```

## Event callbacks

The easiest way to react to a widget is to attach the callback **inline**
when you build it, with `host_action(on_event=...)`:

```python
from touchy_pad.api import touchy_open, Screen, button, host_action

s = Screen("home")
s += button(
    "ping",
    text="Ping host",
    on_click=host_action(on_event=lambda e: print("widget", e.user_data, "fired")),
)

with touchy_open() as pad:
    pad.screen_save(s)                    # uploads to F:host/s/home.pb; callbacks wired up here
    pad.screen_load("F:host/s/home.pb")   # screens load by path, not by bare name
    # ...other work...
```

`host_action(on_event=...)` allocates a unique host code for you (from a
reserved range starting at `0x10000`) and remembers the callback. When the
screen or widget that references it is uploaded — via `pad.screen_save(...)`
or `pad.widget_save(...)` — the callback is registered on that `Touchy`
automatically. No separate `on_host_event` call is needed.

The callback receives the full `LvEvent`; inspect `evt.user_data` (the
widget id), `evt.value` (sliders), `evt.checked` (toggles / checkboxes),
and `evt.host_code`.

### Lower-level: explicit codes

You can still manage host codes yourself. Pass an explicit numeric `code`
to `host_action` (keep it **below** `0x10000` so it never collides with the
auto-allocated range) and dispatch it with `on_host_event`:

```python
s += button("ping", on_click=host_action(0x100))

with touchy_open() as pad:
    pad.on_host_event(0x100, lambda e: print("ping", e.user_data))
```

Callbacks run on the poller thread, so:

* keep them short — long-running callbacks block USB polling;
* protect any shared state with your own lock;
* multiple callbacks can be registered for the same code; they are
  invoked in registration order.


## Running actions device-side

`TouchyClient.run_actions(actions)` — reachable as
`pad.client.run_actions(...)` — asks the device to execute a list of
protobuf `Action`s exactly as if a local widget had triggered them. The
most common use is retargeting the `widget_ref(id="page")` of the default
chrome to an uploaded user-screen body so it jumps to the front, which the
high-level API wraps as `pad.show_user_screen(name)`:

```python
from touchy_pad.api import protobuf

act = protobuf.Action()
act.device.change_widget_ref.behavior = (
    protobuf.ActionChangeWidgetRef.Behavior.BY_PATH
)
act.device.change_widget_ref.path = "F:host/uscr/opendeck.pb"
act.device.change_widget_ref.target_id = "page"

with touchy_open() as pad:
    pad.user_screen_save("opendeck", widget)  # body → F:host/uscr/opendeck.pb
    pad.show_user_screen("opendeck")          # bring it to the front…
    pad.client.run_actions([act])             # …or hand the raw action over
```

This works against both real hardware and the simulator (headless too).

## Overriding widget properties at runtime

`pad.set_properties([...])` applies a **batch** of LVGL property overrides
in one RPC (stage lb14 / googly-vr) — build the entries with
`build_property_override` and send them all at once, without re-uploading
the screen:

```python
from touchy_pad.api import Color, Point, build_property_override

with touchy_open() as pad:
    pad.set_properties([
        build_property_override("welcome", "text", "New message"),  # label text
        build_property_override("box", "bg_color", Color(0xFF8800)),  # colour
        build_property_override("box", "x", 12),  # integer prop
        build_property_override("welcome", "text", None),  # remove override
    ])
```

Each entry's *prop* is an LVGL **property name** (a short name resolved
on-device, e.g. `"text"`, `"bg_color"`, `"x"`) or an `int` raw
`lv_prop_id_t`. The *value* type selects the wire payload:

| Python value            | Property kind        |
|-------------------------|----------------------|
| `bool`                  | boolean              |
| `int`                   | integer              |
| `int` on `"text_font"`  | **font size** (px; `FontSize` / `FONT_SIZES`) |
| `str`                   | text (e.g. label)    |
| `Color(0xRRGGBB)`       | colour               |
| `Point(x, y)`           | point                |
| `None`                  | **remove** the override |

`Color` and `Point` (both importable from `touchy_pad.api`) are required
wrappers: a bare `int` always means an integer property, so colours and
points must be wrapped to be distinguishable. `"text_font"` needs no wrapper
because the property *name* settles it — see "Changing a font size at
runtime" below. It accepts **only** an int size (or `None` to remove): LVGL's
`text_font` value is a font pointer, so any other type raises `TypeError`
rather than being sent as something the device must read as a pointer.

Overrides are **session-scoped** (RAM only, never persisted) and
**sticky**: the device re-applies them whenever the target widget is
(re)built and immediately if it is already on screen — so you can even set
a property before its widget is loaded. The batch is applied atomically
under one device-side lock, in order; a malformed entry fails the RPC but
never aborts the remaining entries, and an absent widget is not an error.

A batch also counts as **display activity** on the device: it resets the
backlight auto-sleep countdown (and wakes a slept panel), just like a touch or
`pad.client.screen_wake()` — so animating widgets at a few Hz keeps the display
awake without any extra wake calls (stage lb15).

The CLI keeps a string-only shortcut:
`touchy property set WIDGET_ID PROPERTY VALUE` (sent as a one-entry batch).

A `text` override targets a **button**, a **label**, or a rich-text span group
(stage LB24). On a span group (and on a `recolor` label, which the device builds
as one) the string is recolour **markup** and is re-parsed into the group's runs —
still one RPC per frame. Removing an override (`value=None`) drops it from the
device's sticky table; the widget's original text reappears the next time it is
(re)built — a screen load or a page swap — not instantly. That is unchanged from
the label behaviour.

> One consequence of that: because a `recolor` label is rendered as a span group,
> the *label-only* properties (`long_mode`, `text_selection_start` / `_end`) no
> longer resolve on it — a `set_properties` entry naming them fails with
> "unknown property". `text` is routed to the markup parser and works as always.
> If you need a scrolling label, set `long_mode` in the screen itself.

The **simulator** honours the documented subset `x` / `y` / `w` / `h`
(geometry, via the widget's rect), `bg_color` (style) and `text`
(button/label) at the proto level — entries for other properties, or for
widgets without a rect placement, log a warning and are ignored. This is
what lets host animation apps (e.g. `googly-vr`) be developed
against the sim with visible motion. A `text_font` size is applied to the
widget's `font_size` style (the sim renders it for labels and span groups).

### Changing a font size at runtime

A label's (or button's / span group's) font size is a **style** property in
LVGL, so it can be changed live with the same `set_properties` path — the
property name is `"text_font"` and the value is a pixel size:

```python
from touchy_pad.api import FontSize, build_property_override, touchy_open

with touchy_open() as pad:
    pad.set_properties([
        build_property_override("title", "text_font", FontSize.FONT_SIZE_30),
        # …or a plain int: 30 — same thing
        # back to the theme default:
        # build_property_override("title", "text_font", FontSize.FONT_SIZE_DEFAULT),
    ])
```

That is one RPC, no screen re-upload, and the override is **sticky** — it
survives screen reloads and page swaps until the device reboots or you remove
it (`value=None`). `0` (`FontSize.FONT_SIZE_DEFAULT`) *drops* the local font so
the widget inherits its style stack and the theme again.

#### Which sizes are valid

The device renders only the fonts its build compiled. `FONT_SIZES` (derived
from the `FontSize` enum, so the two can never drift) is the published list:

```python
from touchy_pad.api import FONT_SIZES, FontSize

FONT_SIZES              # (8, 14, 16, 30)
FontSize.FONT_SIZE_30   # 30 — the members ARE the pixel sizes
```

* `8` is LVGL's **UNSCII 8** (a monospace bitmap face — the only legible font
  on an 8-pixel-tall LED matrix), the rest are Montserrat.
* `0` / unset means the **theme default**, which is Montserrat **14**.
* Any *other* number is not a request the device can honour: `font_for_size()`
  exact-matches the compiled fonts and otherwise leaves the theme font in place
  — so `font_size=24` would silently render at 14 px.

`label(…, font_size=…)`, `span(…, font_size=…)`, `style(font_size=…)` and
`build_property_override(…, "text_font", …)` therefore **raise `ValueError`**
for a size outside that list (listing it in the message) instead of letting the
device render something you did not ask for. Build the list into your UI:

```python
for size in FONT_SIZES:
    page += button(f"size_{size}", text=str(size), font_size=size)
```

From the shell, `--int` on `text_font` is the same call:

```sh
touchy property set title text_font 30 --int
```

## Lifecycle

```python
with touchy_open() as pad:
    ...
```

## Version compatibility

`touchy_open()` queries the device's USB protocol version on open and
raises `IncompatibleFirmwareError` if it's older than
`MINIMUM_FIRMWARE_VERSION`. Update the firmware on the device with "touchy update" if necessary.
