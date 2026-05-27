# Writing an OpenDeck device plugin

OpenDeck (<https://github.com/nekename/OpenDeck>) is an open-source
StreamDeck-compatible app. Out of the box it talks to genuine Elgato
hardware, but third-party hardware can plug in via **device plugins**:
external processes that present some piece of hardware (USB key matrix,
LCD touchpad, MIDI controller, anything) to OpenDeck as if it were a
StreamDeck.

This guide collects what you need to know in one place. It is written
from the perspective of an external maintainer (the
[`touchy-opendeck`](../rust/touchy-opendeck) plugin in this repo), and
is intended to be portable enough to eventually contribute upstream.

It assumes basic familiarity with the OpenDeck app and with whatever
toolchain you use to talk to your hardware. Examples are in Rust because
that's where the official library binding lives, but the wire protocol
underneath is JSON over WebSocket and any language can drive it.

---

## 1. How OpenDeck loads a plugin

A plugin lives in a directory named `<reverse-dns-id>.sdPlugin/` —
e.g. `com.example.myhardware.sdPlugin/`. OpenDeck installs plugins under
its plugin directory (`~/.config/com.amansprojects.opendeck/plugins/`
on Linux). When OpenDeck starts it:

1. Reads `manifest.json` from each plugin directory.
2. Picks the entry from `CodePaths` (or `CodePathLin`/`Mac`/`Win`) that
   matches the host's Rust target triple.
3. Spawns that binary as a child process and connects to it over a
   WebSocket on `ws://localhost:<port>/`. The port and a registration
   UUID are passed on the command line — this is what
   [`openaction::run(std::env::args().collect())`](https://docs.rs/openaction/latest/openaction/fn.run.html)
   parses.
4. The plugin sends a `registerPlugin` event over the socket and from
   then on receives inbound events and sends outbound events.

Plugins shipped as a `.sdPlugin/` directory zipped up can be installed
from the OpenDeck UI ("Install plugin…" → pick the zip).

---

## 2. Action plugins vs device plugins

OpenDeck has two kinds of plugins:

| Plugin kind          | What it adds                                                              | Manifest hints                          |
| -------------------- | ------------------------------------------------------------------------- | --------------------------------------- |
| **Action plugin**    | New action UUIDs the user can drop onto a key (`runCommand`, `openUrl`…). | `Actions: [...]` is non-empty.          |
| **Device plugin**    | A new physical device that appears in the device list.                    | `DeviceNamespace` is set (2 chars).     |

Both can live in the same plugin (the manifest just sets both fields).
This guide focuses on the device-plugin half.

---

## 3. Manifest essentials

`manifest.json` is JSON with PascalCase keys (the parser also accepts
lowercase). The full schema lives in
[`OpenDeck/src-tauri/src/plugins/manifest.rs`](https://github.com/nekename/OpenDeck/blob/main/src-tauri/src/plugins/manifest.rs).
For a device-only plugin the interesting fields are:

```json
{
  "Name": "My Hardware",
  "Description": "Adds the Foobar Pro to OpenDeck.",
  "Author": "Jane Hacker",
  "Version": "0.1.0",
  "Icon": "icons/plugin",
  "Category": "My Hardware",
  "OS": [
    { "Platform": "linux" },
    { "Platform": "mac", "MinimumVersion": "10.15" },
    { "Platform": "windows", "MinimumVersion": "10" }
  ],
  "CodePaths": {
    "x86_64-unknown-linux-gnu": "x86_64-unknown-linux-gnu/bin/my-plugin",
    "aarch64-unknown-linux-gnu": "aarch64-unknown-linux-gnu/bin/my-plugin",
    "x86_64-apple-darwin": "x86_64-apple-darwin/bin/my-plugin",
    "aarch64-apple-darwin": "aarch64-apple-darwin/bin/my-plugin",
    "x86_64-pc-windows-msvc": "x86_64-pc-windows-msvc/bin/my-plugin.exe"
  },
  "Actions": [],
  "DeviceNamespace": "mh"
}
```

Things to know:

* **`DeviceNamespace` must be exactly two characters.** Every device ID
  your plugin registers **must start with this prefix** — OpenDeck uses
  the prefix to know which plugin owns a device. Pick something
  globally unique-ish (avoid `sd` which collides with Elgato).
* **`Icon`** is a path *without* the file extension. OpenDeck looks for
  `icons/plugin.png` next to the manifest. Same for icons referenced in
  `Actions`.
* **`Actions: []`** is required even for a device-only plugin (the
  field is non-optional in the schema).
* **`CodePaths`** is a map from Rust target triple → relative path to
  the binary inside the plugin directory. You can also use the legacy
  per-OS aliases (`CodePathLin`, `CodePathMac`, `CodePathWin`); OpenDeck
  uses the triple map first and falls back to the OS path.

The on-disk layout that OpenDeck expects is:

```
com.example.myhardware.sdPlugin/
├── manifest.json
├── icons/
│   └── plugin.png
├── x86_64-unknown-linux-gnu/bin/my-plugin
├── aarch64-unknown-linux-gnu/bin/my-plugin
├── x86_64-apple-darwin/bin/my-plugin
├── aarch64-apple-darwin/bin/my-plugin
└── x86_64-pc-windows-msvc/bin/my-plugin.exe
```

Cross-compiling for every target is the usual sticking point. CI
matrices and `cargo zigbuild` / `cross` are the standard answers.

---

## 4. The `openaction` Rust crate

If you're writing a plugin in Rust, [`openaction`](https://crates.io/crates/openaction)
is the official client library. It handles the WebSocket handshake,
parses the inbound JSON events, and lets you implement two traits to
react to them. Caret-semver pin: `openaction = "2"`.

The boilerplate is:

```rust
use openaction::*;
use openaction::global_events::{
    GlobalEventHandler, SetImageEvent, SetBrightnessEvent,
};

struct Handler { /* … */ }

#[async_trait::async_trait]
impl GlobalEventHandler for Handler {
    async fn plugin_ready(&self) -> OpenActionResult<()> {
        // Start any device discovery / hot-plug threads here.
        Ok(())
    }

    async fn device_plugin_set_image(&self, e: SetImageEvent)
        -> OpenActionResult<()> { /* … */ Ok(()) }

    async fn device_plugin_set_brightness(&self, e: SetBrightnessEvent)
        -> OpenActionResult<()> { /* … */ Ok(()) }
}

#[tokio::main]
async fn main() -> OpenActionResult<()> {
    simplelog::TermLogger::init(
        simplelog::LevelFilter::Info,
        simplelog::Config::default(),
        simplelog::TerminalMode::Stderr,
        simplelog::ColorChoice::Never,
    ).unwrap();

    // The handler must outlive the process; `Box::leak` is the
    // simplest way to satisfy the `&'static dyn` signature.
    let handler: &'static Handler = Box::leak(Box::new(Handler::new()));
    openaction::global_events::set_global_event_handler(handler);

    openaction::run(std::env::args().collect()).await
}
```

### Inbound events you receive (in `GlobalEventHandler`)

Every method has a no-op default; only implement the ones you care
about.

| Method                              | When OpenDeck sends it                                  |
| ----------------------------------- | ------------------------------------------------------- |
| `plugin_ready`                      | Once, right after the handshake. Start hot-plug here.   |
| `device_plugin_set_image`           | OpenDeck wants a key image drawn (or cleared).          |
| `device_plugin_set_brightness`      | User changed the brightness slider for your device.     |
| `device_did_connect`                | OpenDeck has acknowledged your `register_device` call.  |
| `device_did_disconnect`             | OpenDeck is forgetting the device (rare in practice).   |
| `did_receive_global_settings`       | Plugin-wide settings updated.                           |
| `application_did_launch/terminate`  | Application monitoring.                                 |
| `system_did_wake_up`                | The host left sleep — re-init USB if needed.            |

### Outbound calls you make (in `openaction::device_plugin`)

These are free functions, not methods. They send JSON over the socket.

```rust
use openaction::device_plugin::*;

// Tell OpenDeck a device is online.
register_device(
    /* id */       "mh-0001-0007".into(),  // must start with DeviceNamespace
    /* name */     "Foobar Pro".into(),
    /* rows */     3_u8,
    /* columns */  5_u8,
    /* encoders */ 0_u8,
    /* type */     7_u8, // see "Device type values" below
).await?;

unregister_device(id).await?;       // device unplugged
key_down(id.clone(), position).await?;
key_up(id, position).await?;
encoder_down / encoder_up / encoder_change(id, encoder, delta).await?;
rerender_images(id).await?;         // ask OpenDeck to resend every key image
```

### Device-type values

`type` is an arbitrary `u8`. OpenDeck uses it mostly for UI hints. The
values Elgato hardware reports (from
[`elgato.rs`](https://github.com/nekename/OpenDeck/blob/main/src-tauri/src/elgato.rs)):

| value | shape it suggests              |
| ----- | ------------------------------ |
| 0     | StreamDeck Original (3×5 grid) |
| 1     | StreamDeck Mini    (2×3 grid)  |
| 2     | StreamDeck XL      (4×8 grid)  |
| 5     | StreamDeck Pedal   (1×3, no LCD)|
| 7     | StreamDeck Plus    (2×4 + 4 encoders + touchstrip) |
| 9     | StreamDeck Neo                  |

For an unconventional device just pick whichever value renders
closest. If your device has no encoders and a single touch grid, `0`
(Original) is a safe default; pick `7` (Plus) if you have encoders.

### Device IDs

* Must be **stable across reconnects** (OpenDeck remembers per-device
  layouts by ID).
* Must start with your `DeviceNamespace` (e.g. `"mh"` → `"mh-…"`).
* Best practice: include serial number, USB bus+address, or a Bluetooth
  MAC — anything that uniquely identifies one physical unit.

---

## 5. Hot-plug discovery

OpenDeck does not enumerate hardware for you. Your plugin watches the
host for devices showing up/disappearing and calls
`register_device` / `unregister_device` accordingly.

Common patterns:

* **Polling** — easy and portable. Run a `tokio::time::interval(1s)`
  loop that calls your enumeration function and diffs against the set
  of devices you've already registered. Good enough for most plugins
  and works the same on every OS.
* **OS hot-plug API** — `nusb::watch_devices()`, libudev, IOKit,
  WM_DEVICECHANGE. Lower latency but more code.

For a USB device, filtering on VID/PID is enough. For Bluetooth,
WebSerial, network etc., adapt accordingly. The Elgato handler in
OpenDeck itself uses `elgato-streamdeck`'s discovery + `tokio::select!`
between a watch task and a per-device event task — that's a good model
to copy.

---

## 6. The image protocol

OpenDeck draws every key in the app (text, icons, fancy backgrounds,
plugin-supplied renders) and then ships the final bitmap to your
plugin as a **data URL**:

```rust
async fn device_plugin_set_image(&self, e: SetImageEvent)
    -> OpenActionResult<()>
{
    match (e.position, e.image) {
        (Some(pos), Some(data_url)) => {
            // data_url looks like "data:image/png;base64,iVBORw0KG…"
            let (_header, b64) = data_url.split_once(',').unwrap_or(("", ""));
            let bytes = base64::engine::general_purpose::STANDARD
                .decode(b64).unwrap_or_default();
            self.draw_key(&e.device, pos, &bytes).await;
        }
        (Some(pos), None) => self.clear_key(&e.device, pos).await,
        (None,      None) => self.clear_all(&e.device).await,
        _ => {}
    }
    Ok(())
}
```

A few subtleties:

* **Mime type varies.** Elgato hardware gets JPEG, some plugins get
  PNG. Be liberal — `image::load_from_memory` autodetects, or look at
  the data URL prefix.
* **Bursts.** Switching profiles fires one `set_image` per key in
  rapid succession. If pushing each pixel to hardware is slow,
  coalesce: queue the writes, debounce a "commit" call ~100 ms after
  the last update.
* **Clear vs draw.** `position == None` && `image == None` means
  "clear *every* key on the device". Don't unregister — just blank.

`encoder` slots use the same `SetImageEvent` shape with
`controller == Some("Encoder".to_string())`. Skip or handle as needed.

---

## 7. The event protocol

When the user touches a key, your plugin sends:

```rust
key_down(device_id.clone(), position).await?;
// …user lifts finger…
key_up(device_id, position).await?;
```

OpenDeck takes care of firing the action wired to that key. For
encoders use `encoder_down` / `encoder_up` / `encoder_change(_, _, delta)`
where `delta` is an `i16` of clicks.

If your hardware can drop a release (USB unplugged mid-press), still
send `key_up` so OpenDeck doesn't think the key is stuck. LVGL's
`PRESS_LOST` event class is the typical hook on touch devices.

---

## 8. Logging

* Set up `simplelog::TermLogger::init` writing to **stderr**, not
  stdout. OpenDeck multiplexes the stdio streams; logging to stdout
  corrupts the JSON channel.
* OpenDeck's own log (Linux:
  `~/.local/share/com.amansprojects.opendeck/logs/`) captures your
  stderr, so anything you log lands there.
* For local development, run the plugin manually:

  ```sh
  RUST_LOG=debug ./target/debug/my-plugin <port> <uuid> registerPlugin
  ```

  with whatever args OpenDeck would have passed. The
  [openaction docs](https://docs.rs/openaction/latest/openaction/fn.run.html)
  describe the schema. In practice the easier route is to launch
  OpenDeck pointing at a debug build via the `CodePaths` triple.

---

## 9. Packaging and distribution

There is no central plugin store yet (the user installs from a zip in
the OpenDeck UI), and there is no signing requirement.

A typical release artifact is a zip:

```
com.example.myhardware.sdPlugin.zip
└── com.example.myhardware.sdPlugin/
    ├── manifest.json
    ├── icons/plugin.png
    └── <target>/bin/my-plugin[.exe]   (for each shipped target)
```

The user installs it via *Settings → Plugins → Install plugin from
file…*. OpenDeck extracts it to its plugin directory and starts your
binary.

To distribute via the third-party [Tacto marketplace](https://marketplace.tacto.live/),
follow their submission rules; same zip, same layout.

---

## 10. Common pitfalls

* **Wrong `DeviceNamespace` prefix on device IDs** → OpenDeck silently
  ignores `register_device` calls. The mismatch is logged at debug
  level only.
* **Logging to stdout** → OpenDeck reads stdout as JSON and disconnects
  the plugin with a confusing parse error.
* **Blocking the tokio executor** with synchronous USB / image
  conversion → `set_image` events queue up and OpenDeck times out. Do
  conversion work on a `spawn_blocking` task or in a dedicated
  worker pool.
* **Not calling `unregister_device`** on detach → ghost devices stay in
  the OpenDeck UI until restart. Hook your hot-plug loop to clean up.
* **Forgetting `Actions: []`** → manifest fails to parse. The error
  message is opaque ("missing field `Actions`").
* **Cross-compiling forgot to strip / sign binaries** → macOS Gatekeeper
  rejects unsigned binaries. Either ad-hoc sign (`codesign -s -`) or
  document the user `xattr -d com.apple.quarantine` workaround.

---

## 11. Reference plugins

* **Elgato support inside OpenDeck** —
  [`OpenDeck/src-tauri/src/elgato.rs`](https://github.com/nekename/OpenDeck/blob/main/src-tauri/src/elgato.rs).
  Built-in, not a separate plugin, but the same outbound API.
* **AKP153 (Ajazz)** —
  [`4ndv/opendeck-akp153`](https://github.com/4ndv/opendeck-akp153).
  The canonical Rust device-plugin example. Watcher, per-device
  task, hot-plug, `handle_set_image`.
* **Starter pack (action plugin)** —
  [`com.amansprojects.starterpack.sdPlugin`](https://github.com/nekename/OpenDeck/tree/main/plugins/com.amansprojects.starterpack.sdPlugin).
  Action plugin, not a device plugin, but a clean reference for
  manifest + actions.
* **Touchy-Pad** —
  [`touchy-opendeck`](../rust/touchy-opendeck) in this repo. Device
  plugin where the "keys" are LVGL widgets drawn on a customisable
  LCD; useful if your hardware also renders its own UI.

---

## 12. Checklist

Before submitting a plugin:

- [ ] Two-char `DeviceNamespace`, unique-looking; every registered ID
      starts with it.
- [ ] Stable device IDs across reconnects.
- [ ] `Actions: []` present (even if empty).
- [ ] At least one `CodePaths` entry for each platform you advertise in
      `OS`.
- [ ] Manifest icon (`Icon` path + `.png` file) exists.
- [ ] `key_up` always follows `key_down`, even on disconnect.
- [ ] `unregister_device` is called on detach.
- [ ] Logging goes to stderr only.
- [ ] Image decode handles both PNG and JPEG mime types.
- [ ] `(None, None)` of `SetImageEvent` clears the whole device.
- [ ] Cross-compiled binaries for the targets in `CodePaths`.

Happy hacking.
