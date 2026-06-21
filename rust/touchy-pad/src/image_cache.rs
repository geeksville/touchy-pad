//! Content-addressed image cache (Stage 85).
//!
//! Sending image bytes over the USB / UART link is slow, and most
//! applications (StreamDeck-style key grids in particular) repaint the
//! same small set of icons over and over. [`ImageCache`] uploads each
//! distinct image to the device exactly **once**, keyed by a 128-bit
//! content hash, and hands back the on-device path so callers can point
//! a widget at it cheaply (e.g. via `ActionChangeWidgetRef`).
//!
//! The cache is **host-side and volatile**: the in-RAM map is never
//! serialized, and the device assets live on the `T:` transient drive
//! (a PSRAM ramdisk where available, else a flash scratch area — the
//! device decides; see `SysBoardInfoResponse.temp_is_flash`). Build a
//! fresh [`ImageCache`] whenever you (re)attach to a device — the first
//! [`ImageCache::set_cached_image`] call clears [`IMAGE_CACHE_ROOT`] on
//! the device so a crashed prior session leaves no stale files behind.
//!
//! Eviction is least-recently-used: once [`MAX_IMAGE_CACHE`] distinct
//! images are resident, the next miss deletes the least-recently-used
//! asset before uploading the new one.

use std::collections::HashMap;
use std::sync::Arc;

use base64::Engine as _;
use base64::engine::general_purpose::URL_SAFE_NO_PAD as B64URL;
use tokio::sync::Mutex;
use xxhash_rust::xxh3::xxh3_128;

use crate::error::Result;
use crate::images::normalize_for_device;
use crate::pad::Touchy;

/// On-device directory holding cached image assets. Lives on the `T:`
/// transient drive (PSRAM ramdisk where available, else a flash scratch
/// area) — wiped on device reboot.
pub const IMAGE_CACHE_ROOT: &str = "T:host/icache/";

/// Maximum number of distinct images kept resident on the device
/// before the least-recently-used one is evicted.
pub const MAX_IMAGE_CACHE: usize = 128;

/// Internal mutable state, guarded by a single async mutex so cache
/// operations (and their device I/O) are serialized.
struct Inner {
	/// Whether the one-time [`IMAGE_CACHE_ROOT`] wipe has run.
	initialized: bool,
	/// Recency order, front = least-recently-used, back = most-recent.
	order: Vec<u128>,
	/// `hash -> on-device asset path`.
	map: HashMap<u128, String>,
}

impl Inner {
	/// Move `hash` to the most-recently-used end of [`Inner::order`].
	fn touch(&mut self, hash: u128) {
		if let Some(pos) = self.order.iter().position(|h| *h == hash) {
			self.order.remove(pos);
		}
		self.order.push(hash);
	}
}

/// Host-side, content-addressed image cache for one device.
///
/// See the [module docs][crate::image_cache] for the model. Construct
/// one per attached device with [`ImageCache::new`].
pub struct ImageCache {
	pad: Arc<Touchy>,
	/// When `Some`, images are downscaled so neither dimension exceeds
	/// this before being encoded to a device `.bin`. Matches the
	/// on-screen widget size so the device never rescales per frame.
	max_dim: Option<u32>,
	inner: Mutex<Inner>,
}

impl ImageCache {
	/// Build a cache that uploads to `pad`. Cheap — no device I/O
	/// happens until the first [`set_cached_image`][Self::set_cached_image].
	///
	/// Uploaded images are not rescaled; see [`ImageCache::with_max_dim`]
	/// to cap their size for small fixed-size widgets.
	pub fn new(pad: Arc<Touchy>) -> Self {
		Self::with_max_dim(pad, None)
	}

	/// Like [`ImageCache::new`] but downscales every cached image so
	/// neither dimension exceeds `max_dim` pixels (aspect ratio
	/// preserved). Use this when the images back a fixed-size widget
	/// (e.g. an OpenDeck key) so the device stores — and blits — them at
	/// display size instead of rescaling an oversized source every frame.
	pub fn with_max_dim(pad: Arc<Touchy>, max_dim: Option<u32>) -> Self {
		Self {
			pad,
			max_dim,
			inner: Mutex::new(Inner {
				initialized: false,
				order: Vec::new(),
				map: HashMap::new(),
			}),
		}
	}

	/// Ensure `data` is resident in the device cache and return the
	/// drive-prefixed path to its asset file (an LVGL `.bin`, or the
	/// original format passed through when the transport doesn't need
	/// conversion). Suitable as the `released.path` of an `ImageButton`.
	///
	/// `data` may be PNG / JPEG / BMP / GIF / WebP or an already-encoded
	/// LVGL `.bin`. Bytes are normalised exactly as
	/// [`Touchy::file_save`][crate::Touchy::file_save] would, then hashed
	/// (xxh3-128); identical inputs hit the cache and upload nothing.
	pub async fn set_cached_image(&self, data: &[u8]) -> Result<String> {
		let needs_conversion = self.pad.client().transport().needs_image_conversion();
		let (bytes, suffix) = normalize_for_device(data, needs_conversion, self.max_dim)?;
		let hash = xxh3_128(&bytes);

		let mut inner = self.inner.lock().await;

		// One-time wipe of any stale assets from a prior host session.
		if !inner.initialized {
			// Ignore the error: the directory may simply not exist yet.
			let _ = self.pad.client().file_delete(IMAGE_CACHE_ROOT).await;
			inner.initialized = true;
		}

		// Cache hit — no device I/O.
		if let Some(path) = inner.map.get(&hash).cloned() {
			inner.touch(hash);
			log::debug!("image cache hit {} -> {path}", hash_name(hash));
			return Ok(path);
		}

		// Miss — evict LRU if at capacity.
		if inner.map.len() >= MAX_IMAGE_CACHE && !inner.order.is_empty() {
			let victim = inner.order.remove(0);
			if let Some(old_path) = inner.map.remove(&victim) {
				log::debug!("image cache evict {} ({old_path})", hash_name(victim));
				let _ = self.pad.client().file_delete(&old_path).await;
			}
		}

		// Upload the (already-normalised) bytes verbatim.
		let path = format!("{IMAGE_CACHE_ROOT}{}{suffix}", hash_name(hash));
		log::debug!("image cache miss {} -> uploading {path} ({} bytes)", hash_name(hash), bytes.len());
		self.pad.file_write_raw(&path, &bytes).await?;

		inner.map.insert(hash, path.clone());
		inner.order.push(hash);
		Ok(path)
	}

	/// Number of images currently resident in the cache.
	pub async fn len(&self) -> usize {
		self.inner.lock().await.map.len()
	}

	/// Whether the cache is empty.
	pub async fn is_empty(&self) -> bool {
		self.inner.lock().await.map.is_empty()
	}
}

/// URL-safe, unpadded base64 of a 128-bit hash — the filename stem
/// (no extension) under [`IMAGE_CACHE_ROOT`].
fn hash_name(hash: u128) -> String {
	B64URL.encode(hash.to_be_bytes())
}
