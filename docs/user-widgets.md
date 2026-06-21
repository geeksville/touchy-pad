# User widgets

Adopt that as the standard 'better than streamdeck' replacement.  so people can make screens with guis that do interesting things for any foreground app. 

* app triggers - skip in layout when indicated app is not foreground
* allow multiple per uscr.  allow multiple uscrs
* python callbacks for behaviors / datasources
* url based fetching/sharing/registry (new? stream controler? opendeck?)
* asset packs?  
* let user pair python handler code with device GUI yaml?  
* Make a few examples (fps widget? steam coverart? perfmon?).  
* Extend those uscrs with triggers of "show when app X is in foreground".  
* add some of the example JSON to the API docs

* Possibly esphome yaml https://esphome.io/components/lvgl/ and https://esphome.io/components/image/#display-image 

## dynamic rendering

Today an image widget is backed by a *file already on the device*: the
host converts a PNG/JPEG to LVGL `.bin` and uploads it once, then
`image(..., asset="F:host/images/foo.png")` just points a widget at that
path. There is no way for an API client to say "here is a freshly
rendered bitmap, show it now" — let alone "keep this widget showing the
output of this render function". This section plans that feature.

### Goals

1. Let `image()` / `image_button()` accept **in-memory image content**
   (a `PIL.Image.Image`, raw `bytes`, or a render callable) in the same
   `asset=` slot that today takes a path `str`.
2. Give clients a **clean way to push a new bitmap into an existing
   widget** ("rerender now").
3. Build a **periodic / on-demand refresh** model on top of (2) so a
   widget can track a live data source (FPS, cover art, perfmon) without
   the caller writing its own upload/redraw plumbing.
4. Do all of this without leaking device-storage details (ramdisk vs.
   flash, content hashing, redraw triggering) into user code.

### The "rerender" API (the part to get right)

The primitive is a host-side **image source** object. Both `image()` and
`image_button()` accept it anywhere they accept a path string. The
source owns a stable on-device asset path; calling `.update()` re-renders
and re-uploads, and *because the path is stable the widget redraws in
place* (no screen rebuild, no lost touch state — see "Redraw" below).

```python
from touchy_pad.api import touchy_open, screens
from touchy_pad.api.screens import image, ImageSource

with touchy_open() as pad:
    fps = ImageSource(render_fps)          # render_fps() -> PIL.Image | bytes
    pad.user_screen_save("hud", image("fps", asset=fps))

    while running:
        fps.update()                       # re-render + push *only if changed*
```

`ImageSource(value)` accepts:

* a `PIL.Image.Image` or `bytes` — a static source you can still mutate
  later via `source.update(new_image)`;
* a zero-arg callable returning a `PIL.Image.Image` / `bytes` — calling
  `.update()` (no args) invokes it to produce the next frame.

`.update(new=None)` is the single primitive:

* renders (calls the callable, or uses the passed `new`, or re-encodes
  the stored image),
* content-hashes the encoded `.bin`, and **skips the upload entirely if
  the bytes are unchanged** (the expensive part is the wire transfer),
* otherwise rewrites the file, which triggers the in-place redraw.

The **periodic** API is then *just sugar over the on-demand one* — a
host-side timer that calls `.update()` on a schedule, which is exactly
the "ideally the on-demand api implements the periodic api" goal:

```python
fps = ImageSource(render_fps, every=1.0)   # background timer calls .update()
pad.user_screen_save("hud", image("fps", asset=fps))
fps.start()        # spawn the refresh thread/task
...
fps.stop()         # also happens on pad close / context exit
```

`every=` is optional; when omitted the source is purely on-demand. The
timer is a thin `threading.Timer`/asyncio loop that does nothing but call
`.update()`, so there is one code path and `every=` never gets a
behaviour the manual path lacks. `ImageSource` is registered with the
open `Touchy` so its timer is torn down automatically when the device
connection closes.

### On-device storage: a new `T:` ("temp") drive

Generated frames are transient, so we introduce a dedicated **`T:`
drive** as the canonical home for host-generated, throwaway assets. `T:`
is a *logical* drive the device/simulator resolves to whatever transient
backing store it has:

* On PSRAM boards `T:` maps to the existing **`R:` ramdisk** (PSRAM) —
  fast, wiped on reboot, no flash wear.
* On tiny no-PSRAM boards (CYD, ~520 KB SRAM) the RamFs is too small, so
  `T:` maps to a flash scratch area such as `F:tmp/`. The host does not
  need to know *which* — it always writes `T:...` and the device picks
  the backing store. The host still wants to *know* whether `T:` is
  flash-backed (to throttle high-frequency refreshes and warn about
  flash wear), so we also surface `bool temp_is_flash` on
  `SysBoardInfoResponse` (additive scalar, bump `ProtocolVersion`; today
  `V8`).

Why a real drive letter instead of hard-coding `R:` in each caller:
making `T:` a first-class concept means callers (Python `ImageSource`,
Rust `ImageCache`, future ones) never branch on board capabilities — the
*device* owns the ramdisk-vs-flash decision. **`R:` stays a fully
supported, explicit drive** for callers that specifically want the PSRAM
ramdisk (and accept that it is tiny / may fail on no-PSRAM boards); `T:`
does not replace it, it sits alongside `F:` and `R:` as the
"device-chooses" transient option. This is a behaviour change only for
the Rust `ImageCache`, which currently writes to `R:host/icache/`
directly and moves to `T:`; see "Rust backport" below.

Layout: each `ImageSource` owns one **stable** path `T:dyn/<n>.bin`,
where `<n>` is a process-global counter that starts at `1` on app launch
and increments monotonically for every new `ImageSource`. The path is
fixed for the life of the source, so the widget always points at the
same file and a rewrite is all it takes to repaint (see "Redraw"). The
directory is wiped on first use per connection (same wipe-on-first-use
discipline `ImageCache` already uses).

### Rust backport: migrate `ImageCache` from `R:` to `T:`

The `T:` concept is **retrofitted into the existing Rust code**, not
limited to the new Python feature:

* `rust/touchy-pad/src/image_cache.rs` switches `IMAGE_CACHE_ROOT` from
  `R:host/icache/` to `T:host/icache/`, so the cache automatically lands
  on PSRAM where available and on the flash scratch area on tiny boards
  (today it would simply fail / waste the tiny internal RamFs).
* Centralise the `T:` prefix as a path constant (Rust `lib.rs`, Python
  `paths.py`) the way `R:`/`F:` are, so no caller hard-codes the letter.
* No host-side behaviour change beyond the drive letter — content-hash,
  LRU, and the OpenDeck plugin keep working unchanged.

### Device / simulator: teach them the `T:` drive

`T:` has to be resolvable wherever drive letters are parsed:

* **Firmware** filesystem layer (the drive-prefix dispatcher that today
  knows `F:` = LittleFS and `R:` = RamFs) gains `T:` → the platform's
  chosen transient store: RamFs when PSRAM is present, otherwise a
  flash scratch dir. `platform_get()` / `temp_is_flash` drive the
  decision in one place.
* **Simulator** drive resolution gains the same `T:` mapping (to a temp
  dir or in-memory store) so `T:host/...` round-trips through
  `set_image_update_callback` exactly like a real device.

### Redraw (firmware)

**Stable paths make this simple: one mechanism, file-rewrite.** Because
each `ImageSource` owns a fixed `T:dyn/<n>.bin` path, the widget is
built once pointing at that path and never needs its definition changed.
Rewriting the bytes at that path invalidates the LVGL image cache for
that asset and repaints *in place* — the widget object, its layout, and
its touch/press state are all untouched.

Crucially this is an **asset invalidation**, not a widget rebuild, so we
do **not** need `ActionChangeWidgetRef` / the Stage 86 slot swap for
dynamic images. (Stage 85's original repaint was painful precisely
because it rewrote the *widget ref* — deleting and rebuilding the held
button, which lost the keyUp and raced under bursts. Rewriting only the
backing `.bin` avoids that entirely.) Stage 86's slot swap remains the
right tool for swapping *between two pre-uploaded* button images on
press/release; it is orthogonal to this feature.

What the layers need:

* **Firmware**: an "asset at `path` changed → invalidate the LVGL image
  cache and `lv_obj_invalidate()` any live widget bound to that path"
  hook in the filesystem write path. A reverse index of
  `path → live widget(s)` (or a broadcast invalidate keyed on path) is
  enough.
* **`image_button`**: each slot (released / pressed) is just its own
  `Image.path`, so each can be its own `ImageSource` with its own stable
  `T:dyn/<n>.bin`. Rewriting either slot's file repaints that slot via
  the same hook — no special-casing.
* **Simulator**: already models exactly this via
  `set_image_update_callback(path)` in `sim/device.py`.

### Implementation outline

1. **`T:` drive plumbing first.** Add the `T:` prefix to firmware +
   simulator drive resolution (→ RamFs/PSRAM or flash scratch per
   board), centralise the constant in Rust `lib.rs` + Python `paths.py`,
   and add `bool temp_is_flash` to `SysBoardInfoResponse` (+ firmware
   fill in `host_api.cpp`, + `board-info` CLI line, bump
   `ProtocolVersion`).
2. **Rust backport.** Repoint `ImageCache`'s `IMAGE_CACHE_ROOT` from
   `R:host/icache/` to `T:host/icache/`; verify content-hash/LRU/OpenDeck
   paths are unaffected.
3. **`ImageSource`** in `app/src/touchy_pad/api/` — holds the source
   value/callable, owns a stable `T:dyn/<n>.bin` path (monotonic counter
   from app launch), encodes via the existing `lvgl_image.to_lvgl_bin`,
   content-hash dedups, and `pad.file_save`s on change. `.update()` +
   optional `every=` timer.
4. **`image()` / `image_button()`** accept `ImageSource` (and bare
   `PIL.Image`/`bytes`, auto-wrapped) wherever `asset: str` is taken;
   `_fill_image` resolves a source to its stable on-device path. For a
   button, the released and pressed slots may each take their own
   `ImageSource`.
5. **Firmware redraw-on-rewrite** hook: asset-at-`path` invalidation
   that repaints any live widget bound to that path in place (no widget
   rebuild, no `ActionChangeWidgetRef`).
6. **Simulator** already has `set_image_update_callback`; ensure the
   `T:dyn/` path round-trips through it.

### Open questions

* Generator vs. callable vs. `PIL.Image` subscription — is a plain
  zero-arg callable enough, or do we want to accept an iterator/generator
  that is `next()`-ed each tick? (A callable is the simplest superset;
  a generator can be adapted with `lambda g=g: next(g)`.)
* Back-pressure: if `.update()` is called faster than the wire can push,
  do we coalesce (keep only the latest frame) or block? Coalescing is
  almost certainly right for a display.
* LRU/cleanup of `T:dyn/` across reconnects (wipe-on-first-use, like
  `ImageCache`).

### example of how existing usage works
```python
        image_button(
            "smile",
            asset="F:host/images/smiley.png",
            on_click=host_action(on_event=lambda e: logger.info("[smile]  widget=%r", e.user_data)),
        )
```

### example of using pillow for rendering text strings with rounded rect:
```python
from PIL import Image, ImageDraw

img = Image.new('RGB', (200, 100), color='black')
draw = ImageDraw.Draw(img)

# A one-liner, as the coding gods intended.
draw.rounded_rectangle([10, 10, 190, 90], radius=15, fill='blue')
draw.text((65, 45), "hello world", fill='white')

# img.show() or img.save() to prove it happened
```

### example of using weasyprint to render HTML as a pixel buf
Someday we might want to support this

```python
from weasyprint import HTML
import io
from PIL import Image

# Render HTML string directly to PNG bytes in memory
png_bytes = HTML(string="<h1>Hello <span style='color:blue;'>World</span></h1>").write_png()

# Load those bytes into a Pillow image
img = Image.open(io.BytesIO(png_bytes))
img.show()
```

## ai copypasta
misc notes on ideas...

### ksystemstats vs plasmoid

hmm... musings here... probaby no plasmoids at first - ksystemstats dbus queries instead. QQuickRenderControl looks awesome to allow a qt6 api for writing to touchies. see here
https://gemini.google.com/share/091d654412c4 

possibly render kde plasmoids https://store.kde.org/browse?cat=705&tag=plasmoid
https://userbase.kde.org/Plasma/Plasmoids 
https://medium.com/@linuxrootroom/what-is-plasmoid-in-kde-plasma-desktop-939717e498f0 

### steam cover art

Nope. There isn't a magical, all-in-one package that snoops on your active windows and hits up Valve's servers for high-res JPEGs. You're asking for a Frankenstein library that crosses OS-level window management with web API scraping.

But this is Python, so you can easily duct-tape two tools together to do exactly this:

Spot the Game: Use pygetwindow (or win32gui on Windows) to grab the text title of whatever window is currently in the foreground.

Fetch the Data: Feed that window title into a wrapper like python-steam-api to search the Steam store and extract the game's unique AppID.

Once you have the AppID, you don't even need a library to get the art. Steam's content delivery network uses completely predictable URLs. Just plug the ID into this link and grab it with the requests library:

https://steamcdn-a.akamaihd.net/steam/apps/<APP_ID>/library_600x900.jpg

### mangohud

MangoHud doesn't expose a fancy API, D-Bus interface, or a magical JSON socket for you to live-query its metrics. It's a selfish overlay that hooks directly into the Vulkan/OpenGL swapchain and keeps its performance secrets strictly on your screen.

2. Read the Source Yourself
MangoHud doesn't use black magic to get its info. If you want the raw data, bypass the middleman entirely. You can programmatically read /sys/class/drm/ for AMD/Intel stats, use nvidia-smi for Nvidia, and parse lm-sensors for standard hardware metrics.