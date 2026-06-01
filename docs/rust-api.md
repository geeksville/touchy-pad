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

## Layout

| Module | Purpose |
|---|---|
| `touchy_pad::Touchy` | High-level handle; wraps `Client` + background event poller. |
| `touchy_pad::client::Client` | One-shot RPC methods (`sys_board_info_get`, `file_open_write`, `screen_load`, …). |
| `touchy_pad::transport::Transport` | Async trait — frame in, frame out. Mockable for tests. |
| `touchy_pad::transport_usb::UsbTransport` | Default `nusb`-backed bulk transport. |
| `touchy_pad::images` | LVGL `.bin` conversion (`to_lvgl_bin`, `LvFormat`). |
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

## Image conversion

PNG/JPG/BMP/GIF/WebP are auto-converted to LVGL RGB565 (or RGB565A8 when
the source has non-opaque alpha) by `to_lvgl_bin`. `file_save` checks
the path extension and the destination: anything written to `R:` or `F:`
with an image extension is converted on the host side, mirroring the
Python `touchy_pad.api.images` behaviour.

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
