# touchy-opendeck

An [OpenDeck](https://github.com/nekename/OpenDeck) **device plugin**
that exposes a Touchy-Pad device as a virtual StreamDeck. Once the
plugin is installed and OpenDeck is restarted, plugging in a Touchy-Pad
makes it appear in OpenDeck's device list — touches on the LCD fire
whatever OpenDeck actions the user wired up, and images placed on
OpenDeck keys render on the device.

This crate is part of the [`touchy-pad`](https://github.com/geeksville/touchy-pad)
project; see [`docs/opendeck-device-plugin.md`](../../docs/opendeck-device-plugin.md)
for a generic guide to writing OpenDeck device plugins.

## How it works

```
+------------+   stdio JSON    +-----------------+  vendor-bulk USB  +--------------+
|  OpenDeck  | <-------------> |  touchy-opendeck| <---------------> |  Touchy-Pad  |
|  (Tauri)   |   openaction    |   (this crate)  |   touchy-pad lib  |   firmware   |
+------------+                 +-----------------+                   +--------------+
```

* `main.rs` calls `openaction::run` which speaks the OpenDeck plugin
  protocol over the WebSocket OpenDeck spawned us on.
* `plugin.rs` implements `openaction::global_events::GlobalEventHandler`.
  On `plugin_ready` it kicks off a hot-plug watcher that polls
  `touchy_pad::transport_usb::enumerate` once a second.
* When a new device shows up, `TouchyPlugin::attach` opens a USB
  transport via the `touchy-pad` crate, uploads a fresh grid of
  `ImageButton` widgets (see `layout.rs`), spawns a task that forwards
  `LvEvent`s back to OpenDeck as `key_down`/`key_up`, and calls
  `device_plugin::register_device`.
* Inbound `device_plugin_set_image` events carry data URLs; we
  base64-decode the payload, call `pad.file_save` (which transparently
  converts PNG/JPEG/etc. to LVGL's `.bin` format and rewrites the
  path), and debounce a single `screen_load` 100 ms after the last
  image arrives so a profile switch's burst of N updates only triggers
  one redraw.

### Device-ID scheme

OpenDeck requires every device ID to start with the plugin's two-char
`DeviceNamespace` (here `"tp"`). We derive IDs from the USB bus +
device-address pair: `tp-<bus_hex><addr_hex>`. This is stable for the
lifetime of a port; if you replug into a different port the device gets
a new ID (and OpenDeck treats it as a fresh device with a fresh layout).

### Host-code allocation

Each on-screen key needs an `ActionHost.code` to route press/release
events back to the right plugin. We reserve `0xB000..=0xBFFF` for this
plugin. The Python `TouchyDeck` shim uses `0xA000..=0xAFFF`, so the two
can coexist on a single device.

### Device paths

Both screen and per-key assets live on the device's PSRAM ramdisk
(`R:`) — not flash. Rationale: OpenDeck rewrites them constantly when
the user switches profiles, and flash wear from per-keypress reloads
would be silly.

```
R:host/screens/opendeck_<device_id>.pb      ← encoded layout
R:host/opendeck/<device_id>/key_<k>.bin     ← per-key images
```

### Brightness mapping

Touchy-Pad's host RPC surface doesn't (yet) expose a brightness knob.
`device_plugin_set_brightness` is therefore best-effort:

* `brightness == 0` → `screen_sleep_timeout(1)` (sleep ASAP)
* `brightness > 0`  → `screen_wake`

This is documented as a known limitation; a future host-API addition
should add a real brightness command.

### Event mapping

```
LV_EVENT_PRESSED      (1)  →  device_plugin::key_down(device, key)
LV_EVENT_RELEASED     (8)  →  device_plugin::key_up  (device, key)
LV_EVENT_PRESS_LOST   (7)  →  device_plugin::key_up  (device, key)
```

Anything else is dropped. `key` is recovered from the `host_code` via
`layout::key_for_host_code`.

## Building

The plugin is a normal Cargo workspace member; build it with the
project's `just` recipe:

```sh
just opendeck-build         # cargo build --release for the host triple
just opendeck-package       # bundle the .sdPlugin folder into a zip
```

The package recipe writes the binary into
`com.geeksville.touchypad.sdPlugin/<target-triple>/bin/` and zips that
folder into `rust/target/touchy-opendeck.sdPlugin.zip`. For
cross-target builds (Linux × macOS × Windows × aarch64) you'll want
something like `cargo-zigbuild` or a CI matrix; the manifest already
advertises all five common triples.

## Installing into OpenDeck

1. `just opendeck-package`
2. In OpenDeck, *Settings → Plugins → Install plugin from file…* and
   pick the generated `.sdPlugin.zip`.
3. Restart OpenDeck. Plug in a Touchy-Pad; it should appear in the
   device list as "Touchy-Pad (tp-<bus><addr>)".

On Linux, make sure the udev rule from
[`bin/99-touchy-pad.rules`](../../bin/99-touchy-pad.rules) is installed
so the OpenDeck-launched plugin has permission to talk to the device.

## Development tips

* Run the plugin standalone with `RUST_LOG=debug ./target/debug/touchy-opendeck`
  — it'll just sit there waiting for OpenDeck's args, but you get
  startup smoke-tests.
* OpenDeck's own log on Linux:
  `~/.local/share/com.amansprojects.opendeck/logs/`. Our stderr lands
  there too.
* To iterate quickly, point OpenDeck at a debug build by editing the
  installed manifest's `CodePaths` to absolute paths.
* All logging in this crate goes to **stderr**. Never `println!` —
  OpenDeck reads stdout as JSON.

## License

GPL-3.0-or-later, matching the rest of the `touchy-pad` project.
