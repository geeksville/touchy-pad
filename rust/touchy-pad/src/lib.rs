//! # touchy-pad
//!
//! Host-side Rust API for the [Touchy-Pad](https://github.com/geeksville/touchy-pad)
//! open-source USB multitouch pad / button-matrix device. This crate
//! is the Rust sister of the Python `touchy_pad` package shipped from
//! the same repository.
//!
//! ## Quick start
//!
//! ```no_run
//! use touchy_pad::{Touchy, Result};
//!
//! #[tokio::main]
//! async fn main() -> Result<()> {
//!     let pad = Touchy::open().await?;
//!     let info = pad.client().sys_board_info_get().await?;
//!     println!("connected to {}", info.board_name);
//!     Ok(())
//! }
//! ```
//!
//! ## Layout
//!
//! * [`proto`] — generated protobuf types (`Command`, `Response`,
//!   `Screen`, `Widget`, …). Build user screens here with plain
//!   prost structs; there is no DSL.
//! * [`transport`] — the [`transport::Transport`] trait and length-prefix
//!   framing helpers. Implement this for custom backends (e.g. a TCP
//!   bridge to the Python simulator).
//! * [`transport_usb::UsbTransport`] — pure-Rust USB transport on top
//!   of [`nusb`]. Filters by `Constants::UsbVid`/`UsbPid` from the
//!   generated proto.
//! * [`client::Client`] — low-level typed RPC wrapper.
//! * [`Touchy`] — high-level handle with a background event poller
//!   and helpers like [`Touchy::file_save`] / [`Touchy::screen_save`].
//! * [`images`] — PNG/JPEG/BMP/GIF/WebP → LVGL 9 native `.bin`
//!   conversion.

#![warn(missing_docs)]

pub mod client;
pub mod discover;
pub mod error;
pub mod image_cache;
pub mod images;
pub mod pad;
pub mod proto;
pub mod transport;
pub mod transport_net;
#[cfg(feature = "serial")]
pub mod transport_serial;
pub mod transport_usb;

pub use discover::{DiscoveredDevice, discover};
pub use error::{Result, TouchyError};
pub use image_cache::ImageCache;
pub use pad::Touchy;

/// On-device directory holding screen-layout blobs (Stage 68; moved from
/// the old `host/screens/`). Mirror of `touchy_pad.paths.SCREENS_DIR`.
pub const SCREENS_DIR: &str = "F:host/s/";

/// Canonical prev/next chrome screen the host's `screen init` writes; the
/// firmware prefers it as its boot screen when present.
pub const DEFAULT_SCREEN_PATH: &str = "F:host/s/default.pb";

/// Directory holding user page bodies that the default chrome's
/// `widget_ref(id="page")` pages through (Stage 68).
pub const USER_SCREENS_DIR: &str = "F:host/uscr/";

/// The logical `T:` ("temp") transient drive (Stage 87). The device
/// resolves it to a PSRAM ramdisk where available, else a flash scratch
/// area (see `SysBoardInfoResponse.temp_is_flash`). Host writers of
/// throwaway assets address it as `T:...` and never branch on the board.
/// Mirror of `touchy_pad.paths.TEMP_DRIVE`.
pub const TEMP_DRIVE: &str = "T:";
