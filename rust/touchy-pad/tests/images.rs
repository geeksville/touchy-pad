//! Smoke tests for the LVGL `.bin` encoder.

use touchy_pad::images::{LVGL_BIN_MAGIC, LvFormat, is_lvgl_bin, looks_like_supported_image, rewrite_to_bin_path, to_lvgl_bin};

fn tiny_png_rgb(w: u32, h: u32, r: u8, g: u8, b: u8) -> Vec<u8> {
	use std::io::Cursor;
	let mut img = image::RgbImage::new(w, h);
	for px in img.pixels_mut() {
		*px = image::Rgb([r, g, b]);
	}
	let mut buf = Vec::new();
	image::DynamicImage::ImageRgb8(img)
		.write_to(&mut Cursor::new(&mut buf), image::ImageFormat::Png)
		.unwrap();
	buf
}

fn tiny_png_rgba(w: u32, h: u32, r: u8, g: u8, b: u8, a: u8) -> Vec<u8> {
	use std::io::Cursor;
	let mut img = image::RgbaImage::new(w, h);
	for px in img.pixels_mut() {
		*px = image::Rgba([r, g, b, a]);
	}
	let mut buf = Vec::new();
	image::DynamicImage::ImageRgba8(img)
		.write_to(&mut Cursor::new(&mut buf), image::ImageFormat::Png)
		.unwrap();
	buf
}

#[test]
fn detects_png() {
	let png = tiny_png_rgb(4, 4, 255, 0, 0);
	assert!(looks_like_supported_image(&png));
	assert!(!is_lvgl_bin(&png));
}

#[test]
fn rewrite_extension() {
	assert_eq!(rewrite_to_bin_path("R:host/x.png"), "R:host/x.bin");
	assert_eq!(rewrite_to_bin_path("F:host/A.JPG"), "F:host/A.bin");
	assert_eq!(rewrite_to_bin_path("F:host/y.bin"), "F:host/y.bin");
}

#[test]
fn rgb_png_yields_rgb565_bin() {
	let png = tiny_png_rgb(8, 4, 255, 0, 0);
	let bin = to_lvgl_bin(&png, LvFormat::Auto).unwrap();
	assert!(is_lvgl_bin(&bin));
	assert_eq!(bin[0], LVGL_BIN_MAGIC);
	assert_eq!(bin[1] & 0x1f, 0x12, "auto chose RGB565");
	let w = u16::from_le_bytes([bin[4], bin[5]]);
	let h = u16::from_le_bytes([bin[6], bin[7]]);
	assert_eq!((w, h), (8, 4));
	assert_eq!(bin.len(), 12 + 8 * 4 * 2);
	// First red pixel: 0xF800 little-endian.
	assert_eq!(&bin[12..14], &[0x00, 0xf8]);
}

#[test]
fn rgba_png_with_alpha_yields_rgb565a8() {
	let png = tiny_png_rgba(4, 4, 0, 255, 0, 128);
	let bin = to_lvgl_bin(&png, LvFormat::Auto).unwrap();
	assert_eq!(bin[1] & 0x1f, 0x14, "auto chose RGB565A8");
	// 12-byte header + 4*4*2 RGB plane + 4*4 alpha plane.
	assert_eq!(bin.len(), 12 + 16 * 2 + 16);
	// Final alpha byte must be 128.
	assert_eq!(*bin.last().unwrap(), 128);
}
