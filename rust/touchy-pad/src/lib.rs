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
pub mod error;
pub mod images;
pub mod pad;
pub mod proto;
pub mod transport;
pub mod transport_usb;

pub use error::{Result, TouchyError};
pub use pad::Touchy;
