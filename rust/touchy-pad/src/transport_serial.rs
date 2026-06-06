//! Serial-port transport (USB-CDC ACM ports, and real UARTs on boards
//! without native USB).
//!
//! Gated behind the `serial` cargo feature because it pulls in
//! `tokio-serial` and its platform serial-port dependencies. Speaks the
//! same self-synchronising framing as every other transport — see
//! [`crate::transport`]. The protocol always runs at 115200 baud.

use std::time::Duration;

use async_trait::async_trait;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::sync::Mutex;
use tokio_serial::{SerialPortBuilderExt, SerialStream};

use crate::error::{Result, TouchyError};
use crate::transport::{FrameDecoder, Transport, pack};

/// The protocol always runs at this fixed baud rate.
pub const BAUD_RATE: u32 = 460_800;

/// Serial-port-backed [`Transport`].
pub struct SerialTransport {
	inner: Mutex<SerialStream>,
	port: String,
}

impl SerialTransport {
	/// Open `port` (e.g. `/dev/ttyACM0` or `COM3`) at [`BAUD_RATE`].
	pub fn open(port: &str) -> Result<Self> {
		let stream = tokio_serial::new(port, BAUD_RATE)
			.open_native_async()
			.map_err(|e| TouchyError::Transport(format!("serial open {port}: {e}")))?;
		log::info!("serial: transport connected to {port} @ {BAUD_RATE} baud");
		Ok(Self {
			inner: Mutex::new(stream),
			port: port.to_string(),
		})
	}
}

#[async_trait]
impl Transport for SerialTransport {
	async fn send_command(&self, payload: &[u8]) -> Result<()> {
		let frame = pack(payload)?;
		let mut g = self.inner.lock().await;
		g.write_all(&frame).await.map_err(|e| TouchyError::Transport(format!("serial send to {}: {e}", self.port)))?;
		g.flush().await.map_err(|e| TouchyError::Transport(format!("serial flush {}: {e}", self.port)))?;
		Ok(())
	}

	async fn recv_response(&self, timeout: Duration) -> Result<Vec<u8>> {
		let mut g = self.inner.lock().await;
		let port = self.port.clone();
		let fut = async {
			let mut decoder = FrameDecoder::new();
			let mut chunk = [0u8; 4096];
			loop {
				if let Some(payload) = decoder.next_frame() {
					return Ok::<Vec<u8>, TouchyError>(payload);
				}
				let n = g.read(&mut chunk).await.map_err(|e| TouchyError::Transport(format!("serial recv {port}: {e}")))?;
				if n == 0 {
					return Err(TouchyError::Transport(format!("serial {port}: port closed")));
				}
				decoder.feed(&chunk[..n]);
			}
		};
		match tokio::time::timeout(timeout, fut).await {
			Ok(r) => r,
			Err(_) => Err(TouchyError::Timeout(timeout)),
		}
	}
}
