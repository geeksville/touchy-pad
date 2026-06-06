//! Unified device discovery (Stage 71).
//!
//! [`discover`] enumerates every Touchy-Pad the host can reach —
//! USB devices matching the touchy-pad VID/PID, UART-bridge devices
//! (Stage 83 — gated on the `serial` feature), *and* the
//! out-of-process Python simulator when `TOUCHY_SIM_URL` is set — and
//! returns an opaque [`DiscoveredDevice`] per candidate that callers
//! can [`describe`][DiscoveredDevice::describe] (for a stable,
//! transport-level dedup key) and [`open`][DiscoveredDevice::open]
//! into a [`Transport`].
//!
//! This lives in the API library so every front-end (the OpenDeck
//! plugin, demos, future GUIs) shares one enumeration path that
//! already understands the simulator.

use std::sync::Arc;

use crate::error::Result;
use crate::transport::Transport;
use crate::transport_net::{TcpTransport, sim_url_from_env};
use crate::transport_usb::{UsbTransport, enumerate, usb_vid_pid};

use nusb::DeviceInfo;

/// A reachable Touchy-Pad candidate, ready to be opened.
///
/// Returned by [`discover`]. Each variant carries just enough to
/// produce a stable [`describe`][Self::describe] key (used by hot-plug
/// loops to dedup across polls) and to [`open`][Self::open] a
/// [`Transport`].
pub enum DiscoveredDevice {
	/// A USB device matching the touchy-pad VID/PID.
	Usb(DeviceInfo),
	/// A UART-bridge Touchy (Stage 83 — typically a CYD board appearing
	/// as a CH340 serial port). Only produced when the `serial` feature
	/// is enabled.
	#[cfg(feature = "serial")]
	Uart {
		/// Device-node path (e.g. `/dev/ttyUSB0`, `COM3`).
		path: String,
	},
	/// The out-of-process Python simulator named by `TOUCHY_SIM_URL`.
	Sim {
		/// Simulator host.
		host: String,
		/// Simulator TCP port.
		port: u16,
	},
}

impl DiscoveredDevice {
	/// A stable, transport-level identifier for this candidate.
	///
	/// USB devices key off bus + address (stable for the lifetime of a
	/// physical connection); UART-bridge devices key off the device-node
	/// path; the simulator keys off `host:port`. This is **not** the
	/// device serial — callers that want the hardware serial must
	/// [`open`][Self::open] and query `sys_board_info_get`.
	pub fn describe(&self) -> String {
		match self {
			DiscoveredDevice::Usb(info) => {
				format!("usb:{}:{:03}", info.bus_id(), info.device_address())
			}
			#[cfg(feature = "serial")]
			DiscoveredDevice::Uart { path } => format!("uart:{path}"),
			DiscoveredDevice::Sim { host, port } => format!("sim:{host}:{port}"),
		}
	}

	/// Open a [`Transport`] for this candidate.
	pub async fn open(&self) -> Result<Arc<dyn Transport>> {
		match self {
			DiscoveredDevice::Usb(info) => Ok(Arc::new(UsbTransport::open_info(info).await?)),
			#[cfg(feature = "serial")]
			DiscoveredDevice::Uart { path } => Ok(Arc::new(crate::transport_serial::SerialTransport::open(path)?)),
			DiscoveredDevice::Sim { host, port } => Ok(Arc::new(TcpTransport::connect(host, *port).await?)),
		}
	}
}

/// Enumerate every reachable Touchy-Pad.
///
/// Order: native-USB → UART-bridge (Stage 83, `serial` feature) →
/// simulator (`TOUCHY_SIM_URL`). Hardware takes precedence over the
/// simulator when a caller just wants "the first device". A failed USB
/// enumeration is treated as "no USB devices" (logged) rather than an
/// error, so the simulator remains discoverable on hosts without libusb.
pub async fn discover() -> Result<Vec<DiscoveredDevice>> {
	let mut out: Vec<DiscoveredDevice> = Vec::new();

	let (vid, pid) = usb_vid_pid();
	match enumerate(vid, pid).await {
		Ok(infos) => out.extend(infos.into_iter().map(DiscoveredDevice::Usb)),
		Err(e) => log::debug!("discover: USB enumeration failed (continuing): {e:#}"),
	}

	#[cfg(feature = "serial")]
	{
		for path in crate::transport_serial::discover_serial_ports() {
			out.push(DiscoveredDevice::Uart { path });
		}
	}

	if let Some((host, port)) = sim_url_from_env() {
		out.push(DiscoveredDevice::Sim { host, port });
	}

	Ok(out)
}

#[cfg(test)]
mod tests {
	use super::*;

	#[test]
	fn sim_describe_is_stable() {
		let d = DiscoveredDevice::Sim { host: "127.0.0.1".into(), port: 8935 };
		assert_eq!(d.describe(), "sim:127.0.0.1:8935");
	}

	#[cfg(feature = "serial")]
	#[test]
	fn uart_describe_is_stable() {
		let d = DiscoveredDevice::Uart { path: "/dev/ttyUSB0".into() };
		assert_eq!(d.describe(), "uart:/dev/ttyUSB0");
	}

	#[tokio::test]
	async fn discover_includes_sim_from_env() {
		// SAFETY: single-threaded test; we set and clear the var.
		unsafe { std::env::set_var("TOUCHY_SIM_URL", "tcp://127.0.0.1:8935") };
		let devices = discover().await.unwrap();
		unsafe { std::env::remove_var("TOUCHY_SIM_URL") };
		assert!(devices.iter().any(|d| matches!(d, DiscoveredDevice::Sim { .. })));
	}
}
