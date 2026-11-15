//! Enumerate every reachable Touchy-Pad and describe it.
//!
//! Covers all three discovery paths in one list: native USB, UART-bridge boards
//! (only when the `serial` feature is on), and the out-of-process Python
//! simulator when `TOUCHY_SIM_URL` is set.
//!
//! ```sh
//! cargo run --example discover
//! TOUCHY_SIM_URL=tcp://127.0.0.1:8935 cargo run --example discover
//! ```

use touchy_pad::{Touchy, discover};

#[tokio::main]
async fn main() -> touchy_pad::Result<()> {
	env_logger::init();

	let devices = discover().await?;
	if devices.is_empty() {
		println!("no Touchy-Pad found — connect one, or start the simulator and set TOUCHY_SIM_URL");
		return Ok(());
	}

	for dev in devices {
		// `describe()` is a *transport-level* key (bus:address / node path /
		// host:port) — handy for dedup in a hot-plug loop. The hardware serial
		// number only comes back from the device, below.
		let pad = Touchy::from_transport(dev.open().await?);
		let info = pad.client().sys_board_info_get().await?;
		println!(
			"{} → board={} fw={} display={}x{} touch={} multitouch={} usb={} serial={}",
			dev.describe(),
			info.board_name,
			info.firmware_version_str,
			info.display_width,
			info.display_height,
			info.is_touchable,
			info.is_multitouch,
			info.has_usb,
			info.serial
		);
		pad.close().await;
	}
	Ok(())
}
