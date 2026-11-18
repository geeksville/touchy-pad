# Rust API

The `touchy-pad` crate is the async, pure-Rust counterpart to the
[Python `touchy_pad` package](python-api.md). It talks the same
self-synchronising protobuf protocol (see
[wire framing](host-api.md#wire-framing)) over the device's
vendor-specific bulk endpoints, with no libusb dependency. The TCP
(simulator) transport is always available; an optional `serial` cargo
feature adds a serial-port transport (via `tokio-serial`).

* Source: `rust/touchy-pad/`
* Demo binary: `rust/touchy-demo/`
* Edition: 2024
* MSRV: stable

## Adding it to your project

```toml
[dependencies]
touchy-pad = "0.3"
```

Building the crate needs **no `protoc` and no build script**: the prost bindings
are checked in at `rust/touchy-pad/src/proto/touchy.rs`, the same deal the Python
package makes by shipping its generated `_pb2` modules inside the wheel.
(`protoc` is only needed inside *this* repository, to regenerate them.)

The crate is on crates.io — API docs at [docs.rs/touchy-pad](https://docs.rs/touchy-pad).
You can also consume the source tree, which is what you want to track unreleased
changes:

```toml
touchy-pad = { git = "https://github.com/geeksville/touchy-pad" }
```

## Regenerating the bindings

`proto/*.proto` is the wire-format source of truth. After editing it:

```sh
just build-proto     # Python + C + Rust bindings, plus the embedded default screen
just rust-gen-proto  # Rust only (needs a `protoc` binary on PATH, or $PROTOC)
```

Never hand-edit the generated file — CI re-runs the generator and fails if the
committed copy differs.

## Examples

`rust/touchy-pad/examples/` holds runnable, compile-checked examples (they are
built by `cargo test` and `clippy --all-targets`, so they cannot rot):

```sh
cargo run --example discover      # enumerate USB / UART / simulator devices
cargo run --example screen        # build a screen from prost structs and show it
cargo run --example events        # stream touch events
cargo run --example image_cache   # upload an image once, repaint a key in place
```

## For AI agents

The crate docs carry a "For AI agents" block with the facts that most often lead
to wrong code (the device is a dumb renderer, `events()` is one-shot, images
convert themselves, drive-prefixed paths), and the repo-root `llms.txt` is the
wider orientation guide. docs.rs also serves **rustdoc JSON** for every
published version — `https://docs.rs/crate/touchy-pad/latest/json` — which is the
machine-readable form of this API. Release notes live in
[`CHANGELOG.md`](https://github.com/geeksville/touchy-pad/blob/main/CHANGELOG.md).

## Layout

| Module | Purpose |
|---|---|
| `touchy_pad::Touchy` | High-level handle; wraps `Client` + background event poller. |
| `touchy_pad::client::Client` | One-shot RPC methods (`sys_board_info_get`, `file_open_write`, `screen_load`, `run_actions`, …). |
| `touchy_pad::discover` | `discover()` + `DiscoveredDevice` — unified USB + sim enumeration. |
| `touchy_pad::transport::Transport` | Async trait — frame in, frame out. Mockable for tests. |
| `touchy_pad::transport_usb::UsbTransport` | Default `nusb`-backed bulk transport. |
| `touchy_pad::images` | LVGL `.bin` conversion (`to_lvgl_bin`, `LvFormat`, `normalize_for_device`). |
| `touchy_pad::image_cache::ImageCache` | Content-addressed, one-upload-per-image device cache. |
| `touchy_pad::error` | `TouchyError` + `Result<T>`. |
| `touchy_pad::proto` | Generated `prost` types (re-exposed for advanced callers). |

## Quickstart

```rust,no_run
use touchy_pad::Touchy;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let pad = Touchy::open().await?;

    // RPC: ask the device who it is.
    let info = pad.client().sys_board_info_get().await?;
    println!("board: {info:?}");

    // Upload an image; host auto-converts PNG/JPG to LVGL `.bin`.
    let png = std::fs::read("button.png")?;
    pad.file_save("R:host/demo/button.png", &png).await?;

    // Stream touch events.
    let mut rx = pad.events().await.expect("events() consumed once");
    while let Some(evt) = rx.recv().await {
        println!("{evt:?}");
    }
    Ok(())
}
```

## Discovery (USB + sim)

`touchy_pad::discover()` enumerates every locally reachable device in one
list — USB devices matching the Touchy vendor/product id **and** a
simulator entry when `TOUCHY_SIM_URL` is set — so sim devices show up
alongside hardware:

```rust,no_run
use touchy_pad::{discover, DiscoveredDevice, Touchy};

# async fn run() -> anyhow::Result<()> {
for dev in discover().await? {
    println!("found {}", dev.describe()); // "usb:001:007" or "sim:127.0.0.1:8765"
    let transport = dev.open().await?;    // USB bulk or TCP, same trait
    let pad = Touchy::from_transport(transport);
    let info = pad.client().sys_board_info_get().await?;
    println!("  serial = {}", info.serial); // stable id, Stage 71
}
# Ok(()) }
```

`DiscoveredDevice` deliberately carries only enough to *open* the
transport; the canonical stable id is the device's reported `serial`
(read after opening), not a bus/address tuple.

## Running actions device-side

`client().run_actions(actions)` runs a `Vec<Action>` on the device as if a
local widget had fired them. `Touchy::show_user_screen(name)` is the
convenience wrapper that brings an uploaded `F:host/uscr/<name>.pb` body
to the front of the default chrome:

```rust,no_run
# use touchy_pad::Touchy;
# async fn run(pad: &Touchy, page: &touchy_pad::proto::Widget) -> anyhow::Result<()> {
pad.user_screen_save("opendeck", page).await?;
pad.show_user_screen("opendeck").await?; // RunActionsCmd → ChangeWidgetRef(page)
# Ok(()) }
```

## Live widget property overrides

`pad.set_properties(vec![…])` applies a batch of runtime LVGL property
overrides in one round-trip (stage lb12/lb14). Overrides are session-scoped
and **sticky**: the device applies them at once if the widget is on screen and
re-applies them whenever it is next built, so targeting a not-yet-loaded widget
is fine. An entry with `value: None` removes the override.

The one helper with typed sugar is the font size (stage lb26): a label's font is
a *style* property whose value is a font pointer, so the wire carries a **pixel
size** instead and the device maps it (`text_font` →
`SetPropertyCmd.font_size_value`):

```rust,no_run
# use touchy_pad::Touchy;
# async fn run(pad: &Touchy) -> anyhow::Result<()> {
use touchy_pad::{FONT_SIZES, FontSize};

FONT_SIZES;                                        // [8, 14, 16, 30]
pad.set_font_size("title", FontSize::FontSize30).await?;
pad.set_font_size("title", FontSize::Default).await?;  // back to the theme
# Ok(()) }
```

Only those sizes exist — the firmware exact-matches the fonts its build
compiled, and anything else would silently render at the theme default
(Montserrat 14), so `set_font_size` with a size outside the list is answered
with `INVALID_ARG`. `FontSize::Default` (`0`) drops the local font so the widget
inherits its style stack again.

Anything else (text, colour, geometry) is built by hand and sent through the
same batch:

```rust,no_run
# use touchy_pad::Touchy;
# async fn run(pad: &Touchy) -> anyhow::Result<()> {
use touchy_pad::proto::{SetPropertyCmd, set_property_cmd};

pad.set_properties(vec![SetPropertyCmd {
    widget_id: "title".into(),
    property: Some(set_property_cmd::Property::PropertyName("text".into())),
    value: Some(set_property_cmd::Value::StringValue("New title".into())),
}]).await?;
# Ok(()) }
```

## Image conversion

PNG/JPG/BMP/GIF/WebP are auto-converted to LVGL RGB565 (or RGB565A8 when
the source has non-opaque alpha) by `to_lvgl_bin`. `file_save` checks
the path extension and the destination: anything written to `R:` or `F:`
with an image extension is converted on the host side, mirroring the
Python `touchy_pad.api.images` behaviour.

The shared normaliser `images::normalize_for_device(data, needs_conversion)`
returns the exact bytes `file_save` would upload plus the on-device file
suffix (`.bin` for a converted image or an unrecognised blob, otherwise
the detected format). The image cache reuses it so cached assets are
byte-for-byte identical to a direct `file_save`.

## Image cache

Sending image bytes over USB / UART is slow, and StreamDeck-style key
grids repaint the same handful of icons constantly. `ImageCache` uploads
each **distinct** image to the device exactly once, keyed by a 128-bit
content hash (xxh3), and returns the on-device path so a widget can
point at it cheaply:

```rust,no_run
use std::sync::Arc;
use touchy_pad::{ImageCache, Touchy};

# async fn demo() -> anyhow::Result<()> {
let pad = Arc::new(Touchy::open().await?);
let cache = ImageCache::new(pad.clone());

let png = std::fs::read("icon.png")?;
// First call uploads the (normalised) bytes and returns its path…
let path = cache.set_cached_image(&png).await?;
// …a second identical call uploads nothing and returns the same path.
let again = cache.set_cached_image(&png).await?;
assert_eq!(path, again);
# Ok(())
# }
```

Properties:

* **Host-side and volatile.** The map is never serialized; assets live
  on the `R:` PSRAM ramdisk (`image_cache::IMAGE_CACHE_ROOT`,
  `R:host/icache/`), wiped on device reboot. Build a fresh `ImageCache`
  per (re)attach — the first `set_cached_image` clears the cache root on
  the device so a crashed prior session leaves no stale files.
* **LRU eviction.** Once `image_cache::MAX_IMAGE_CACHE` (128) distinct
  images are resident, the next miss deletes the least-recently-used
  asset before uploading.
* **Repaint by file overwrite.** The firmware auto-reloads the active
  screen whenever a file a visible `WidgetRef` (or `Image`/`ImageButton`
  asset) targets is rewritten. So to swap a key's icon you keep the
  cell's `WidgetRef` path constant and just rewrite the small stub it
  points at to reference the new cached `.bin` — no `run_actions` round
  trip. The OpenDeck plugin (`rust/touchy-opendeck/`) uses exactly this
  model: each grid cell is a `WidgetRef` to a per-key `ImageButton` stub
  under `R:host/opendeck/`, and `set_image` repoints the stub at a
  cached asset.

## Errors

`TouchyError` is the single error type. Notable variants:

* `DeviceNotFound { vid, pid }` — no matching USB device.
* `Usb(String)` — surface-level USB or descriptor failure.
* `Framing(String)` — protocol framing violation.
* `Device { code, name }` — non-OK `ResultCode` returned by the device.
* `Timeout(Duration)` — `recv_response` deadline elapsed.

## Justfile

```text
just rust-build   # cargo build --workspace
just rust-test    # cargo test --workspace
just rust-lint    # cargo fmt --check + cargo clippy -- -D warnings
just rust-doc     # cargo doc --workspace --no-deps
just rust-run -- <demo|info|listen>
```

## Out of scope

* Simulator (use the Python `--sim` and connect over the planned TCP
  bridge in stage 80).
* `no_std`: this crate intentionally depends on `tokio` and `image`.
* C FFI shim: not planned. Consumers should call the async API directly.
