//! `ImageCache` integration tests against an in-memory mock Transport
//! that auto-replies to the file-streaming RPCs (Stage 85).
//!
//! The mock decodes each `Command` in `send_command` and stashes it,
//! then synthesises the matching `Response` in `recv_response`. Both
//! calls complete synchronously (no `.await` that yields), so on the
//! single-threaded test runtime each `Client::rpc` send→recv pair is
//! effectively atomic and the background event poller (which only ever
//! sends `EventConsume`, answered with `NOT_FOUND`) cannot interleave.

use std::sync::Arc;
use std::sync::Mutex;
use std::time::Duration;

use async_trait::async_trait;
use prost::Message;
use touchy_pad::Touchy;
use touchy_pad::image_cache::{IMAGE_CACHE_ROOT, ImageCache, MAX_IMAGE_CACHE};
use touchy_pad::proto::{Command, FileOpenWriteResponse, Response, ResultCode, command, response};
use touchy_pad::transport::Transport;

/// Records the file-level side effects the cache performs so tests can
/// assert on uploads / deletes without a real device.
#[derive(Default)]
struct Recorder {
	/// Paths passed to `file_open_write`, in order.
	opened: Vec<String>,
	/// Paths passed to `file_delete`, in order (first entry is the
	/// one-time `IMAGE_CACHE_ROOT` wipe).
	deleted: Vec<String>,
	/// Monotonic write-handle source.
	next_handle: u32,
	/// The most recent command, set by `send_command` and consumed by
	/// `recv_response`.
	last: Option<Command>,
}

struct Mock {
	rec: Mutex<Recorder>,
}

impl Mock {
	fn new() -> Arc<Self> {
		Arc::new(Self { rec: Mutex::new(Recorder::default()) })
	}
}

#[async_trait]
impl Transport for Mock {
	async fn send_command(&self, payload: &[u8]) -> touchy_pad::Result<()> {
		let cmd = Command::decode(payload).expect("decode command");
		self.rec.lock().unwrap().last = Some(cmd);
		Ok(())
	}

	async fn recv_response(&self, _: Duration) -> touchy_pad::Result<Vec<u8>> {
		let mut rec = self.rec.lock().unwrap();
		let cmd = rec.last.take().expect("recv_response without a prior send");
		let resp = match cmd.cmd {
			Some(command::Cmd::EventConsume(_)) => Response {
				code: ResultCode::NotFound as i32,
				payload: None,
			},
			Some(command::Cmd::FileOpenWrite(c)) => {
				rec.opened.push(c.path);
				let handle = rec.next_handle;
				rec.next_handle += 1;
				Response {
					code: ResultCode::Ok as i32,
					payload: Some(response::Payload::FileOpenWrite(FileOpenWriteResponse { handle })),
				}
			}
			Some(command::Cmd::FileDelete(c)) => {
				rec.deleted.push(c.path);
				Response {
					code: ResultCode::Ok as i32,
					payload: None,
				}
			}
			// FileWrite, FileClose, and anything else: bare OK.
			_ => Response {
				code: ResultCode::Ok as i32,
				payload: None,
			},
		};
		let mut buf = Vec::with_capacity(resp.encoded_len());
		resp.encode(&mut buf).unwrap();
		Ok(buf)
	}

	// TCP/USB convert images to LVGL `.bin`; we want the raw bytes to
	// pass through (no PNG/JPEG decoding in the test) so report `false`.
	fn needs_image_conversion(&self) -> bool {
		false
	}
}

fn cache_for(mock: Arc<Mock>) -> (Arc<Touchy>, ImageCache) {
	let pad = Arc::new(Touchy::from_transport(mock as Arc<dyn Transport>));
	let cache = ImageCache::new(pad.clone());
	(pad, cache)
}

#[tokio::test]
async fn miss_uploads_then_identical_hits() {
	let mock = Mock::new();
	let (pad, cache) = cache_for(mock.clone());

	// First image — a miss. Non-image bytes ⇒ pass-through ⇒ `.bin`.
	let p1 = cache.set_cached_image(b"alpha-image-bytes").await.unwrap();
	assert!(p1.starts_with(IMAGE_CACHE_ROOT), "path under cache root: {p1}");
	assert!(p1.ends_with(".bin"), "non-image bytes get a .bin suffix: {p1}");
	assert_eq!(cache.len().await, 1);

	{
		let rec = mock.rec.lock().unwrap();
		// The one-time wipe deletes the cache root first.
		assert_eq!(rec.deleted.first().map(String::as_str), Some(IMAGE_CACHE_ROOT));
		// Exactly one asset uploaded so far.
		assert_eq!(rec.opened, vec![p1.clone()]);
	}

	// Same bytes again — a hit. Same path, no new upload.
	let p2 = cache.set_cached_image(b"alpha-image-bytes").await.unwrap();
	assert_eq!(p1, p2);
	assert_eq!(cache.len().await, 1);
	assert_eq!(mock.rec.lock().unwrap().opened.len(), 1, "hit must not upload");

	// Different bytes — a second miss ⇒ second upload, different path.
	let p3 = cache.set_cached_image(b"beta-image-bytes").await.unwrap();
	assert_ne!(p1, p3);
	assert_eq!(cache.len().await, 2);
	assert_eq!(mock.rec.lock().unwrap().opened.len(), 2);

	pad.close().await;
}

#[tokio::test]
async fn eviction_is_least_recently_used() {
	let mock = Mock::new();
	let (pad, cache) = cache_for(mock.clone());

	// Fill the cache to capacity with distinct images.
	let mut first_path = String::new();
	for i in 0..MAX_IMAGE_CACHE {
		let p = cache.set_cached_image(format!("img-{i}").as_bytes()).await.unwrap();
		if i == 0 {
			first_path = p;
		}
	}
	assert_eq!(cache.len().await, MAX_IMAGE_CACHE);

	// One more distinct image overflows: the LRU (the very first one,
	// never re-touched) is evicted via file_delete, and the cache stays
	// pinned at capacity.
	let overflow = cache.set_cached_image(b"one-too-many").await.unwrap();
	assert_eq!(cache.len().await, MAX_IMAGE_CACHE);

	{
		let rec = mock.rec.lock().unwrap();
		assert!(
			rec.deleted.iter().any(|d| d == &first_path),
			"LRU asset {first_path} should have been deleted; deletes={:?}",
			rec.deleted
		);
		// The overflow asset was uploaded.
		assert!(rec.opened.iter().any(|o| o == &overflow), "overflow asset should have been uploaded");
	}

	pad.close().await;
}
