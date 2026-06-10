//! Low-level RPC client over a [`Transport`].
//!
//! Each method wraps one [`crate::proto::Command`] / [`crate::proto::Response`]
//! exchange. The high-level [`crate::Touchy`] in `pad.rs` layers
//! convenience (file streaming, image conversion, event delivery) on
//! top of this.

use std::sync::Arc;
use std::time::Duration;

use prost::Message;
use tokio::sync::Mutex;

use crate::error::{Result, TouchyError};
use crate::proto::Action;
use crate::proto::{Command, LogPriority, LogRecord, LvEvent, ResultCode, command, response};
use crate::proto::{
	EventConsumeCmd, FileCloseCmd, FileDeleteCmd, FileOpenWriteCmd, FileWriteCmd, PreferencesFile, Response, RunActionsCmd, ScreenWakeCmd, SetPreferencesCmd, SysBoardInfoGetCmd, SysBoardInfoResponse,
	SysRebootBootloaderCmd,
};
use crate::transport::Transport;

const DEFAULT_TIMEOUT: Duration = Duration::from_secs(2);

/// Max payload bytes per [`FileWriteCmd`] — the on-wire frame must fit
/// inside the device's RX buffer (5 KiB after stage 51); we leave
/// ~1 KiB of headroom for the surrounding `Command`/oneof overhead.
pub const FILE_WRITE_CHUNK: usize = 4096;

/// Low-level RPC client wrapping a [`Transport`].
#[derive(Clone)]
pub struct Client {
	transport: Arc<dyn Transport>,
	// Stage 64.1: `drain_pending()` may surface an `LvEvent` while
	// draining tunneled log records at connect time; we park it
	// here so the next `event_consume()` call returns it instead
	// of polling the device again. Shared via `Arc<Mutex<_>>` so
	// cloned `Client`s (e.g. the one handed to `spawn_event_poller`)
	// see the same parked event.
	pending_event: Arc<Mutex<Option<LvEvent>>>,
}

impl Client {
	/// Build a client around any [`Transport`] implementation.
	pub fn new(transport: Arc<dyn Transport>) -> Self {
		Self {
			transport,
			pending_event: Arc::new(Mutex::new(None)),
		}
	}

	/// Borrow the underlying transport (for [`Transport::needs_image_conversion`]).
	pub fn transport(&self) -> &Arc<dyn Transport> {
		&self.transport
	}

	async fn rpc(&self, cmd: Command) -> Result<Response> {
		let mut buf = Vec::with_capacity(cmd.encoded_len());
		cmd.encode(&mut buf)?;
		self.transport.send_command(&buf).await?;
		let reply = self.transport.recv_response(DEFAULT_TIMEOUT).await?;
		let resp = Response::decode(reply.as_slice())?;
		Ok(resp)
	}

	fn check(resp: Response) -> Result<Response> {
		if resp.code == ResultCode::ResultOk as i32 {
			Ok(resp)
		} else {
			let name = ResultCode::try_from(resp.code)
				.map(|c| c.as_str_name().to_string())
				.unwrap_or_else(|_| format!("UNKNOWN({})", resp.code));
			Err(TouchyError::Device { code: resp.code, name })
		}
	}

	// -- typed wrappers ----------------------------------------------------

	/// Return board / firmware info.
	pub async fn sys_board_info_get(&self) -> Result<SysBoardInfoResponse> {
		let resp = Self::check(
			self.rpc(Command {
				cmd: Some(command::Cmd::SysBoardInfoGet(SysBoardInfoGetCmd {})),
			})
			.await?,
		)?;
		match resp.payload {
			Some(response::Payload::SysBoardInfo(b)) => Ok(b),
			_ => Err(TouchyError::Proto("sys_board_info_get: response payload missing".into())),
		}
	}

	/// Reboot into the ESP32 ROM bootloader (USB-DFU).
	pub async fn sys_reboot_bootloader(&self) -> Result<()> {
		Self::check(
			self.rpc(Command {
				cmd: Some(command::Cmd::SysRebootBootloader(SysRebootBootloaderCmd {})),
			})
			.await?,
		)?;
		Ok(())
	}

	/// Wake the display backlight.
	pub async fn screen_wake(&self) -> Result<()> {
		Self::check(
			self.rpc(Command {
				cmd: Some(command::Cmd::ScreenWake(ScreenWakeCmd {})),
			})
			.await?,
		)?;
		Ok(())
	}

	/// Apply a partial set of device preferences (Stage 82). Only the
	/// fields present on `prefs` are changed device-side; the device
	/// persists the merged result.
	pub async fn set_preferences(&self, prefs: PreferencesFile) -> Result<()> {
		Self::check(
			self.rpc(Command {
				cmd: Some(command::Cmd::SetPreferences(SetPreferencesCmd { prefs: Some(prefs) })),
			})
			.await?,
		)?;
		Ok(())
	}

	/// Configure the display sleep timeout (milliseconds; 0 = never).
	pub async fn screen_sleep_timeout(&self, timeout_ms: u32) -> Result<()> {
		self.set_preferences(PreferencesFile {
			screen_timeout_ms: Some(timeout_ms),
			..Default::default()
		})
		.await
	}

	/// Set the minimum device log priority queued back to the host
	/// (Stage 82). Records below this level are dropped device-side.
	pub async fn set_min_log_level(&self, level: LogPriority) -> Result<()> {
		self.set_preferences(PreferencesFile {
			min_log_level: Some(level as u32),
			..Default::default()
		})
		.await
	}

	/// Configure the early-boot delay in seconds (Stage 82; 0 = none).
	pub async fn set_boot_delay(&self, seconds: u32) -> Result<()> {
		self.set_preferences(PreferencesFile {
			boot_delay_s: Some(seconds),
			..Default::default()
		})
		.await
	}

	/// Delete a file or directory subtree.
	pub async fn file_delete(&self, path: &str) -> Result<()> {
		Self::check(
			self.rpc(Command {
				cmd: Some(command::Cmd::FileDelete(FileDeleteCmd { path: path.into() })),
			})
			.await?,
		)?;
		Ok(())
	}

	/// Begin streaming a file write. Returns an opaque write handle.
	pub async fn file_open_write(&self, path: &str) -> Result<u32> {
		let resp = Self::check(
			self.rpc(Command {
				cmd: Some(command::Cmd::FileOpenWrite(FileOpenWriteCmd { path: path.into() })),
			})
			.await?,
		)?;
		match resp.payload {
			Some(response::Payload::FileOpenWrite(r)) => Ok(r.handle),
			_ => Err(TouchyError::Proto("file_open_write: response payload missing".into())),
		}
	}

	/// Append a chunk to an in-progress file write.
	pub async fn file_write(&self, handle: u32, data: &[u8]) -> Result<()> {
		Self::check(
			self.rpc(Command {
				cmd: Some(command::Cmd::FileWrite(FileWriteCmd { handle, data: data.to_vec() })),
			})
			.await?,
		)?;
		Ok(())
	}

	/// Finish (commit=true) or abort (commit=false) a streaming write.
	pub async fn file_close(&self, handle: u32, commit: bool) -> Result<()> {
		Self::check(
			self.rpc(Command {
				cmd: Some(command::Cmd::FileClose(FileCloseCmd { handle, commit })),
			})
			.await?,
		)?;
		Ok(())
	}

	/// Activate a previously-uploaded screen. Empty path = default screen.
	pub async fn screen_load(&self, path: &str) -> Result<()> {
		self.set_preferences(PreferencesFile {
			current_screen: Some(path.into()),
			..Default::default()
		})
		.await
	}

	/// Run a list of [`Action`]s device-side as if a local widget fired
	/// them (Stage 71). Used to drive device-local behaviour from the
	/// host — e.g. forcing a page to the front via an
	/// `ActionChangeWidgetRef` without a user touch.
	pub async fn run_actions(&self, actions: Vec<Action>) -> Result<()> {
		Self::check(
			self.rpc(Command {
				cmd: Some(command::Cmd::RunActions(RunActionsCmd { actions })),
			})
			.await?,
		)?;
		Ok(())
	}

	/// Pop one pending [`LvEvent`] from the device queue.
	///
	/// Returns `Ok(None)` only when the device's event *and* log
	/// queues are both empty. Stage 64.1: tunneled [`LogRecord`]
	/// payloads are consumed transparently (forwarded to the [`log`]
	/// crate facade) and this method keeps polling in the same call
	/// until either an [`LvEvent`] surfaces or the device reports
	/// `ResultNotFound` — that way the caller's poll-and-sleep loop
	/// doesn't insert latency between consecutive log records.
	pub async fn event_consume(&self) -> Result<Option<LvEvent>> {
		{
			let mut parked = self.pending_event.lock().await;
			if let Some(evt) = parked.take() {
				return Ok(Some(evt));
			}
		}
		loop {
			match self.poll().await? {
				Some(PollItem::Event(evt)) => return Ok(Some(evt)),
				Some(PollItem::Log(rec)) => {
					// LogRecord — dispatch and keep draining; there may
					// be more logs queued, or an event right behind them.
					dispatch_log_record(&rec);
				}
				None => return Ok(None),
			}
		}
	}

	/// Drain queued events and log records from the device.
	///
	/// Log records are forwarded to the [`log`] crate. If an
	/// [`LvEvent`] surfaces during the drain it is parked for the
	/// next [`Client::event_consume`] call so no event is lost. Caps
	/// at `max_iterations` polls as a runaway guard.
	pub async fn drain_pending(&self, max_iterations: usize) -> Result<()> {
		for _ in 0..max_iterations {
			match self.poll().await? {
				None => return Ok(()),
				Some(PollItem::Event(evt)) => {
					// Save for the next event_consume; stop draining so
					// subsequent logs queued behind this event are
					// picked up by the normal polling loop (which
					// preserves the device-side events-first ordering).
					*self.pending_event.lock().await = Some(evt);
					return Ok(());
				}
				Some(PollItem::Log(rec)) => dispatch_log_record(&rec),
			}
		}
		Ok(())
	}

	/// Low-level drain: pop whichever of an [`LvEvent`] or [`LogRecord`]
	/// the device has pending, or `None` when both queues are empty.
	pub async fn poll(&self) -> Result<Option<PollItem>> {
		let resp = self
			.rpc(Command {
				cmd: Some(command::Cmd::EventConsume(EventConsumeCmd {})),
			})
			.await?;
		if resp.code == ResultCode::ResultNotFound as i32 {
			return Ok(None);
		}
		let resp = Self::check(resp)?;
		match resp.payload {
			Some(response::Payload::EventConsume(ec)) => Ok(ec.event.map(PollItem::Event)),
			Some(response::Payload::LogRecord(rec)) => Ok(Some(PollItem::Log(rec))),
			_ => Ok(None),
		}
	}
}

/// Result of [`Client::poll`] — either a UI event or a tunneled device
/// log record (Stage 64.1).
#[derive(Debug, Clone)]
pub enum PollItem {
	/// A UI event from a `Widget` activation.
	Event(LvEvent),
	/// A tunneled ESP_LOG line from the device.
	Log(LogRecord),
}

/// Forward a tunneled [`LogRecord`] into the global [`log`] facade.
/// `LOG_PRIORITY_TRACE` maps to [`log::Level::Trace`]; the originating
/// ESP_LOG TAG becomes the log target so `RUST_LOG=touchy_pad::device::WIFI=…`
/// style filters work out of the box.
pub fn dispatch_log_record(rec: &LogRecord) {
	let level = match LogPriority::try_from(rec.priority).unwrap_or(LogPriority::Trace) {
		LogPriority::Trace => log::Level::Trace,
		LogPriority::Debug => log::Level::Info, // We show device Debug logs at INFO level to make for easier logging
		LogPriority::Info => log::Level::Info,
		LogPriority::Warn => log::Level::Warn,
		LogPriority::Error => log::Level::Error,
	};
	let target = if rec.tag.is_empty() {
		"touchy_pad::device".to_string()
	} else {
		format!("touchy_pad::device::{}", rec.tag)
	};
	log::log!(target: &target, level, "{}", rec.message);
	if rec.num_dropped > 0 {
		log::warn!(
			target: "touchy_pad::device",
			"device dropped {} log record(s) before this one",
			rec.num_dropped
		);
	}
}
