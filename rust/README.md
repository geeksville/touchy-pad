# touchy-pad — Rust client

Async, pure-Rust client library and demo binary for the
[Touchy-Pad](../README.md) USB touch device.

* **`touchy-pad`** — library crate. Async API built on `tokio` + `nusb`
  (no libusb). Talks the same length-prefixed protobuf protocol as the
  Python `touchy_pad` package.
* **`touchy-demo`** — command-line demo that uploads a 3-button screen
  to a connected device, then prints touch events.

## Quickstart

```bash
just rust-build          # cargo build --workspace
just rust-test           # cargo test --workspace
just rust-run -- info    # cargo run -p touchy-demo -- info
just rust-run -- demo    # upload demo screen
just rust-run -- listen  # print events forever
```

## Library example

```rust,no_run
use touchy_pad::Touchy;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let pad = Touchy::open().await?;
    let info = pad.client().sys_board_info_get().await?;
    println!("connected to {info:?}");
    Ok(())
}
```

See `docs/rust-api.md` for the full public-API tour and `touchy-demo/src/main.rs`
for a worked example that uploads images and listens for touch events.

## Conventions

* Edition 2024, hard tabs, `max_width = 200` (see `rustfmt.toml`).
* Library errors use `thiserror`; the demo binary uses `anyhow`.
* `cargo fmt --check` and `cargo clippy -- -D warnings` are enforced by
  `just rust-lint`.
