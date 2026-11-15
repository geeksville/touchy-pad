//! Upload images once, then point `ImageButton`s at the cached on-device paths
//! — and repaint a single key in place without re-uploading the screen.
//!
//! ```sh
//! cargo run --example image_cache
//! ```

use std::io::Cursor;
use std::sync::Arc;

use touchy_pad::proto::{Action, ActionHost, Image, ImageButton, Layout, LayoutAbsolute, Rect, Widget, action, widget};
use touchy_pad::{ImageCache, Touchy};

/// A solid-colour PNG, so the example needs no binary assets on disk.
fn solid_png(rgb: [u8; 3]) -> Vec<u8> {
	let mut img = ::image::RgbImage::new(64, 64);
	for px in img.pixels_mut() {
		*px = ::image::Rgb(rgb);
	}
	let mut out = Vec::new();
	::image::DynamicImage::ImageRgb8(img)
		.write_to(&mut Cursor::new(&mut out), ::image::ImageFormat::Png)
		.expect("encode png");
	out
}

fn key(id: &str, x: i32, path: &str, code: u32) -> Widget {
	let action = Action {
		kind: Some(action::Kind::Host(ActionHost { code })),
	};
	Widget {
		id: id.into(),
		placement: Some(widget::Placement::Rect(Rect { x, y: 16, w: 64, h: 64 })),
		kind: Some(widget::Kind::ImageButton(ImageButton {
			released: Some(Image {
				path: path.into(),
				..Default::default()
			}),
			on_press: vec![action.clone()],
			on_release: vec![action],
			..Default::default()
		})),
		..Default::default()
	}
}

#[tokio::main]
async fn main() -> touchy_pad::Result<()> {
	env_logger::init();
	// `ImageCache` needs a shared handle so it can outlive this scope.
	let pad = Arc::new(Touchy::open().await?);
	// `with_max_dim` downscales before upload so a fixed-size key never makes
	// the device rescale per frame. `ImageCache::new` uploads at source size.
	let cache = ImageCache::with_max_dim(pad.clone(), Some(64));

	// PNG/JPEG/BMP/GIF/WebP in; LVGL `.bin` (on the device's transient `T:`
	// drive) out. Identical bytes hit the cache and upload nothing.
	let red = cache.set_cached_image(&solid_png([200, 50, 50])).await?;
	let blue = cache.set_cached_image(&solid_png([50, 50, 200])).await?;
	println!("cached {red} and {blue} ({} resident)", cache.len().await);

	let root = Widget {
		id: "root".into(),
		version: widget::Version::Current as i32,
		kind: Some(widget::Kind::LayoutAbsolute(LayoutAbsolute {
			layout: Some(Layout {
				children: vec![key("key_0", 16, &red, 0), key("key_1", 96, &blue, 1)],
			}),
		})),
		..Default::default()
	};
	pad.user_screen_save("rust_images", &root).await?;
	pad.screen_load(touchy_pad::DEFAULT_SCREEN_PATH).await?;
	pad.show_user_screen("rust_images").await?;

	// Repaint one key in place (Stage 86): swaps just that image slot via the
	// device's image registry — no screen re-upload, and if the user is pressing
	// this button it keeps its touch state and still emits its release event.
	pad.set_image_button_slot("key_0", false, &blue).await?;
	println!("key_0 now shows the blue asset");
	Ok(())
}
