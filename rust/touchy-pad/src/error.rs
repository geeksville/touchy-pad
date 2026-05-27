//! Error type and result alias for the Touchy-Pad library.

use thiserror::Error;

/// Errors returned by every fallible operation in this crate.
#[derive(Debug, Error)]
pub enum TouchyError {
	/// No Touchy-Pad device was found on the USB bus.
	#[error("no Touchy-Pad device found (VID:PID {vid:#06x}:{pid:#06x})")]
	DeviceNotFound {
		/// USB Vendor ID searched for.
		vid: u16,
		/// USB Product ID searched for.
		pid: u16,
	},

	/// Failure talking to USB (open, claim, bulk read/write).
	#[error("USB I/O error: {0}")]
	Usb(String),

	/// A bulk read returned an unexpected number of bytes, or a frame
	/// header advertised an impossible length.
	#[error("framing error: {0}")]
	Framing(String),

	/// `prost` failed to encode/decode a protobuf message.
	#[error("protobuf error: {0}")]
	Proto(String),

	/// Device returned a non-OK [`crate::proto::ResultCode`].
	#[error("device returned {name} ({code})")]
	Device {
		/// Numeric `ResultCode` value from the device.
		code: i32,
		/// Symbolic name (e.g. `RESULT_NOT_FOUND`).
		name: String,
	},

	/// PNG / JPEG / image decode or encode failure.
	#[error("image error: {0}")]
	Image(String),

	/// Catch-all I/O (file system, etc.).
	#[error(transparent)]
	Io(#[from] std::io::Error),

	/// The transport's background event channel was closed.
	#[error("event stream closed")]
	EventStreamClosed,

	/// Operation timed out.
	#[error("operation timed out after {0:?}")]
	Timeout(std::time::Duration),

	/// Non-USB transport I/O failure (e.g. TCP sim connection).
	#[error("transport error: {0}")]
	Transport(String),

	/// Anything else.
	#[error("{0}")]
	Other(String),
}

impl From<prost::EncodeError> for TouchyError {
	fn from(e: prost::EncodeError) -> Self {
		TouchyError::Proto(e.to_string())
	}
}

impl From<prost::DecodeError> for TouchyError {
	fn from(e: prost::DecodeError) -> Self {
		TouchyError::Proto(e.to_string())
	}
}

impl From<image::ImageError> for TouchyError {
	fn from(e: image::ImageError) -> Self {
		TouchyError::Image(e.to_string())
	}
}

/// Crate-wide convenience alias.
pub type Result<T> = std::result::Result<T, TouchyError>;
