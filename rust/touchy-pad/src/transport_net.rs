//! TCP transport for the out-of-process Python simulator.
//!
//! The simulator (`touchy simulator`) speaks the *same* length-prefixed
//! nanopb framing on TCP as the firmware does over USB bulk endpoints,
//! so this transport is the network counterpart of
//! [`crate::transport_usb::UsbTransport`].
//!
//! See `docs/simulator.md` and Stage 63 in `docs/design.md`.

use std::time::Duration;

use async_trait::async_trait;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::sync::Mutex;

use crate::error::{Result, TouchyError};
use crate::transport::{MAX_FRAME, Transport, pack};

/// Default TCP port the out-of-process simulator listens on. Mirrors
/// `touchy_pad.transport_net.DEFAULT_SIM_PORT`.
pub const DEFAULT_SIM_PORT: u16 = 8935;

/// Environment variable consulted by [`crate::Touchy::open`] before
/// USB enumeration. Same name as on the Python side.
pub const SIM_URL_ENV: &str = "TOUCHY_SIM_URL";

/// TCP-backed [`Transport`] talking to a `touchy simulator` process.
pub struct TcpTransport {
	inner: Mutex<TcpStream>,
	addr: String,
}

impl TcpTransport {
	/// Connect to `host:port`.
	pub async fn connect(host: &str, port: u16) -> Result<Self> {
		let addr = format!("{host}:{port}");
		let stream = TcpStream::connect(&addr).await.map_err(|e| TouchyError::Transport(format!("sim TCP connect {addr}: {e}")))?;
		stream.set_nodelay(true).ok();
		tracing_log(&format!("sim: TCP transport connected to {addr}"));
		Ok(Self { inner: Mutex::new(stream), addr })
	}

	/// Connect using a `host[:port]` / `tcp://host[:port]` URL string.
	pub async fn connect_url(url: &str) -> Result<Self> {
		let (host, port) = parse_sim_url(url)?;
		Self::connect(&host, port).await
	}
}

#[async_trait]
impl Transport for TcpTransport {
	async fn send_command(&self, payload: &[u8]) -> Result<()> {
		let frame = pack(payload)?;
		let mut g = self.inner.lock().await;
		g.write_all(&frame).await.map_err(|e| TouchyError::Transport(format!("sim TCP send to {}: {e}", self.addr)))?;
		Ok(())
	}

	async fn recv_response(&self, timeout: Duration) -> Result<Vec<u8>> {
		let mut g = self.inner.lock().await;
		let fut = async {
			let mut hdr = [0u8; 4];
			g.read_exact(&mut hdr).await.map_err(|e| TouchyError::Transport(format!("sim TCP recv header: {e}")))?;
			let len = u32::from_le_bytes(hdr) as usize;
			if len > MAX_FRAME {
				return Err(TouchyError::Framing(format!("sim TCP frame exceeds {MAX_FRAME}-byte cap: {len} bytes")));
			}
			let mut buf = vec![0u8; len];
			g.read_exact(&mut buf).await.map_err(|e| TouchyError::Transport(format!("sim TCP recv body: {e}")))?;
			Ok::<Vec<u8>, TouchyError>(buf)
		};
		match tokio::time::timeout(timeout, fut).await {
			Ok(r) => r,
			Err(_) => Err(TouchyError::Timeout(timeout)),
		}
	}

	fn needs_image_conversion(&self) -> bool {
		// The TCP sim consumes raw LVGL `.bin` exactly like the
		// firmware does, so the host pipeline must convert
		// PNG/JPG/etc. before upload.
		true
	}
}

/// Parse a `host[:port]` / `tcp://host[:port]` string.
///
/// Mirrors `touchy_pad.transport_net.parse_sim_url`. Supports IPv6
/// literals in square brackets (e.g. `[::1]:8935`). Returns
/// [`DEFAULT_SIM_PORT`] when no port is given.
pub fn parse_sim_url(value: &str) -> Result<(String, u16)> {
	let s = value.trim();
	let s = s.strip_prefix("tcp://").unwrap_or(s);
	let s = s.split('/').next().unwrap_or(s);

	let (host, port) = if let Some(rest) = s.strip_prefix('[') {
		// IPv6 literal
		let end = rest.find(']').ok_or_else(|| TouchyError::Other(format!("unterminated IPv6 literal in sim URL: {value:?}")))?;
		let host = &rest[..end];
		let after = &rest[end + 1..];
		let port = if let Some(p) = after.strip_prefix(':') {
			p.parse::<u16>().map_err(|e| TouchyError::Other(format!("bad port in sim URL {value:?}: {e}")))?
		} else {
			DEFAULT_SIM_PORT
		};
		(host.to_string(), port)
	} else if let Some(idx) = s.rfind(':') {
		let (h, p) = s.split_at(idx);
		let p = &p[1..];
		(h.to_string(), p.parse::<u16>().map_err(|e| TouchyError::Other(format!("bad port in sim URL {value:?}: {e}")))?)
	} else {
		(s.to_string(), DEFAULT_SIM_PORT)
	};

	if host.is_empty() {
		return Err(TouchyError::Other(format!("empty host in sim URL: {value:?}")));
	}
	Ok((host, port))
}

/// Return `Some((host, port))` from the [`SIM_URL_ENV`] env var, or
/// `None` if it is unset / malformed.
pub fn sim_url_from_env() -> Option<(String, u16)> {
	let raw = std::env::var(SIM_URL_ENV).ok()?;
	if raw.is_empty() {
		return None;
	}
	match parse_sim_url(&raw) {
		Ok(v) => Some(v),
		Err(e) => {
			tracing_log(&format!("ignoring malformed {SIM_URL_ENV}={raw:?}: {e}"));
			None
		}
	}
}

fn tracing_log(msg: &str) {
	// Library-side logging; callers wire in their own subscriber.
	log::info!("{msg}");
}

#[cfg(test)]
mod tests {
	use super::*;

	#[test]
	fn parse_host_port() {
		assert_eq!(parse_sim_url("127.0.0.1:8935").unwrap(), ("127.0.0.1".to_string(), 8935));
	}

	#[test]
	fn parse_host_only_default_port() {
		assert_eq!(parse_sim_url("example.test").unwrap(), ("example.test".to_string(), DEFAULT_SIM_PORT));
	}

	#[test]
	fn parse_tcp_prefix() {
		assert_eq!(parse_sim_url("tcp://10.0.0.1:1234").unwrap(), ("10.0.0.1".to_string(), 1234));
	}

	#[test]
	fn parse_ipv6() {
		assert_eq!(parse_sim_url("[::1]:9000").unwrap(), ("::1".to_string(), 9000));
		assert_eq!(parse_sim_url("[::1]").unwrap(), ("::1".to_string(), DEFAULT_SIM_PORT));
	}

	#[test]
	fn parse_rejects_empty_host() {
		assert!(parse_sim_url("").is_err());
		assert!(parse_sim_url(":8935").is_err());
	}
}
