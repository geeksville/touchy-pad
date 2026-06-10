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
use tokio_serial::{SerialPortBuilderExt, SerialPortType, SerialStream};

use crate::error::{Result, TouchyError};
use crate::transport::{FrameDecoder, Transport, pack};

/// The protocol always runs at this fixed baud rate.
pub const BAUD_RATE: u32 = 460_800;

/// Stage 83 — Touchy-Pad variants without native USB (the classic-ESP32
/// CYD family) appear on the host as USB-to-UART bridges. Discovery
/// treats any serial port whose USB descriptor matches one of these
/// `(vid, pid)` pairs as a Touchy candidate. New bridges are added with
/// one line.
pub const UART_BRIDGE_VID_PIDS: &[(u16, u16)] = &[
	(0x1A86, 0x7523), // QinHeng CH340 (CYD2USB classic-ESP32 boards)
];

#[cfg(target_os = "linux")]
const HOST_DEV_DIR: &str = "/host/dev";

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

/// Returns true iff `path` exists and is read+write accessible.
///
/// Used to drop device nodes the user has no permission to open (no
/// `dialout` / `uucp` group membership). Trying to open them would just
/// fail at [`SerialTransport::open`] time anyway, so silently skip them
/// during discovery.
fn is_rw_accessible(path: &str) -> bool {
	std::fs::OpenOptions::new().read(true).write(true).open(path).is_ok()
}

/// Stage 83 — return device paths of UART-bridge Touchys on the host.
///
/// Walks [`tokio_serial::available_ports`] and keeps every port whose
/// `(vid, pid)` is in [`UART_BRIDGE_VID_PIDS`]. Inaccessible nodes
/// (no `dialout` / `uucp` group membership) are dropped silently.
///
/// **Devcontainer fallback** (Linux only): if `available_ports()`
/// returns nothing *and* `/host/dev` exists, additionally glob
/// `/host/dev/ttyUSB*` and `/host/dev/ttyACM*`. Sysfs isn't bind-mounted
/// into the container so the VID/PID filter can't run there; the
/// fallback is a path-shape check, consistent with the spec's
/// "trust the device if accessible" rule.
pub fn discover_serial_ports() -> Vec<String> {
	let mut paths: Vec<String> = Vec::new();

	match tokio_serial::available_ports() {
		Ok(ports) => {
			for p in ports {
				if let SerialPortType::UsbPort(info) = &p.port_type {
					if UART_BRIDGE_VID_PIDS.iter().any(|(v, pid)| *v == info.vid && *pid == info.pid) && is_rw_accessible(&p.port_name) {
						paths.push(p.port_name);
					}
				}
			}
		}
		Err(e) => log::debug!("discover_serial_ports: available_ports failed: {e}"),
	}

	#[cfg(target_os = "linux")]
	if paths.is_empty() {
		if let Ok(rd) = std::fs::read_dir(HOST_DEV_DIR) {
			let mut hits: Vec<String> = rd
				.filter_map(|e| e.ok())
				.filter_map(|e| {
					let name = e.file_name();
					let s = name.to_string_lossy();
					(s.starts_with("ttyUSB") || s.starts_with("ttyACM")).then(|| format!("{HOST_DEV_DIR}/{s}"))
				})
				.filter(|p| is_rw_accessible(p))
				.collect();
			hits.sort();
			paths.extend(hits);
		}
	}

	paths
}

#[cfg(test)]
mod tests {
	use super::*;

	#[test]
	fn known_bridge_table_includes_ch340() {
		assert!(UART_BRIDGE_VID_PIDS.contains(&(0x1A86, 0x7523)));
	}

	#[test]
	fn discover_runs_without_panicking() {
		// Just exercise the code path; the result depends on what's
		// plugged in to the test host (typically nothing in CI).
		let _ = discover_serial_ports();
	}
}
