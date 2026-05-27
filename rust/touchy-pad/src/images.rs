//! Convert standard image formats (PNG / JPEG / BMP / GIF / WebP) to
//! LVGL 9's native `.bin` binary format.
//!
//! The firmware always supports LVGL's built-in image decoder, which
//! reads the 12-byte `lv_image_header_t` followed by raw pixel data
//! from any `F:` filesystem path. To avoid depending on optional
//! BMP/PNG/JPEG decoders on-device, the host transparently converts
//! every uploaded image to this native format.
//!
//! Output color format is **RGB565** by default — the firmware is built
//! with `CONFIG_LV_COLOR_DEPTH_16` and RGB565 matches the panel's
//! native format, letting the device take the Stage 52 zero-copy mmap
//! fast path on `R:` (PSRAM) uploads. We fall back to **RGB565A8**
//! only when the source actually contains non-opaque alpha.
//!
//! This is the Rust port of `app/src/touchy_pad/api/lvgl_image.py`.

use image::DynamicImage;

use crate::error::{Result, TouchyError};

/// First byte of an LVGL 9 native `.bin` header. See
/// `LVGLImageHeader.binary` in the upstream `LVGLImage.py` script.
pub const LVGL_BIN_MAGIC: u8 = 0x19;

// `enum lv_color_format_t` values from LVGL 9 (`lv_color_format_t` in
// `lvgl/src/misc/lv_color.h`).
const CF_RGB565: u8 = 0x12;
const CF_RGB565A8: u8 = 0x14;

/// Magic-byte prefixes for the image formats we recognise as "an image
/// we should convert" rather than "a file the user wants to upload
/// verbatim".
const IMAGE_MAGICS: &[&[u8]] = &[
	&[0x89, b'P', b'N', b'G', b'\r', b'\n', 0x1a, b'\n'], // PNG
	&[0xff, 0xd8, 0xff],                                  // JPEG
	&[b'B', b'M'],                                        // BMP
	&[b'G', b'I', b'F', b'8', b'7', b'a'],                // GIF87a
	&[b'G', b'I', b'F', b'8', b'9', b'a'],                // GIF89a
	&[b'R', b'I', b'F', b'F'],                            // WebP (RIFF…WEBP)
];

/// Convertible image file extensions (matched case-insensitively).
const CONVERTIBLE_EXTS: &[&str] = &[".bmp", ".png", ".jpg", ".jpeg", ".gif", ".webp"];

/// Return `true` if `data` already looks like an LVGL `.bin`.
pub fn is_lvgl_bin(data: &[u8]) -> bool {
	data.len() >= 12 && data[0] == LVGL_BIN_MAGIC
}

/// Return `true` if `data` looks like a PNG / JPEG / BMP / GIF / WebP
/// file we know how to convert.
pub fn looks_like_supported_image(data: &[u8]) -> bool {
	if is_lvgl_bin(data) {
		return false;
	}
	IMAGE_MAGICS.iter().any(|m| data.starts_with(m))
}

/// Replace a recognised image extension with `.bin`. LVGL 9's bin
/// decoder selects itself by file extension, so a converted file must
/// also be *named* `*.bin` on-device.
pub fn rewrite_to_bin_path(path: &str) -> String {
	let lower = path.to_ascii_lowercase();
	for ext in CONVERTIBLE_EXTS {
		if lower.ends_with(ext) {
			let cut = path.len() - ext.len();
			return format!("{}.bin", &path[..cut]);
		}
	}
	path.to_string()
}

/// Color format choice for [`to_lvgl_bin`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum LvFormat {
	/// Auto-pick: emit `Rgb565` for opaque images, fall back to
	/// `Rgb565A8` when the source has non-opaque alpha.
	#[default]
	Auto,
	/// Force opaque RGB565 (alpha is dropped).
	Rgb565,
	/// Force RGB565 + A8 plane.
	Rgb565A8,
}

/// Convert `source` image bytes to an LVGL 9 `.bin` blob.
///
/// `source` may be PNG, JPEG, BMP, GIF, or WebP — anything the `image`
/// crate's enabled features can decode.
pub fn to_lvgl_bin(source: &[u8], format: LvFormat) -> Result<Vec<u8>> {
	let img = image::load_from_memory(source)?;
	let chosen = match format {
		LvFormat::Auto => {
			if has_non_opaque_alpha(&img) {
				log::warn!("image has non-opaque alpha ({}x{}); falling back to RGB565A8 (mmap fast path unavailable)", img.width(), img.height());
				LvFormat::Rgb565A8
			} else {
				LvFormat::Rgb565
			}
		}
		other => other,
	};
	match chosen {
		LvFormat::Rgb565 => {
			let (w, h, pixels) = to_rgb565(&img);
			Ok(build_bin(CF_RGB565, w, h, w * 2, &pixels))
		}
		LvFormat::Rgb565A8 => {
			let (w, h, pixels) = to_rgb565a8(&img);
			// Stride covers the RGB565 plane only; the A8 plane is
			// implicitly the same width.
			Ok(build_bin(CF_RGB565A8, w, h, w * 2, &pixels))
		}
		LvFormat::Auto => unreachable!(),
	}
}

fn build_bin(cf: u8, w: u32, h: u32, stride: u32, pixels: &[u8]) -> Vec<u8> {
	// 12-byte v9 image header. Layout: `<BBHHHHH` = magic, cf, flags,
	// w, h, stride, reserved (little-endian).
	let mut out = Vec::with_capacity(12 + pixels.len());
	out.push(LVGL_BIN_MAGIC);
	out.push(cf & 0x1f);
	out.extend_from_slice(&0u16.to_le_bytes()); // flags
	out.extend_from_slice(&(w as u16).to_le_bytes());
	out.extend_from_slice(&(h as u16).to_le_bytes());
	out.extend_from_slice(&(stride as u16).to_le_bytes());
	out.extend_from_slice(&0u16.to_le_bytes()); // reserved
	out.extend_from_slice(pixels);
	out
}

fn rgb565_word(r: u8, g: u8, b: u8) -> u16 {
	((r as u16 & 0xf8) << 8) | ((g as u16 & 0xfc) << 3) | ((b as u16) >> 3)
}

fn to_rgb565(img: &DynamicImage) -> (u32, u32, Vec<u8>) {
	let rgb = img.to_rgb8();
	let (w, h) = rgb.dimensions();
	let mut out = Vec::with_capacity((w * h * 2) as usize);
	for px in rgb.pixels() {
		let [r, g, b] = px.0;
		out.extend_from_slice(&rgb565_word(r, g, b).to_le_bytes());
	}
	(w, h, out)
}

fn to_rgb565a8(img: &DynamicImage) -> (u32, u32, Vec<u8>) {
	let rgba = img.to_rgba8();
	let (w, h) = rgba.dimensions();
	let n = (w * h) as usize;
	let mut rgb_plane = Vec::with_capacity(n * 2);
	let mut a_plane = Vec::with_capacity(n);
	for px in rgba.pixels() {
		let [r, g, b, a] = px.0;
		rgb_plane.extend_from_slice(&rgb565_word(r, g, b).to_le_bytes());
		a_plane.push(a);
	}
	rgb_plane.extend_from_slice(&a_plane);
	(w, h, rgb_plane)
}

fn has_non_opaque_alpha(img: &DynamicImage) -> bool {
	if !img.color().has_alpha() {
		return false;
	}
	let rgba = img.to_rgba8();
	rgba.pixels().any(|p| p.0[3] < 255)
}

/// Silences the unused-import warning when no feature uses [`TouchyError`].
#[allow(dead_code)]
fn _touchy_error_used(e: TouchyError) -> TouchyError {
	e
}
