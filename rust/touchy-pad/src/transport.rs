//! Async bidirectional message transport.
//!
//! The wire format is a self-synchronising frame carrying one serialised
//! protobuf [`crate::proto::Command`] (host → device) or
//! [`crate::proto::Response`] (device → host):
//!
//! ```text
//!     MAGIC(2) | LEN(u16 little-endian) | payload | CRC8(1)
//! ```
//!
//! `MAGIC` is the byte pair `0xA5 0x5A` and lets a reader re-acquire frame
//! alignment after corruption or a mid-stream connect; `CRC8` (poly 0x07,
//! init 0x00) over `LEN || payload` detects corruption. Identical across
//! every transport (USB bulk, TCP simulator, serial). See `docs/host-api.md`
//! and Stage 64.3 in `docs/design.md`.

use std::time::Duration;

use async_trait::async_trait;

use crate::error::{Result, TouchyError};

/// Frame sync marker. Mirrors `touchy_pad.transport._MAGIC`.
pub const MAGIC: [u8; 2] = [0xA5, 0x5A];

/// Hard cap on accepted frame size. The on-wire LEN field is a `u16`, so
/// anything larger than this is unrepresentable and treated as a framing
/// error.
pub const MAX_FRAME: usize = 0xFFFF;

/// Bytes of fixed header before the payload: MAGIC(2) + LEN(2).
const HDR: usize = MAGIC.len() + 2;

/// CRC-8 (polynomial 0x07, init 0x00). Mirrors
/// `touchy_pad.transport._crc8` and the firmware `crc8_update`.
pub fn crc8(data: &[u8]) -> u8 {
	let mut crc: u8 = 0x00;
	for &b in data {
		crc ^= b;
		for _ in 0..8 {
			crc = if crc & 0x80 != 0 { (crc << 1) ^ 0x07 } else { crc << 1 };
		}
	}
	crc
}

/// Wrap `payload` in a self-synchronising frame.
pub fn pack(payload: &[u8]) -> Result<Vec<u8>> {
	if payload.len() > MAX_FRAME {
		return Err(TouchyError::Framing(format!("payload exceeds {MAX_FRAME}-byte cap: {} bytes", payload.len())));
	}
	let mut body = Vec::with_capacity(2 + payload.len());
	body.extend_from_slice(&(payload.len() as u16).to_le_bytes());
	body.extend_from_slice(payload);
	let crc = crc8(&body);
	let mut out = Vec::with_capacity(MAGIC.len() + body.len() + 1);
	out.extend_from_slice(&MAGIC);
	out.extend_from_slice(&body);
	out.push(crc);
	Ok(out)
}

/// Parse one complete frame's payload from `buf`, which must begin at a
/// MAGIC marker. Returns the payload and the total bytes consumed. Used
/// by tests; streaming callers use [`FrameDecoder`].
pub fn unpack(buf: &[u8]) -> Result<(Vec<u8>, usize)> {
	if buf.len() < HDR {
		return Err(TouchyError::Framing(format!("short header: {} bytes", buf.len())));
	}
	if buf[..MAGIC.len()] != MAGIC {
		return Err(TouchyError::Framing("missing frame magic".into()));
	}
	let len = u16::from_le_bytes([buf[2], buf[3]]) as usize;
	let total = HDR + len + 1;
	if buf.len() < total {
		return Err(TouchyError::Framing(format!("truncated frame: need {total} bytes, got {}", buf.len())));
	}
	let crc = buf[HDR + len];
	let calc = crc8(&buf[MAGIC.len()..HDR + len]);
	if crc != calc {
		return Err(TouchyError::Framing(format!("CRC mismatch: got 0x{crc:02x}, want 0x{calc:02x}")));
	}
	Ok((buf[HDR..HDR + len].to_vec(), total))
}

/// Stateful resync decoder for byte-stream transports.
///
/// Feed it arbitrary chunks; it yields complete, CRC-validated payloads
/// and silently skips leading garbage / corrupt frames. Mirrors
/// `touchy_pad.transport._FrameDecoder`.
#[derive(Default)]
pub struct FrameDecoder {
	buf: Vec<u8>,
}

impl FrameDecoder {
	/// Create an empty decoder.
	pub fn new() -> Self {
		Self { buf: Vec::new() }
	}

	/// Append received bytes.
	pub fn feed(&mut self, data: &[u8]) {
		self.buf.extend_from_slice(data);
	}

	/// Return the next complete payload, or `None` if more bytes are
	/// needed. Skips leading garbage and CRC-failed frames.
	pub fn next_frame(&mut self) -> Option<Vec<u8>> {
		loop {
			let start = self.buf.windows(MAGIC.len()).position(|w| w == MAGIC);
			match start {
				None => {
					// No magic yet. Keep only the last byte in case a
					// partial magic straddles the next chunk boundary.
					if self.buf.len() > MAGIC.len() - 1 {
						let drop = self.buf.len() - (MAGIC.len() - 1);
						self.buf.drain(0..drop);
					}
					return None;
				}
				Some(idx) => {
					if idx > 0 {
						self.buf.drain(0..idx);
					}
				}
			}
			if self.buf.len() < HDR {
				return None;
			}
			let len = u16::from_le_bytes([self.buf[2], self.buf[3]]) as usize;
			let total = HDR + len + 1;
			if self.buf.len() < total {
				return None;
			}
			let crc = self.buf[HDR + len];
			let calc = crc8(&self.buf[MAGIC.len()..HDR + len]);
			if crc == calc {
				let payload = self.buf[HDR..HDR + len].to_vec();
				self.buf.drain(0..total);
				return Some(payload);
			}
			// CRC mismatch: drop one byte past this magic and rescan so a
			// real frame starting just after a false magic isn't missed.
			self.buf.drain(0..1);
		}
	}
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
