//! USB-backed [`Transport`] using the pure-Rust [`nusb`] crate.
//!
//! The Touchy-Pad device exposes a composite USB descriptor; the host
//! protocol lives on a dedicated **vendor-specific** interface
//! (`bInterfaceClass == 0xFF`) with one bulk OUT and one bulk IN
//! endpoint. We locate it by class, not by interface number, so the
//! firmware can rearrange the composite descriptor without breaking
//! us.
//!
//! VID/PID come from the `Constants` enum in `proto/touchy.proto`,
//! read out of the generated [`crate::proto::Constants`] at runtime
//! so this code stays in sync with the firmware automatically.

use std::time::Duration;

use async_trait::async_trait;
use nusb::DeviceInfo;
use nusb::descriptors::TransferType;
use nusb::transfer::{Buffer, Bulk, Direction, In, Out};
use tokio::sync::Mutex;

use crate::error::{Result, TouchyError};
use crate::proto::Constants;
use crate::transport::{FrameDecoder, Transport, pack};

const VENDOR_INTERFACE_CLASS: u8 = 0xFF;

/// USB IDs for the Touchy-Pad device, read from the generated proto.
pub fn usb_vid_pid() -> (u16, u16) {
	(Constants::UsbVid as u16, Constants::UsbPid as u16)
}

/// Bulk USB transport for a connected Touchy-Pad.
pub struct UsbTransport {
	inner: Mutex<Inner>,
}

struct Inner {
	ep_out: nusb::Endpoint<Bulk, Out>,
	ep_in: nusb::Endpoint<Bulk, In>,
	max_packet_size_in: usize,
}

impl UsbTransport {
	/// Find and open the first connected Touchy-Pad device.
	pub async fn open() -> Result<Self> {
		let (vid, pid) = usb_vid_pid();
		let info = enumerate_first(vid, pid).await?;
		Self::open_info(&info).await
	}

	/// Open a specific device identified by an already-enumerated
	/// [`nusb::DeviceInfo`].
	pub async fn open_info(info: &DeviceInfo) -> Result<Self> {
		let device = match info.open().await {
			Ok(d) => d,
			Err(e) => {
				// Devcontainer fallback: `/sys` is bind-mounted but the
				// matching `/dev/bus/usb/BBB/DDD` node may live under
				// `/host/dev/bus/usb` instead. Mirror what the Python
				// `_install_host_dev_fallback` does and open the node by
				// hand, then hand the fd to nusb via `Device::from_fd`.
				// Linux-only: `busnum()` / `Device::from_fd` aren't
				// available on macOS (also unix), and the fallback path
				// itself is a Linux container quirk.
				#[cfg(target_os = "linux")]
				{
					match try_open_via_host_dev(info).await {
						Some(Ok(d)) => d,
						Some(Err(fe)) => return Err(fe),
						None => return Err(usb_open_err(info, e)),
					}
				}
				#[cfg(not(target_os = "linux"))]
				{
					return Err(usb_open_err(info, e));
				}
			}
		};
		let cfg = device.active_configuration().map_err(|e| TouchyError::Usb(format!("active_configuration: {e}")))?;

		// Locate the vendor-specific interface and its two bulk endpoints.
		let mut iface_num = None;
		let mut ep_out_addr = None;
		let mut ep_in_addr = None;
		for iface in cfg.interfaces() {
			for alt in iface.alt_settings() {
				if alt.class() != VENDOR_INTERFACE_CLASS {
					continue;
				}
				iface_num = Some(iface.interface_number());
				for ep in alt.endpoints() {
					if ep.transfer_type() != TransferType::Bulk {
						continue;
					}
					match ep.direction() {
						Direction::Out => ep_out_addr = Some(ep.address()),
						Direction::In => ep_in_addr = Some(ep.address()),
					}
				}
			}
		}
		let iface_num = iface_num.ok_or_else(|| TouchyError::Usb("device has no vendor-specific (0xFF) interface".into()))?;
		let ep_out_addr = ep_out_addr.ok_or_else(|| TouchyError::Usb("vendor interface missing bulk OUT endpoint".into()))?;
		let ep_in_addr = ep_in_addr.ok_or_else(|| TouchyError::Usb("vendor interface missing bulk IN endpoint".into()))?;

		let interface = device.claim_interface(iface_num).await.map_err(|e| TouchyError::Usb(format!("claim_interface({iface_num}): {e}")))?;

		let ep_out = interface
			.endpoint::<Bulk, Out>(ep_out_addr)
			.map_err(|e| TouchyError::Usb(format!("endpoint OUT {ep_out_addr:#04x}: {e}")))?;
		let ep_in = interface
			.endpoint::<Bulk, In>(ep_in_addr)
			.map_err(|e| TouchyError::Usb(format!("endpoint IN {ep_in_addr:#04x}: {e}")))?;
		let max_packet_size_in = ep_in.max_packet_size();

		Ok(Self {
			inner: Mutex::new(Inner { ep_out, ep_in, max_packet_size_in }),
		})
	}
}

/// Enumerate every connected Touchy-Pad device matching `(vid, pid)`.
pub async fn enumerate(vid: u16, pid: u16) -> Result<Vec<DeviceInfo>> {
	let iter = nusb::list_devices().await.map_err(|e| TouchyError::Usb(format!("list_devices: {e}")))?;
	Ok(iter.filter(|d| d.vendor_id() == vid && d.product_id() == pid).collect())
}

async fn enumerate_first(vid: u16, pid: u16) -> Result<DeviceInfo> {
	enumerate(vid, pid).await?.into_iter().next().ok_or(TouchyError::DeviceNotFound { vid, pid })
}

#[async_trait]
impl Transport for UsbTransport {
	async fn send_command(&self, payload: &[u8]) -> Result<()> {
		let frame = pack(payload)?;
		let mut g = self.inner.lock().await;
		let buf = Buffer::from(frame);
		g.ep_out.submit(buf);
		let completion = g.ep_out.next_complete().await;
		completion.status.map_err(|e| TouchyError::Usb(format!("bulk OUT: {e:?}")))?;
		Ok(())
	}

	async fn recv_response(&self, timeout: Duration) -> Result<Vec<u8>> {
		let mut g = self.inner.lock().await;
		let mps = g.max_packet_size_in;
		// requested_len must be a nonzero multiple of max_packet_size;
		// 64 KiB rounded to that boundary is plenty for any Response
		// frame we emit.
		let req_len = ((64 * 1024) / mps).max(1) * mps;
		// Loop to absorb USB Zero-Length Packets (ZLPs) and reassemble
		// the self-synchronising frame. TinyUSB on the device appends a
		// ZLP whenever the response payload is an exact multiple of
		// `mps` (USB-spec requirement to mark end-of-transfer); nusb
		// surfaces that as an Ok completion with an empty buffer. We
		// feed every non-empty completion into a [`FrameDecoder`] which
		// handles resync and yields one complete, CRC-validated payload.
		let mut decoder = FrameDecoder::new();
		loop {
			let buf = Buffer::new(req_len);
			g.ep_in.submit(buf);
			let completion = match tokio::time::timeout(timeout, g.ep_in.next_complete()).await {
				Ok(c) => c,
				Err(_) => {
					g.ep_in.cancel_all();
					// Drain the cancelled transfer so the endpoint
					// isn't left with a pending completion.
					let _ = g.ep_in.next_complete().await;
					return Err(TouchyError::Timeout(timeout));
				}
			};
			completion.status.map_err(|e| TouchyError::Usb(format!("bulk IN: {e:?}")))?;
			let data: &[u8] = &completion.buffer;
			if data.is_empty() {
				// ZLP from a previous frame — try again.
				continue;
			}
			decoder.feed(data);
			if let Some(payload) = decoder.next_frame() {
				return Ok(payload);
			}
		}
	}
}

/// Devcontainer/sandbox fallback for `info.open()`.
///
/// Some sandboxed environments — notably the touchy-pad devcontainer —
/// bind-mount `/sys/bus/usb` from the host into the container (so
/// enumeration sees the device) but only expose live device nodes under
/// `/host/dev/bus/usb/...`, leaving `/dev/bus/usb/...` empty. nusb's
/// normal open path tries `/dev/bus/usb/BBB/DDD` and fails with ENOENT.
///
/// We mirror the Python `_install_host_dev_fallback` workaround in
/// `app/src/touchy_pad/transport.py`: open the matching node under
/// `/host/dev/bus/usb` ourselves and hand the fd to
/// [`nusb::Device::from_fd`].
///
/// Returns `None` if the fallback root doesn't exist (so the caller
/// surfaces the original error), `Some(Ok)` on success, or
/// `Some(Err)` if the fallback was attempted but failed.
#[cfg(target_os = "linux")]
async fn try_open_via_host_dev(info: &DeviceInfo) -> Option<Result<nusb::Device>> {
	use std::fs::OpenOptions;
	use std::os::fd::OwnedFd;
	use std::os::unix::fs::OpenOptionsExt;

	let host_root = "/host/dev/bus/usb";
	if !std::path::Path::new(host_root).is_dir() {
		return None;
	}

	let bus = info.busnum();
	let addr = info.device_address();
	let path = format!("{host_root}/{bus:03}/{addr:03}");

	let file = match OpenOptions::new().read(true).write(true).custom_flags(libc::O_CLOEXEC).open(&path) {
		Ok(f) => f,
		Err(e) if e.kind() == std::io::ErrorKind::PermissionDenied => {
			// Missing/bad udev rule — surface the friendly fix-up hint
			// (mirrors the Python `TransportPermissionError`).
			return Some(Err(TouchyError::UsbPermission { path }));
		}
		Err(e) => return Some(Err(TouchyError::Usb(format!("opening fallback node {path}: {e}")))),
	};
	let fd: OwnedFd = file.into();
	match nusb::Device::from_fd(fd).await {
		Ok(d) => Some(Ok(d)),
		Err(e) => Some(Err(TouchyError::Usb(format!("Device::from_fd({path}): {e}")))),
	}
}

/// Map a failed [`DeviceInfo::open`] error onto a [`TouchyError`].
///
/// A permission-denied failure (typically a missing/bad udev rule on
/// Linux) becomes [`TouchyError::UsbPermission`], which carries the
/// friendly fix-up hint that points at `docs/udev.md` — mirroring the
/// Python `TransportPermissionError`. Everything else is wrapped as a
/// generic [`TouchyError::Usb`] preserving nusb's message.
fn usb_open_err(info: &DeviceInfo, e: nusb::Error) -> TouchyError {
	if e.kind() == nusb::ErrorKind::PermissionDenied {
		let path = format!("VID:PID {:#06x}:{:#06x}", info.vendor_id(), info.product_id());
		TouchyError::UsbPermission { path }
	} else {
		TouchyError::Usb(format!("open: {e}"))
	}
}
