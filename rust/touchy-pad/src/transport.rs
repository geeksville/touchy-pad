//! Async bidirectional message transport.
//!
//! The wire format is a 4-byte little-endian length prefix followed by a
//! serialised protobuf [`crate::proto::Command`] (host → device) or
//! [`crate::proto::Response`] (device → host). See `docs/host-api.md`.

use std::time::Duration;

use async_trait::async_trait;

use crate::error::{Result, TouchyError};

/// Hard cap on accepted frame size — anything larger is treated as a
/// framing error so corrupt length fields can't make us allocate huge
/// buffers.
pub const MAX_FRAME: usize = 1 << 20; // 1 MiB

const LEN_PREFIX: usize = 4;

/// Wrap `payload` in a length-prefixed frame.
pub fn pack(payload: &[u8]) -> Result<Vec<u8>> {
	if payload.len() > MAX_FRAME {
		return Err(TouchyError::Framing(format!("payload exceeds {MAX_FRAME}-byte cap: {} bytes", payload.len())));
	}
	let mut out = Vec::with_capacity(LEN_PREFIX + payload.len());
	out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
	out.extend_from_slice(payload);
	Ok(out)
}

/// Parse a length-prefixed frame's payload from `buf`. Returns the
/// payload slice and the total number of bytes consumed.
pub fn unpack(buf: &[u8]) -> Result<(&[u8], usize)> {
	if buf.len() < LEN_PREFIX {
		return Err(TouchyError::Framing(format!("short header: {} bytes", buf.len())));
	}
	let len = u32::from_le_bytes(buf[..LEN_PREFIX].try_into().unwrap()) as usize;
	if len > MAX_FRAME {
		return Err(TouchyError::Framing(format!("frame length {len} exceeds {MAX_FRAME}-byte cap")));
	}
	let end = LEN_PREFIX + len;
	if buf.len() < end {
		return Err(TouchyError::Framing(format!("truncated frame: header says {len} bytes, got {}", buf.len() - LEN_PREFIX)));
	}
	Ok((&buf[LEN_PREFIX..end], end))
}

/// Async transport: send a command frame, receive a response frame.
///
/// Implementations must be `Send + Sync` so the high-level
/// [`crate::Touchy`] can park a background event-poll task on them.
/// The default impl is [`crate::transport_usb::UsbTransport`]; tests
/// use the in-memory mock under `tests/`.
#[async_trait]
pub trait Transport: Send + Sync {
	/// Send a serialised [`crate::proto::Command`] (the bytes are the
	/// payload only; the transport adds the length prefix).
	async fn send_command(&self, payload: &[u8]) -> Result<()>;

	/// Read one [`crate::proto::Response`] frame. Returns just the
	/// payload (the length prefix is stripped).
	async fn recv_response(&self, timeout: Duration) -> Result<Vec<u8>>;

	/// Whether host-side image conversion to LVGL `.bin` is required
	/// before upload. Real device transports return `true`; future
	/// in-process simulator bridges may return `false`.
	fn needs_image_conversion(&self) -> bool {
		true
	}
}
