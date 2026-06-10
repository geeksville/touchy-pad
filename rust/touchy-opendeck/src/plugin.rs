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
//!    `code == LV_EVENT_PRESSED` to `key_down` and `LV_EVENT_RELEASED`
//!    / `LV_EVENT_PRESS_LOST` to `key_up`.
//! 5. When the watcher notices the device is gone, [`TouchyPlugin::detach`]
//!    drops the context and tells OpenDeck via `unregister_device`.

use std::collections::HashMap;
use std::collections::HashSet;
use std::sync::Arc;
use std::time::Duration;

use anyhow::{Context, Result, anyhow};
use async_trait::async_trait;
use base64::Engine as _;
use base64::engine::general_purpose::STANDARD as B64;
use openaction::OpenActionResult;
use openaction::device_plugin;
use openaction::global_events::{GlobalEventHandler, SetBrightnessEvent, SetImageEvent};
use tokio::sync::{Mutex, RwLock};
use tokio::task::JoinHandle;
use touchy_pad::image_cache::ImageCache;
use touchy_pad::proto::LogPriority;
use touchy_pad::proto::LvEventCode;
use touchy_pad::proto::lv_event;
use touchy_pad::{DiscoveredDevice, Touchy, discover};

use crate::layout;

const LV_EVENT_PRESSED: u32 = LvEventCode::LvEventPressed as u32;
const LV_EVENT_PRESS_LOST: u32 = LvEventCode::LvEventPressLost as u32;
const LV_EVENT_RELEASED: u32 = LvEventCode::LvEventReleased as u32;

/// Nominal key size in pixels used to compute the grid from the device's
/// reported `display_width` / `display_height`. 96 px gives 5 × 3 on the
/// 480 × 320 jc4827w543 panel; 800 × 480 yields 8 × 5, etc.
const KEY_PX: u32 = 72;

/// Height in pixels reserved for the default chrome's prev/next top
/// row (Stage 71). Subtracted from the panel height before computing
/// how many key rows fit, since the OpenDeck page renders *below* the
/// chrome.
const TOP_ROW_HEIGHT: u32 = 32;

/// OpenDeck device-type byte (see `docs/opendeck-device-plugin.md`).
/// `0` (StreamDeck Original 3×5) is the closest match for a flat
/// LCD touch grid with no rotary encoders.
const DEVICE_TYPE: u8 = 0;

/// Per-attached-device state.
struct DeviceCtx {
	pad: Arc<Touchy>,
	device_id: String,
	cols: u8,
	rows: u8,
	/// Content-addressed image cache (Stage 85): each distinct icon's
	/// bytes cross the wire once; repaints become a cheap in-place
	/// image-slot swap.
	cache: ImageCache,
	/// Keys the user is currently holding (Stage 86). Updated by the
	/// event-forwarding task on PRESSED / RELEASED edges *before* the
	/// corresponding key_down/key_up is dispatched to OpenDeck, so the
	/// `set_image` that OpenDeck sends back in response observes the
	/// correct press state and we repaint the matching image slot.
	pressed_keys: Arc<Mutex<HashSet<u8>>>,
	/// Last image asset path written to each (key, pressed-slot), so a
	/// repaint with unchanged content skips re-issuing the action.
	last_slot_path: Mutex<HashMap<(u8, bool), String>>,
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
	/// Attached devices keyed by their *transport-level* discovery key
	/// (USB bus/addr or sim host:port — see
	/// [`DiscoveredDevice::describe`]). The OpenDeck-facing device id
	/// (derived from the hardware serial) lives in each
	/// [`DeviceCtx::device_id`].
	handles: Arc<RwLock<HashMap<String, Arc<DeviceCtx>>>>,
}

impl TouchyPlugin {
	pub fn new() -> Self {
		Self::default()
	}

	/// Background hot-plug loop. Polls every second and diffs against
	/// the set of devices we've already attached.
	pub async fn run_hotplug_loop(self: Arc<Self>) {
		log::info!("hot-plug watcher started — polling via touchy_pad::discover() every 1s");
		let mut interval = tokio::time::interval(Duration::from_secs(1));
		interval.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
		// Track the previous candidate count so we only log the
		// "found N" / "none found" line when it actually changes —
		// otherwise a once-per-second poll would flood the log.
		let mut last_count: Option<usize> = None;
		loop {
			interval.tick().await;
			let candidates = match discover().await {
				Ok(v) => v,
				Err(e) => {
					log::info!("discover failed: {e:#}");
					last_count = None;
					continue;
				}
			};
			if last_count != Some(candidates.len()) {
				if candidates.is_empty() {
					log::info!("no Touchy-Pad devices found");
				} else {
					log::info!("found {} Touchy-Pad candidate(s)", candidates.len());
					for cand in &candidates {
						log::info!("  candidate {}", cand.describe());
					}
				}
				last_count = Some(candidates.len());
			}
			let mut seen = Vec::with_capacity(candidates.len());
			for cand in &candidates {
				// Dedup on the transport-level key (USB bus/addr or
				// sim host:port); the OpenDeck-facing device id is
				// derived from the hardware serial inside `attach`.
				let key = cand.describe();
				seen.push(key.clone());
				if !self.handles.read().await.contains_key(&key) {
					log::info!("new device {key} — attaching");
					let attach_start = std::time::Instant::now();
					if let Err(e) = self.attach(cand, &key).await {
						// A failed attach drops its transport, but the
						// device may still be draining this attempt's
						// queued writes. The next poll re-attaches and
						// its file_open_write can then collide with that
						// leftover transaction ("stale transaction" on
						// the device). Log the timing so the race is
						// visible in the combined log.
						log::warn!(
							"attach {key} failed after {:?}: {e:#} — device may still be \
							 draining in-flight writes; next poll will retry",
							attach_start.elapsed()
						);
					}
				}
			}
			// Detach gone devices.
			let to_drop: Vec<String> = {
				let map = self.handles.read().await;
				map.keys().filter(|k| !seen.contains(k)).cloned().collect()
			};
			for id in to_drop {
				log::info!("device {id} disappeared — detaching");
				self.detach(&id).await;
			}
		}
	}

	async fn attach(&self, disc: &DiscoveredDevice, key: &str) -> Result<()> {
		log::info!("attach {key}: opening transport");
		let transport = disc.open().await.context("open transport")?;
		let pad = Arc::new(Touchy::from_transport(transport));

		log::info!("attach {key}: querying board info");
		let board = pad.client().sys_board_info_get().await.context("sys_board_info_get")?;
		// Stage 71: the OpenDeck-facing device id is derived from the
		// hardware serial, so it is stable across ports / re-enumeration.
		let device_id = layout::device_id_for(&board.serial);
		log::info!(
			"attach {key}: serial '{}' -> {device_id}; board '{}', display {}×{}, protocol v{}, multitouch={}, usb={}",
			board.serial,
			board.board_name,
			board.display_width,
			board.display_height,
			board.protocol_version,
			board.is_multitouch,
			board.has_usb,
		);
		if board.display_width == 0 || board.display_height == 0 {
			return Err(anyhow!("{device_id} reported zero display dimensions ({}×{})", board.display_width, board.display_height));
		}
		// Match the device's log threshold to the host log filter so a
		// `TOUCHY_LOG=debug` run actually receives DEBUG device records.
		// Device-side they're dropped below `min_log_level` (Stage 82,
		// default ERROR), so without this a debug host filter still sees
		// no device DEBUG lines.
		let dev_level = match log::max_level() {
			log::LevelFilter::Trace => LogPriority::Trace,
			log::LevelFilter::Debug => LogPriority::Debug,
			log::LevelFilter::Info => LogPriority::Info,
			log::LevelFilter::Warn => LogPriority::Warn,
			log::LevelFilter::Error | log::LevelFilter::Off => LogPriority::Error,
		};
		match pad.client().set_min_log_level(dev_level).await {
			Ok(()) => log::info!("attach {device_id}: device min log level set to {dev_level:?}"),
			Err(e) => log::warn!("attach {device_id}: set_min_log_level({dev_level:?}) failed: {e:#}"),
		}
		let cols = u8::try_from(board.display_width / KEY_PX).context("cols overflow")?;
		// The OpenDeck page renders below the chrome's prev/next top
		// row, so subtract that band before sizing the key grid.
		let usable_h = board.display_height.saturating_sub(TOP_ROW_HEIGHT);
		let rows = u8::try_from(usable_h / KEY_PX).context("rows overflow")?;
		if cols == 0 || rows == 0 {
			return Err(anyhow!("{device_id} usable area {}×{} is smaller than one key ({KEY_PX} px)", board.display_width, usable_h));
		}
		log::info!("attach {device_id}: grid {cols}×{rows} ({} keys, {KEY_PX}px each)", cols as u32 * rows as u32);

		// Stage 85/86: build a content-addressed image cache (icons
		// cross the wire once) downscaled to the key size, and seed it
		// with the blank placeholder so every key has a valid released
		// image before any artwork arrives. Each grid cell is a per-key
		// `ImageButton` whose released image starts at that blank path;
		// repaints swap the image slot in place (no widget rebuild), so
		// a key the user is mid-press on keeps its touch state and still
		// emits a release event.
		let cache = ImageCache::with_max_dim(pad.clone(), Some(KEY_PX));
		let blank_path = cache.set_cached_image(layout::BLANK_BIN).await.context("seed blank image")?;
		log::info!("attach {device_id}: seeded blank image at {blank_path}");

		let page = layout::build_page(cols, rows, &blank_path);
		log::info!("attach {device_id}: uploading page body '{}'", layout::PAGE_NAME);
		pad.user_screen_save(layout::PAGE_NAME, &page).await.context("user_screen_save")?;
		pad.show_user_screen(layout::PAGE_NAME).await.context("show_user_screen")?;

		let pressed_keys: Arc<Mutex<HashSet<u8>>> = Arc::new(Mutex::new(HashSet::new()));

		// Take the event receiver before we hand the pad off.
		let mut rx = pad.events().await.ok_or_else(|| anyhow!("events() already taken"))?;
		log::info!("attach {device_id}: spawning event-forwarding task");
		let id_for_task = device_id.clone();
		let pressed_for_task = pressed_keys.clone();
		let event_task = tokio::spawn(async move {
			while let Some(evt) = rx.recv().await {
				if let Some(state) = &evt.state
					&& let lv_event::State::Value(_) | lv_event::State::Checked(_) = state
				{
					// Slider/toggle events — not applicable to a key grid.
				}
				let Some(key) = layout::key_for_host_code(evt.host_code) else {
					log::debug!("{id_for_task}: ignoring event host_code={:#x} code={} (outside key range)", evt.host_code, evt.code);
					continue;
				};
				let res = match evt.code {
					LV_EVENT_PRESSED => {
						// Record the press *before* notifying OpenDeck so
						// the set_image it sends back targets the pressed
						// image slot (Stage 86).
						pressed_for_task.lock().await.insert(key);
						log::info!("{id_for_task}: key {key} down -> keyDown");
						device_plugin::key_down(id_for_task.clone(), key).await
					}
					LV_EVENT_RELEASED | LV_EVENT_PRESS_LOST => {
						// Clear the press *before* notifying OpenDeck so a
						// restore set_image targets the released slot.
						pressed_for_task.lock().await.remove(&key);
						log::info!("{id_for_task}: key {key} up -> keyUp");
						device_plugin::key_up(id_for_task.clone(), key).await
					}
					other => {
						log::debug!("{id_for_task}: key {key} ignored event code {other}");
						continue;
					}
				};
				if let Err(e) = res {
					log::warn!("dispatch event for {id_for_task}/key{key}: {e:?}");
				}
			}
			log::info!("event stream for {id_for_task} ended");
		});

		let ctx = Arc::new(DeviceCtx {
			pad,
			device_id: device_id.clone(),
			cols,
			rows,
			cache,
			pressed_keys,
			last_slot_path: Mutex::new(HashMap::new()),
			event_task: Mutex::new(Some(event_task)),
		});

		self.handles.write().await.insert(key.to_string(), ctx.clone());

		// Tell OpenDeck about the new device. NOTE: openaction's
		// `register_device` silently no-ops (returns Ok) if the outbound
		// WebSocket manager isn't live yet — but by the time the
		// hot-plug loop runs (spawned from `plugin_ready`) the manager is
		// always set, so a successful return here does mean the
		// `registerDevice` event hit the socket.
		log::info!("attach {device_id}: sending registerDevice (name='{}', {cols}×{rows}, type {DEVICE_TYPE})", ctx.name());
		device_plugin::register_device(device_id.clone(), ctx.name(), rows, cols, /* encoders */ 0, DEVICE_TYPE)
			.await
			.map_err(|e| anyhow!("register_device: {e:?}"))?;
		log::info!("attach {device_id}: registered OK as {cols}×{rows}");
		Ok(())
	}

	async fn detach(&self, key: &str) {
		log::info!("detach {key}: tearing down");
		let ctx = self.handles.write().await.remove(key);
		if let Some(ctx) = ctx {
			let device_id = ctx.device_id.clone();
			if let Some(h) = ctx.event_task.lock().await.take() {
				log::info!("detach {device_id}: cancelling event task");
				h.abort();
			}
			ctx.pad.close().await;
			log::info!("detach {device_id}: sending deregisterDevice");
			if let Err(e) = device_plugin::unregister_device(device_id.clone()).await {
				log::warn!("unregister_device {device_id}: {e:?}");
			}
			log::info!("detach {device_id}: done");
		}
	}

	/// Look up an attached device by its OpenDeck-facing device id
	/// (`tp-<serial>`), as carried on inbound `set_image` /
	/// `set_brightness` events.
	async fn ctx_for(&self, device_id: &str) -> Option<Arc<DeviceCtx>> {
		self.handles.read().await.values().find(|c| c.device_id == device_id).cloned()
	}

	/// Paint `key` with `data` (any supported image, or
	/// [`layout::BLANK_BIN`] to clear). The image bytes are deduped
	/// through the cache (uploaded at most once), then the matching
	/// image *slot* — pressed while the user is holding the key,
	/// released otherwise — is repointed in place. Skips re-issuing the
	/// action when that slot already shows this asset.
	async fn set_key_image(&self, ctx: &DeviceCtx, key: u8, data: &[u8]) -> Result<()> {
		let path = ctx.cache.set_cached_image(data).await.context("cache image")?;
		let pressed = ctx.pressed_keys.lock().await.contains(&key);
		let mut last = ctx.last_slot_path.lock().await;
		if last.get(&(key, pressed)).is_some_and(|p| p == &path) {
			return Ok(());
		}
		ctx.pad.set_image_button_slot(&layout::key_widget_id(key), pressed, &path).await.context("set image slot")?;
		last.insert((key, pressed), path);
		Ok(())
	}

	async fn handle_set_image(&self, ev: SetImageEvent) -> Result<()> {
		log::info!("set_image: device={} position={:?} has_image={}", ev.device, ev.position, ev.image.is_some());
		let Some(ctx) = self.ctx_for(&ev.device).await else {
			log::info!("set_image for unknown device {} — ignored", ev.device);
			return Ok(());
		};
		let key_count = (ctx.cols as usize) * (ctx.rows as usize);
		match (ev.position, ev.image.as_deref()) {
			(Some(pos), Some(data_url)) => {
				if pos as usize >= key_count {
					log::debug!("set_image position {pos} out of range for {}", ctx.device_id);
					return Ok(());
				}
				let b64 = data_url.split_once(',').map(|(_, b)| b).unwrap_or(data_url);
				let bytes = B64.decode(b64.trim()).context("base64 decode")?;
				self.set_key_image(&ctx, pos, &bytes).await?;
			}
			(Some(pos), None) => {
				if pos as usize >= key_count {
					return Ok(());
				}
				// Clear a single key — repaint it with the blank image.
				self.set_key_image(&ctx, pos, layout::BLANK_BIN).await?;
			}
			(None, None) => {
				// Clear all keys.
				for k in 0..(ctx.cols * ctx.rows) {
					if let Err(e) = self.set_key_image(&ctx, k, layout::BLANK_BIN).await {
						log::warn!("clear key {k} on {}: {e:#}", ctx.device_id);
					}
				}
			}
			(None, Some(_)) => {
				log::debug!("set_image with image but no position — ignored");
			}
		}
		Ok(())
	}

	async fn handle_set_brightness(&self, ev: SetBrightnessEvent) -> Result<()> {
		log::info!("set_brightness: device={} brightness={}", ev.device, ev.brightness);
		let Some(ctx) = self.ctx_for(&ev.device).await else {
			log::info!("set_brightness for unknown device {} — ignored", ev.device);
			return Ok(());
		};
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
		log::info!("plugin_ready — OpenDeck outbound link is live; starting hot-plug watcher");
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
