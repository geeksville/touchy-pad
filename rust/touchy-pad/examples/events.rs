//! Stream touch events from the device.
//!
//! Widgets carrying `host_action(...)` (see the `screen` example) report their
//! press/release here; `host_code` is the value you passed, and `user_data` is
//! the widget's `id`.
//!
//! ```sh
//! cargo run --example screen   # upload something with host actions
//! cargo run --example events   # then watch it
//! ```

use touchy_pad::proto::{LvEventCode, lv_event};
use touchy_pad::{Touchy, TouchyError};

#[tokio::main]
async fn main() -> touchy_pad::Result<()> {
	env_logger::init();
	let pad = Touchy::open().await?;

	// One-shot: the first caller takes the receiver, later callers get `None`.
	// The background poll task starts with the first call and also forwards
	// device log records into the `log` facade (`RUST_LOG=touchy_pad=debug`).
	let mut rx = pad.events().await.ok_or_else(|| TouchyError::Other("events() was already taken".into()))?;

	println!("listening for touches — Ctrl-C to stop");
	while let Some(evt) = rx.recv().await {
		let code = LvEventCode::try_from(evt.code as i32).map(|c| format!("{c:?}")).unwrap_or_else(|_| evt.code.to_string());
		let state = match &evt.state {
			Some(lv_event::State::Value(v)) => format!(" value={v}"),
			Some(lv_event::State::Checked(c)) => format!(" checked={c}"),
			None => String::new(),
		};
		println!("host_code={} event={code} widget={:?}{state}", evt.host_code, evt.user_data);
	}
	Ok(())
}
