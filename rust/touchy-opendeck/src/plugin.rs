//! OpenDeck device-plugin glue layer.
//!
//! Lifecycle (per physical Touchy-Pad device):
//!
//! 1. The hot-plug watcher in [`crate::main`] spots a new USB device
//!    matching the touchy-pad VID/PID and calls
//!    [`TouchyPlugin::attach`].
//! 2. `attach` opens a USB transport, builds a [`DeviceCtx`], uploads
//!    a fresh [`crate::layout::build_screen`], spawns an event-forwarding
//!    task, and calls `openaction::device_plugin::register_device`.
//! 3. OpenDeck pushes per-key images via
//!    [`TouchyPlugin::handle_set_image`] — we decode the data URL,
//!    save the bytes to the device (which transparently transcodes to
//!    LVGL's `.bin` format), and debounce a `screen_load` so a profile
//!    switch's burst of N keys only redraws once.
//! 4. Touch events flow back as `LvEvent`s; we translate
//!    `code == 1 (PRESSED)` to `key_down` and `code == 8 (RELEASED)`
//!    / `7 (PRESS_LOST)` to `key_up`.
//! 5. When the watcher notices the device is gone, [`TouchyPlugin::detach`]
//!    drops the context and tells OpenDeck via `unregister_device`.

use std::collections::HashMap;
use std::sync::Arc;
use std::time::Duration;

use anyhow::{Context, Result, anyhow};
use async_trait::async_trait;
use base64::Engine as _;
use base64::engine::general_purpose::STANDARD as B64;
use nusb::DeviceInfo;
use openaction::OpenActionResult;
use openaction::device_plugin;
use openaction::global_events::{GlobalEventHandler, SetBrightnessEvent, SetImageEvent};
use tokio::sync::{Mutex, RwLock};
use tokio::task::JoinHandle;
use touchy_pad::Touchy;
use touchy_pad::proto::lv_event;
use touchy_pad::transport_usb::{UsbTransport, enumerate, usb_vid_pid};

use crate::layout;

/// LVGL ``LV_EVENT_PRESSED`` — emitted at the start of a touch.
const LV_EVENT_PRESSED: u32 = 1;
/// LVGL ``LV_EVENT_PRESS_LOST`` — finger slid off or USB stalled.
const LV_EVENT_PRESS_LOST: u32 = 7;
/// LVGL ``LV_EVENT_RELEASED`` — the press completed normally.
const LV_EVENT_RELEASED: u32 = 8;

/// Nominal key size in pixels used to compute the grid from the device's
/// reported `display_width` / `display_height`. 96 px gives 5 × 3 on the
/// 480 × 320 jc4827w543 panel; 800 × 480 yields 8 × 5, etc.
const KEY_PX: u32 = 96;

/// OpenDeck device-type byte (see `docs/opendeck-device-plugin.md`).
/// `0` (StreamDeck Original 3×5) is the closest match for a flat
/// LCD touch grid with no rotary encoders.
const DEVICE_TYPE: u8 = 0;

/// Per-attached-device state.
struct DeviceCtx {
	pad: Arc<Touchy>,
	device_id: String,
	screen_path: String,
	cols: u8,
	rows: u8,
	/// Debounce handle for `screen_load` after a burst of `set_image`s.
	reload_handle: Mutex<Option<JoinHandle<()>>>,
	/// Event-forwarding task; cancelled on detach.
	event_task: Mutex<Option<JoinHandle<()>>>,
}

impl DeviceCtx {
	fn name(&self) -> String {
		format!("Touchy-Pad ({})", self.device_id)
	}
}

/// Singleton plugin object — owned by `Box::leak` in `main`.
#[derive(Clone, Default)]
pub struct TouchyPlugin {
	devices: Arc<RwLock<HashMap<String, Arc<DeviceCtx>>>>,
}

impl TouchyPlugin {
	pub fn new() -> Self {
		Self::default()
	}

	/// Background hot-plug loop. Polls every second and diffs against
	/// the set of devices we've already attached.
	pub async fn run_hotplug_loop(self: Arc<Self>) {
		let (vid, pid) = usb_vid_pid();
		let mut interval = tokio::time::interval(Duration::from_secs(1));
		interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
		loop {
			interval.tick().await;
			let infos = match enumerate(vid, pid).await {
				Ok(v) => v,
				Err(e) => {
					log::debug!("usb enumerate failed: {e:#}");
					continue;
				}
			};
			let mut seen = Vec::with_capacity(infos.len());
			for info in &infos {
				let id = layout::device_id_for(info.busnum(), info.device_address());
				seen.push(id.clone());
				if !self.devices.read().await.contains_key(&id) {
					if let Err(e) = self.attach(info, &id).await {
						log::warn!("attach {id} failed: {e:#}");
					}
				}
			}
			// Detach gone devices.
			let to_drop: Vec<String> = {
				let map = self.devices.read().await;
				map.keys().filter(|k| !seen.contains(k)).cloned().collect()
			};
			for id in to_drop {
				self.detach(&id).await;
			}
		}
	}

	async fn attach(&self, info: &DeviceInfo, device_id: &str) -> Result<()> {
		log::info!("attaching {device_id}");
		let transport = UsbTransport::open_info(info).await.context("open usb transport")?;
		let pad = Arc::new(Touchy::from_transport(Arc::new(transport)));

		let board = pad.client().sys_board_info_get().await.context("sys_board_info_get")?;
		if board.display_width == 0 || board.display_height == 0 {
			return Err(anyhow!(
				"{device_id} reported zero display dimensions ({}×{})",
				board.display_width,
				board.display_height
			));
		}
		let cols = u8::try_from(board.display_width / KEY_PX)
			.context("cols overflow")?;
		let rows = u8::try_from(board.display_height / KEY_PX)
			.context("rows overflow")?;
		if cols == 0 || rows == 0 {
			return Err(anyhow!(
				"{device_id} display {}×{} is smaller than one key ({KEY_PX} px)",
				board.display_width,
				board.display_height
			));
		}
		let screen_path = layout::screen_path_for(device_id);
		let screen = layout::build_screen(cols, rows, device_id);
		pad.screen_save(&screen_path, &screen).await.context("screen_save")?;
		pad.screen_load(&screen_path).await.context("screen_load")?;

		// Take the event receiver before we hand the pad off.
		let mut rx = pad.events().await.ok_or_else(|| anyhow!("events() already taken"))?;
		let id_for_task = device_id.to_string();
		let event_task = tokio::spawn(async move {
			while let Some(evt) = rx.recv().await {
				if let Some(state) = &evt.state
					&& let lv_event::State::Value(_) | lv_event::State::Checked(_) = state
				{
					// Slider/toggle events — not applicable to a key grid.
				}
				let Some(key) = layout::key_for_host_code(evt.host_code) else { continue };
				let res = match evt.code {
					LV_EVENT_PRESSED => device_plugin::key_down(id_for_task.clone(), key).await,
					LV_EVENT_RELEASED | LV_EVENT_PRESS_LOST => device_plugin::key_up(id_for_task.clone(), key).await,
					_ => continue,
				};
				if let Err(e) = res {
					log::warn!("dispatch event for {id_for_task}/key{key}: {e:?}");
				}
			}
			log::info!("event stream for {id_for_task} ended");
		});

		let ctx = Arc::new(DeviceCtx {
			pad,
			device_id: device_id.to_string(),
			screen_path,
			cols,
			rows,
			reload_handle: Mutex::new(None),
			event_task: Mutex::new(Some(event_task)),
		});

		self.devices.write().await.insert(device_id.to_string(), ctx.clone());

		// Tell OpenDeck about the new device.
		device_plugin::register_device(device_id.to_string(), ctx.name(), rows, cols, /* encoders */ 0, DEVICE_TYPE)
			.await
			.map_err(|e| anyhow!("register_device: {e:?}"))?;
		log::info!("registered {device_id} as {cols}x{rows}");
		Ok(())
	}

	async fn detach(&self, device_id: &str) {
		log::info!("detaching {device_id}");
		let ctx = self.devices.write().await.remove(device_id);
		if let Some(ctx) = ctx {
			if let Some(h) = ctx.reload_handle.lock().await.take() {
				h.abort();
			}
			if let Some(h) = ctx.event_task.lock().await.take() {
				h.abort();
			}
			ctx.pad.close().await;
		}
		if let Err(e) = device_plugin::unregister_device(device_id.to_string()).await {
			log::warn!("unregister_device {device_id}: {e:?}");
		}
	}

	async fn ctx_for(&self, device_id: &str) -> Option<Arc<DeviceCtx>> {
		self.devices.read().await.get(device_id).cloned()
	}

	async fn schedule_reload(&self, ctx: &Arc<DeviceCtx>) {
		let mut slot = ctx.reload_handle.lock().await;
		if let Some(h) = slot.take() {
			h.abort();
		}
		let pad = ctx.pad.clone();
		let path = ctx.screen_path.clone();
		let id = ctx.device_id.clone();
		*slot = Some(tokio::spawn(async move {
			tokio::time::sleep(Duration::from_millis(100)).await;
			if let Err(e) = pad.screen_load(&path).await {
				log::warn!("screen_load after image burst for {id}: {e:#}");
			}
		}));
	}

	async fn handle_set_image(&self, ev: SetImageEvent) -> Result<()> {
		let Some(ctx) = self.ctx_for(&ev.device).await else {
			log::debug!("set_image for unknown device {}", ev.device);
			return Ok(());
		};
		match (ev.position, ev.image.as_deref()) {
			(Some(pos), Some(data_url)) => {
				let key = u8::try_from(pos).context("position out of u8 range")?;
				if key as usize >= (ctx.cols as usize) * (ctx.rows as usize) {
					log::debug!("set_image position {key} out of range for {}", ctx.device_id);
					return Ok(());
				}
				let b64 = data_url.split_once(',').map(|(_, b)| b).unwrap_or(data_url);
				let bytes = B64.decode(b64.trim()).context("base64 decode")?;
				let asset = layout::asset_path_for(&ctx.device_id, key);
				ctx.pad.file_save(&asset, &bytes).await.context("file_save")?;
				self.schedule_reload(&ctx).await;
			}
			(Some(pos), None) => {
				// Clear single key — write a 1px transparent PNG so
				// LVGL has something to render. Easiest portable
				// approach: drop the asset and reload.
				let key = u8::try_from(pos).context("position out of u8 range")?;
				let asset = layout::asset_path_for(&ctx.device_id, key);
				let _ = ctx.pad.client().file_delete(&asset).await;
				self.schedule_reload(&ctx).await;
			}
			(None, None) => {
				// Clear all — drop every cell's asset and reload.
				for k in 0..(ctx.cols * ctx.rows) {
					let asset = layout::asset_path_for(&ctx.device_id, k);
					let _ = ctx.pad.client().file_delete(&asset).await;
				}
				self.schedule_reload(&ctx).await;
			}
			(None, Some(_)) => {
				log::debug!("set_image with image but no position — ignored");
			}
		}
		Ok(())
	}

	async fn handle_set_brightness(&self, ev: SetBrightnessEvent) -> Result<()> {
		let Some(ctx) = self.ctx_for(&ev.device).await else { return Ok(()) };
		// Touchy-Pad doesn't expose a brightness knob via the host
		// API yet; we map 0 → sleep ASAP, anything else → wake.
		// Document this limitation in the README.
		if ev.brightness == 0 {
			ctx.pad.client().screen_sleep_timeout(1).await.context("screen_sleep_timeout")?;
		} else {
			ctx.pad.client().screen_wake().await.context("screen_wake")?;
		}
		Ok(())
	}
}

#[async_trait]
impl GlobalEventHandler for TouchyPlugin {
	async fn plugin_ready(&self) -> OpenActionResult<()> {
		log::info!("plugin_ready — starting hot-plug watcher");
		let me = Arc::new(self.clone());
		tokio::spawn(async move { me.run_hotplug_loop().await });
		Ok(())
	}

	async fn device_plugin_set_image(&self, event: SetImageEvent) -> OpenActionResult<()> {
		if let Err(e) = self.handle_set_image(event).await {
			log::warn!("set_image failed: {e:#}");
		}
		Ok(())
	}

	async fn device_plugin_set_brightness(&self, event: SetBrightnessEvent) -> OpenActionResult<()> {
		if let Err(e) = self.handle_set_brightness(event).await {
			log::warn!("set_brightness failed: {e:#}");
		}
		Ok(())
	}
}
