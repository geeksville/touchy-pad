//! High-level [`Touchy`] handle wrapping [`Client`] with a background
//! event-poll task.
//!
//! Mirrors the `Touchy` class in `app/src/touchy_pad/api/device.py`.

use std::sync::Arc;
use std::time::Duration;

use tokio::sync::Mutex;
use tokio::sync::mpsc;
use tokio::task::JoinHandle;

use crate::client::{Client, FILE_WRITE_CHUNK};
use crate::error::{Result, TouchyError};
use crate::images::{LvFormat, looks_like_supported_image, rewrite_to_bin_path, to_lvgl_bin};
use crate::proto::{LvEvent, Screen};
use crate::transport::Transport;
use crate::transport_usb::UsbTransport;
use prost::Message;

/// High-level handle to a connected Touchy-Pad.
///
/// Wraps a [`Client`] with:
///
/// * a background tokio task that polls [`Client::event_consume`] and
///   forwards events to a [`tokio::sync::mpsc::Receiver`] handed out by
///   [`Touchy::events`];
/// * helpers like [`Touchy::file_save`] that chunk large payloads and
///   transparently convert PNG/JPEG/etc. to LVGL `.bin`.
pub struct Touchy {
	client: Client,
	events_rx: Mutex<Option<mpsc::Receiver<LvEvent>>>,
	poller: Mutex<Option<JoinHandle<()>>>,
	shutdown: Arc<tokio::sync::Notify>,
}

impl Touchy {
	/// Open the first connected Touchy-Pad via USB.
	pub async fn open() -> Result<Self> {
		let transport: Arc<dyn Transport> = Arc::new(UsbTransport::open().await?);
		Ok(Self::from_transport(transport))
	}

	/// Build a [`Touchy`] around any [`Transport`] implementation.
	/// The background event poller is started automatically.
	pub fn from_transport(transport: Arc<dyn Transport>) -> Self {
		let client = Client::new(transport);
		let (tx, rx) = mpsc::channel::<LvEvent>(64);
		let shutdown = Arc::new(tokio::sync::Notify::new());
		let poller = spawn_event_poller(client.clone(), tx, shutdown.clone());
		Self {
			client,
			events_rx: Mutex::new(Some(rx)),
			poller: Mutex::new(Some(poller)),
			shutdown,
		}
	}

	/// Borrow the low-level RPC client for cases the helpers below
	/// don't cover.
	pub fn client(&self) -> &Client {
		&self.client
	}

	/// Take ownership of the event receiver. The first caller gets it;
	/// subsequent callers see `Ok(None)`.
	pub async fn events(&self) -> Option<mpsc::Receiver<LvEvent>> {
		self.events_rx.lock().await.take()
	}

	/// Save a file to the device, chunking large payloads and
	/// transparently converting recognised image formats to LVGL
	/// `.bin` when the transport requests it.
	///
	/// Returns the on-device path actually used — equal to `path`
	/// except when the input was an image and the extension was
	/// rewritten to `.bin`.
	pub async fn file_save(&self, path: &str, data: &[u8]) -> Result<String> {
		let (final_path, converted): (String, Option<Vec<u8>>) = if self.client.transport().needs_image_conversion() && looks_like_supported_image(data) {
			(rewrite_to_bin_path(path), Some(to_lvgl_bin(data, LvFormat::Auto)?))
		} else {
			(path.to_string(), None)
		};
		let data: &[u8] = converted.as_deref().unwrap_or(data);

		let handle = self.client.file_open_write(&final_path).await?;
		let result: Result<()> = async {
			for chunk in data.chunks(FILE_WRITE_CHUNK) {
				self.client.file_write(handle, chunk).await?;
			}
			self.client.file_close(handle, true).await?;
			Ok(())
		}
		.await;
		if result.is_err() {
			let _ = self.client.file_close(handle, false).await;
		}
		result.map(|_| final_path)
	}

	/// Encode and save a [`Screen`] message at `path`.
	pub async fn screen_save(&self, path: &str, screen: &Screen) -> Result<()> {
		let mut buf = Vec::with_capacity(screen.encoded_len());
		screen.encode(&mut buf)?;
		self.file_save(path, &buf).await?;
		Ok(())
	}

	/// Activate a previously-uploaded screen.
	pub async fn screen_load(&self, path: &str) -> Result<()> {
		self.client.screen_load(path).await
	}

	/// Stop the background event poller. Called automatically on drop.
	pub async fn close(&self) {
		self.shutdown.notify_waiters();
		if let Some(handle) = self.poller.lock().await.take() {
			let _ = handle.await;
		}
	}
}

impl Drop for Touchy {
	fn drop(&mut self) {
		self.shutdown.notify_waiters();
	}
}

fn spawn_event_poller(client: Client, tx: mpsc::Sender<LvEvent>, shutdown: Arc<tokio::sync::Notify>) -> JoinHandle<()> {
	tokio::spawn(async move {
		let poll_interval = Duration::from_millis(50);
		loop {
			tokio::select! {
				_ = shutdown.notified() => break,
				res = client.event_consume() => {
					match res {
						Ok(Some(evt)) => {
							if tx.send(evt).await.is_err() {
								// Receiver dropped — done.
								break;
							}
						}
						Ok(None) => {
							tokio::time::sleep(poll_interval).await;
						}
						Err(e) => {
							log::warn!("event poller: {e}");
							tokio::time::sleep(poll_interval).await;
						}
					}
				}
			}
		}
	})
}

// `TouchyError` used only via `Result<>` here; touch it so the
// import doesn't trip dead_code on minimal feature sets.
#[allow(dead_code)]
fn _touch(e: TouchyError) -> TouchyError {
	e
}
