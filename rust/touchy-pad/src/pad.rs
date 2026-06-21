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
use crate::images::{looks_like_supported_image, normalize_for_device, rewrite_to_bin_path};
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
	// Serialises whole file-write sequences. The wire protocol has a
	// single global write transaction on the device: an `open_write`
	// from task B that lands between task A's `open_write` and
	// `file_close` aborts A's transaction (the device logs "stale
	// transaction detected" and drops A's bytes). Individual RPCs are
	// already serialised by the transport, but a multi-RPC
	// open→write→close must stay atomic as a whole. Every writer
	// ([`file_write_raw`]) holds this for the duration of its sequence.
	write_lock: Mutex<()>,
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
			// Stage 83: try native USB first, then UART-bridge auto-discovery
			// (CH340-attached CYD boards). Re-raise the USB error if neither
			// transport finds a device.
			match UsbTransport::open().await {
				Ok(t) => Arc::new(t),
				Err(usb_err) => {
					#[cfg(feature = "serial")]
					{
						let mut serial: Option<Arc<dyn Transport>> = None;
						for path in crate::transport_serial::discover_serial_ports() {
							match crate::transport_serial::SerialTransport::open(&path) {
								Ok(t) => {
									serial = Some(Arc::new(t));
									break;
								}
								Err(e) => {
									log::debug!("Touchy::open: serial port {path} did not open ({e}); trying next");
								}
							}
						}
						match serial {
							Some(t) => t,
							None => return Err(usb_err),
						}
					}
					#[cfg(not(feature = "serial"))]
					{
						return Err(usb_err);
					}
				}
			}
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
			write_lock: Mutex::new(()),
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
		let needs_conversion = self.client.transport().needs_image_conversion();
		// Path: rewrite a recognised image extension to `.bin` only when
		// we are actually converting. Bytes: delegate to the shared
		// normaliser so this path and the image cache agree byte-for-byte.
		let final_path = if needs_conversion && looks_like_supported_image(data) {
			rewrite_to_bin_path(path)
		} else {
			path.to_string()
		};
		let (bytes, _suffix) = normalize_for_device(data, needs_conversion, None)?;
		self.file_write_raw(&final_path, &bytes).await?;
		Ok(final_path)
	}

	/// Stream `data` to `path` verbatim (chunked), with no image
	/// conversion or path rewriting. Used by callers that have already
	/// normalised their bytes (e.g. [`crate::image_cache::ImageCache`]).
	pub async fn file_write_raw(&self, path: &str, data: &[u8]) -> Result<()> {
		// Hold the write lock for the entire open→write→close so a
		// concurrent writer can't open the device's single global write
		// transaction mid-sequence and abort ours (see `write_lock`).
		let wait_start = std::time::Instant::now();
		let _guard = self.write_lock.lock().await;
		let waited = wait_start.elapsed();
		let seq_start = std::time::Instant::now();
		if waited >= Duration::from_millis(50) {
			log::debug!("file_write_raw('{path}'): waited {waited:?} for write_lock");
		}
		log::debug!("file_write_raw('{path}'): {} bytes — begin", data.len());
		let handle = self.client.file_open_write(path).await?;
		let result: Result<()> = async {
			for chunk in data.chunks(FILE_WRITE_CHUNK) {
				self.client.file_write(handle, chunk).await?;
			}
			self.client.file_close(handle, true).await?;
			Ok(())
		}
		.await;
		if let Err(ref e) = result {
			log::warn!("file_write_raw('{path}'): failed after {:?} (handle={handle}): {e} — aborting", seq_start.elapsed());
			let _ = self.client.file_close(handle, false).await;
		} else {
			log::debug!("file_write_raw('{path}'): committed in {:?}", seq_start.elapsed());
		}
		result
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
		self.widget_save(&path, widget).await
	}

	/// Encode a [`Widget`] and write it verbatim to `path` (no image
	/// conversion). Useful for standalone `WidgetRef` target files such
	/// as the OpenDeck plugin's per-key stubs.
	pub async fn widget_save(&self, path: &str, widget: &Widget) -> Result<()> {
		let mut buf = Vec::with_capacity(widget.encoded_len());
		widget.encode(&mut buf)?;
		self.file_write_raw(path, &buf).await
	}

	/// Page the default chrome's `widget_ref(id="page")` to a
	/// previously-saved user screen body (Stage 71).
	///
	/// `name` is the bare stem passed to [`user_screen_save`][Self::user_screen_save]
	/// (e.g. `"opendeck"`). Issues a [`RunActionsCmd`][crate::proto::RunActionsCmd]
	/// carrying an `ActionChangeWidgetRef` that swaps the chrome's
	/// `page` ref to `F:host/uscr/<name>.pb` — so the page shows
	/// without a user touch. The target id (`"page"`) matches the id
	/// the default chrome assigns its body `widget_ref`.
	pub async fn show_user_screen(&self, name: &str) -> Result<()> {
		use crate::USER_SCREENS_DIR;
		use crate::proto::{Action, ActionChangeWidgetRef, ActionDevice, action, action_change_widget_ref, action_device};
		let path = format!("{USER_SCREENS_DIR}{name}.pb");
		let act = Action {
			kind: Some(action::Kind::Device(ActionDevice {
				kind: Some(action_device::Kind::ChangeWidgetRef(ActionChangeWidgetRef {
					behavior: action_change_widget_ref::Behavior::ByPath as i32,
					target_id: "page".into(),
					path,
				})),
			})),
		};
		self.client.run_actions(vec![act]).await
	}

	/// Repoint an `ImageButton`'s released- or pressed-state image in
	/// place (Stage 86), addressed by the button's own `Widget.id`.
	///
	/// Issues a [`RunActionsCmd`][crate::proto::RunActionsCmd] carrying an
	/// `ActionChangeWidgetRef` with the `IMAGE_BUTTON_RELEASED` /
	/// `IMAGE_BUTTON_PRESSED` behaviour, so the device swaps just that
	/// image source via its Stage 60 registry — without rebuilding the
	/// widget. A button the user is currently pressing therefore keeps
	/// its touch state and still emits its release event.
	///
	/// `path` is a drive-prefixed image asset (e.g. a cached
	/// `T:host/icache/<hash>.bin` from
	/// [`ImageCache`][crate::image_cache::ImageCache]). The change is
	/// visible at once only if that slot is the one currently displayed;
	/// otherwise the bytes are staged for the next press/release edge.
	pub async fn set_image_button_slot(&self, target_id: &str, pressed: bool, path: &str) -> Result<()> {
		use crate::proto::{Action, ActionChangeWidgetRef, ActionDevice, action, action_change_widget_ref, action_device};
		let behavior = if pressed {
			action_change_widget_ref::Behavior::ImageButtonPressed
		} else {
			action_change_widget_ref::Behavior::ImageButtonReleased
		} as i32;
		let act = Action {
			kind: Some(action::Kind::Device(ActionDevice {
				kind: Some(action_device::Kind::ChangeWidgetRef(ActionChangeWidgetRef {
					behavior,
					target_id: target_id.into(),
					path: path.into(),
				})),
			})),
		};
		self.client.run_actions(vec![act]).await
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
