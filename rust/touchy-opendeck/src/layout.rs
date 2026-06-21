//! Screen-layout helpers for the OpenDeck device plugin.
//!
//! Each OpenDeck "key" maps 1:1 to an LVGL ``ImageButton`` widget on a
//! Touchy-Pad screen. The widget's ``on_press`` and ``on_release``
//! action slots both fire the same ``ActionHost`` host code; the host
//! distinguishes press from release by inspecting ``LvEvent.code``.
//!
//! Conventions chosen to coexist with TouchyDeck (the Python
//! StreamDeck-compat shim, ``HOST_CODE_BASE = 0xA000``): this crate
//! uses ``0xB000`` so a single device can host both layouts.

use touchy_pad::proto::{Action, ActionHost, GridCell, Image, ImageButton, Layout, LayoutGrid, Style, Widget, action, image, widget};

/// Two-character OpenDeck device-namespace prefix used by this plugin.
///
/// All device IDs we ``register_device`` must start with this prefix —
/// OpenDeck uses it to route inbound events back to us.
pub const NAMESPACE: &str = "tp";

/// Bare stem of the user-screen page body this plugin uploads.
///
/// Pushed to ``F:host/uscr/opendeck.pb`` via
/// [`Touchy::user_screen_save`][touchy_pad::Touchy::user_screen_save];
/// the default chrome's ``widget_ref(id="page")`` displays it. A single
/// shared page is enough because OpenDeck exposes one Touchy-Pad device.
pub const PAGE_NAME: &str = "opendeck";

/// Lowest ``ActionHost.code`` assigned to OpenDeck key 0.
///
/// Key ``k`` is wired to ``HOST_CODE_BASE + k``. The range
/// ``0xB000..=0xBFFF`` is reserved by this plugin; the Python
/// TouchyDeck shim uses ``0xA000..=0xAFFF``.
pub const HOST_CODE_BASE: u32 = 0xB000;

/// Maximum key index we'll map (keeps host codes well within
/// ``HOST_CODE_BASE..=0xBFFF``).
pub const MAX_KEYS: u32 = 0x1000;

/// Map an OpenDeck key index to its host-code.
pub fn host_code_for(key: u8) -> u32 {
	HOST_CODE_BASE + key as u32
}

/// Inverse of [`host_code_for`]. Returns ``None`` if ``code`` is
/// outside this plugin's allocated range.
pub fn key_for_host_code(code: u32) -> Option<u8> {
	if code < HOST_CODE_BASE {
		return None;
	}
	let k = code - HOST_CODE_BASE;
	if k >= MAX_KEYS {
		return None;
	}
	u8::try_from(k).ok()
}

/// Stable, plugin-namespaced device ID derived from the device serial.
///
/// The OpenDeck protocol requires the ID to start with [`NAMESPACE`]
/// and to be stable across reconnects. Stage 71 surfaces a real
/// hardware serial (`SysBoardInfoResponse.serial`), so the ID is
/// ``tp-<serial>`` — stable across ports and re-enumerations.
pub fn device_id_for(serial: &str) -> String {
	format!("{NAMESPACE}-{serial}")
}

/// Stable per-key `ImageButton` widget id.
///
/// The OpenDeck plugin builds one `ImageButton` per grid cell with this
/// id, then repaints it in place via
/// [`Touchy::set_image_button_slot`][touchy_pad::Touchy::set_image_button_slot]
/// (`target_id = key_widget_id(k)`). The id also routes touch events:
/// `ActionHost` reports the clickable widget's own id, so the per-key id
/// is what carries this key's host code back to the host.
pub fn key_widget_id(key: u8) -> String {
	format!("opendeck_key_{key}")
}

/// A minimal opaque “blank key” LVGL `.bin`: a 1×1 dark-grey
/// (`0x101010`) RGB565 pixel, stretched to fill the cell. Seeded into
/// the image cache at attach so every key has a valid released image
/// before any artwork arrives, and reused to clear a key. RGB565
/// (native) so it takes the device's zero-copy mmap fast path.
///
/// Layout: 12-byte v9 header (`magic, cf, flags, w, h, stride, rsvd`)
/// + one little-endian RGB565 word. `rgb565(0x10,0x10,0x10) == 0x1082`.
pub const BLANK_BIN: &[u8] = &[
	0x19, 0x12, 0x00, 0x00, // magic, cf=RGB565, flags
	0x01, 0x00, 0x01, 0x00, // w=1, h=1
	0x02, 0x00, 0x00, 0x00, // stride=2, reserved
	0x82, 0x10, // pixel: rgb565(0x10,0x10,0x10) little-endian
];

/// Build one grid cell: a per-key `ImageButton` whose `released` image
/// starts at `blank_path` (a cached blank `.bin`). The button carries
/// this key's host code on both press and release, so it stays
/// clickable regardless of its current artwork. Repaints swap the
/// `released` / `pressed` image in place via
/// [`Touchy::set_image_button_slot`][touchy_pad::Touchy::set_image_button_slot]
/// — never rebuilding the widget, so a key the user is pressing keeps
/// its touch state and still emits a release event.
fn key_button(key: u8, blank_path: &str) -> Widget {
	let code = host_code_for(key);
	let act = Action {
		kind: Some(action::Kind::Host(ActionHost { code })),
	};
	let released = Image {
		path: blank_path.to_string(),
		align: Some(image::Align::ImageAlignStretch as i32),
		..Default::default()
	};
	Widget {
		id: key_widget_id(key),
		// Dark-grey fill shown behind any transparent image regions.
		styles: vec![Style {
			bg_color: Some(0x101010),
			shadow_width: Some(0),
			..Default::default()
		}],
		kind: Some(widget::Kind::ImageButton(ImageButton {
			released: Some(released),
			on_press: vec![act.clone()],
			on_release: vec![act],
			..Default::default()
		})),
		..Default::default()
	}
}

/// Build the OpenDeck page body: a `cols × rows` grid where each cell is
/// a per-key `ImageButton` (see [`key_button`]). Returned as a bare
/// [`Widget`] for upload via
/// [`Touchy::user_screen_save`][touchy_pad::Touchy::user_screen_save].
///
/// `blank_path` is the cached blank-image path every key starts at;
/// painting a key later swaps its image slot in place (no page rewrite).
/// Keys are numbered left-to-right, top-to-bottom from 0 — matching
/// StreamDeck convention.
pub fn build_page(cols: u8, rows: u8, blank_path: &str) -> Widget {
	let mut children: Vec<Widget> = Vec::with_capacity(cols as usize * rows as usize);
	for r in 0..rows {
		for c in 0..cols {
			let k = r * cols + c;
			let mut cell = key_button(k, blank_path);
			cell.placement = Some(widget::Placement::Cell(GridCell {
				col: c as u32,
				row: r as u32,
				..Default::default()
			}));
			cell.grow_x = 1;
			cell.grow_y = 1;
			children.push(cell);
		}
	}
	Widget {
		id: "opendeck_root".into(),
		version: widget::Version::Current as i32,
		kind: Some(widget::Kind::LayoutGrid(LayoutGrid {
			cols: cols as u32,
			rows: rows as u32,
			gap: 4,
			layout: Some(Layout { children }),
		})),
		..Default::default()
	}
}

#[cfg(test)]
mod tests {
	use super::*;

	#[test]
	fn host_code_roundtrip() {
		for k in [0_u8, 1, 14, 255] {
			assert_eq!(key_for_host_code(host_code_for(k)), Some(k));
		}
		assert_eq!(key_for_host_code(0x0FFF), None);
		assert_eq!(key_for_host_code(HOST_CODE_BASE + MAX_KEYS), None);
	}

	#[test]
	fn build_page_has_expected_children() {
		let page = build_page(5, 3, "T:host/icache/blank.bin");
		match page.kind.unwrap() {
			widget::Kind::LayoutGrid(g) => {
				assert_eq!(g.cols, 5);
				assert_eq!(g.rows, 3);
				let children = g.layout.unwrap().children;
				assert_eq!(children.len(), 15);
				// Every cell is a per-key ImageButton carrying its id.
				assert_eq!(children[0].id, key_widget_id(0));
				match children[0].kind.as_ref().unwrap() {
					widget::Kind::ImageButton(ib) => {
						assert_eq!(ib.released.as_ref().unwrap().path, "T:host/icache/blank.bin");
					}
					_ => panic!("expected ImageButton cell"),
				}
			}
			_ => panic!("expected LayoutGrid"),
		}
	}

	#[test]
	fn key_button_carries_host_code_and_blank_path() {
		let page = build_page(2, 1, "T:host/icache/blank.bin");
		let children = match page.kind.unwrap() {
			widget::Kind::LayoutGrid(g) => g.layout.unwrap().children,
			_ => panic!("expected LayoutGrid"),
		};
		let cell = &children[1]; // key 1
		assert_eq!(cell.id, key_widget_id(1));
		match cell.kind.as_ref().unwrap() {
			widget::Kind::ImageButton(ib) => {
				assert_eq!(ib.released.as_ref().unwrap().path, "T:host/icache/blank.bin");
				let act = ib.on_press[0].kind.as_ref().unwrap();
				match act {
					action::Kind::Host(h) => assert_eq!(h.code, host_code_for(1)),
					_ => panic!("expected host action"),
				}
				// Same host code on release for hold-aware routing.
				let rel = ib.on_release[0].kind.as_ref().unwrap();
				match rel {
					action::Kind::Host(h) => assert_eq!(h.code, host_code_for(1)),
					_ => panic!("expected host action"),
				}
			}
			_ => panic!("expected ImageButton"),
		}
	}

	#[test]
	fn blank_bin_is_a_valid_lvgl_header() {
		// magic + at least the 12-byte v9 header.
		assert!(BLANK_BIN.len() >= 12);
		assert_eq!(BLANK_BIN[0], 0x19); // LVGL bin magic
		assert_eq!(BLANK_BIN[1], 0x12); // cf = RGB565
	}

	#[test]
	fn device_id_uses_serial() {
		assert_eq!(device_id_for("tsim001"), "tp-tsim001");
	}
}
