//! High-level [`Touchy`] handle wrapping [`Client`] with a background
//! event-poll task.
//!
//! Mirrors the `Touchy` class in `app/src/touchy_pad/api/device.py`.

use std::sync::Arc;
use std::time::Duration;

use tokio::sync::Mutex;
use tokio::sync::mpsc;
use tokio::sync::watch;
use tokio::task::JoinHandle;

use crate::client::{Client, FILE_WRITE_CHUNK};
use crate::error::{Result, TouchyError};
use crate::images::{LvFormat, looks_like_supported_image, rewrite_to_bin_path, to_lvgl_bin};
use crate::proto::{LvEvent, Screen, Widget};
use crate::transport::Transport;
use crate::transport_net::{TcpTransport, sim_url_from_env};
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
	// `watch` instead of `Notify` because the poll task may not be
	// polled even once before `close()` fires the shutdown signal —
	// `Notify::notify_waiters()` only wakes already-registered
	// waiters and would silently lose the signal in that race.
	shutdown_tx: watch::Sender<bool>,
}

impl Touchy {
	/// Open the first connected Touchy-Pad.
	///
	/// Honours the `TOUCHY_SIM_URL` environment variable (Stage 63):
	/// if set to e.g. `tcp://127.0.0.1:8935`, connects to that
	/// out-of-process Python simulator instead of enumerating USB.
	pub async fn open() -> Result<Self> {
		let transport: Arc<dyn Transport> = if let Some((host, port)) = sim_url_from_env() {
			Arc::new(TcpTransport::connect(&host, port).await?)
		} else {
			Arc::new(UsbTransport::open().await?)
		};
		let touchy = Self::from_transport(transport);
		// Stage 64.1: drain any device-side log records / events
		// buffered before the host connected so the first
		// `events()` consumer doesn't see stale data and the `log`
		// crate facade gets the boot-time ESP_LOG output. Best
		// effort — if the device is unresponsive the regular
		// poll loop will surface the same error shortly.
		if let Err(e) = touchy.client.drain_pending(256).await {
			log::debug!("drain_pending at connect: {e}");
		}
		Ok(touchy)
	}

	/// Build a [`Touchy`] around any [`Transport`] implementation.
	/// The background event poller is started automatically.
	pub fn from_transport(transport: Arc<dyn Transport>) -> Self {
		let client = Client::new(transport);
		let (tx, rx) = mpsc::channel::<LvEvent>(64);
		let (shutdown_tx, shutdown_rx) = watch::channel(false);
		let poller = spawn_event_poller(client.clone(), tx, shutdown_rx);
		Self {
			client,
			events_rx: Mutex::new(Some(rx)),
			poller: Mutex::new(Some(poller)),
			shutdown_tx,
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

	/// Encode a [`Widget`] and save it as a user-screen body under
	/// [`USER_SCREENS_DIR`][crate::USER_SCREENS_DIR].
	///
	/// `name` is the bare stem (e.g. `"rust_demo"`); the path becomes
	/// `F:host/uscr/<name>.pb`. The default chrome's
	/// `widget_ref(id="page")` will cycle through these files via
	/// the Prev / Next buttons.
	pub async fn user_screen_save(&self, name: &str, widget: &Widget) -> Result<()> {
		use crate::USER_SCREENS_DIR;
		let path = format!("{USER_SCREENS_DIR}{name}.pb");
		let mut buf = Vec::with_capacity(widget.encoded_len());
		widget.encode(&mut buf)?;
		self.file_save(&path, &buf).await?;
		Ok(())
	}

	/// Activate a previously-uploaded screen.
	pub async fn screen_load(&self, path: &str) -> Result<()> {
		self.client.screen_load(path).await
	}

	/// Stop the background event poller. Called automatically on drop.
	pub async fn close(&self) {
		let _ = self.shutdown_tx.send(true);
		if let Some(handle) = self.poller.lock().await.take() {
			let _ = handle.await;
		}
	}
}

impl Drop for Touchy {
	fn drop(&mut self) {
		let _ = self.shutdown_tx.send(true);
	}
}

fn spawn_event_poller(client: Client, tx: mpsc::Sender<LvEvent>, mut shutdown_rx: watch::Receiver<bool>) -> JoinHandle<()> {
	tokio::spawn(async move {
		let poll_interval = Duration::from_millis(50);
		// Fast-path: shutdown may already have been signalled before
		// this task gets its first poll. `watch::Receiver::has_changed`
		// reports buffered state, so we can't miss the signal.
		if *shutdown_rx.borrow() {
			return;
		}
		loop {
			tokio::select! {
				biased;
				_ = shutdown_rx.changed() => break,
				res = client.event_consume() => {
					match res {
						Ok(Some(evt)) => {
							if tx.send(evt).await.is_err() {
								// Receiver dropped — done.
								break;
							}
						}
						Ok(None) => {
							tokio::select! {
								biased;
								_ = shutdown_rx.changed() => break,
								_ = tokio::time::sleep(poll_interval) => {}
							}
						}
						Err(e) => {
							log::warn!("event poller: {e}");
							tokio::select! {
								biased;
								_ = shutdown_rx.changed() => break,
								_ = tokio::time::sleep(poll_interval) => {}
							}
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
