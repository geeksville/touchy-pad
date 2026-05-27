//! Low-level RPC client over a [`Transport`].
//!
//! Each method wraps one [`crate::proto::Command`] / [`crate::proto::Response`]
//! exchange. The high-level [`crate::Touchy`] in `pad.rs` layers
//! convenience (file streaming, image conversion, event delivery) on
//! top of this.

use std::sync::Arc;
use std::time::Duration;

use prost::Message;

use crate::error::{Result, TouchyError};
use crate::proto::{Command, LvEvent, ResultCode, command, response};
use crate::proto::{
	EventConsumeCmd, FileCloseCmd, FileDeleteCmd, FileOpenWriteCmd, FileWriteCmd, Response, ScreenLoadCmd, ScreenSleepTimeoutCmd, ScreenWakeCmd, SysBoardInfoGetCmd,
	SysBoardInfoResponse, SysRebootBootloaderCmd,
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
}

impl Client {
	/// Build a client around any [`Transport`] implementation.
	pub fn new(transport: Arc<dyn Transport>) -> Self {
		Self { transport }
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
			let name = ResultCode::try_from(resp.code).map(|c| c.as_str_name().to_string()).unwrap_or_else(|_| format!("UNKNOWN({})", resp.code));
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

	/// Configure the display sleep timeout (milliseconds; 0 = never).
	pub async fn screen_sleep_timeout(&self, timeout_ms: u32) -> Result<()> {
		Self::check(
			self.rpc(Command {
				cmd: Some(command::Cmd::ScreenSleepTimeout(ScreenSleepTimeoutCmd { timeout_ms })),
			})
			.await?,
		)?;
		Ok(())
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
		Self::check(
			self.rpc(Command {
				cmd: Some(command::Cmd::ScreenLoad(ScreenLoadCmd { path: path.into() })),
			})
			.await?,
		)?;
		Ok(())
	}

	/// Pop one pending [`LvEvent`] from the device queue.
	///
	/// Returns `Ok(None)` when the device's queue is empty
	/// (`ResultCode::ResultNotFound`).
	pub async fn event_consume(&self) -> Result<Option<LvEvent>> {
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
			Some(response::Payload::EventConsume(ec)) => Ok(ec.event),
			_ => Ok(None),
		}
	}
}
