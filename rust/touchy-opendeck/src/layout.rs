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

use touchy_pad::proto::{Action, ActionHost, GridCell, Image, ImageButton, Layout, LayoutGrid, Style, Widget, action, widget};

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

/// Ramdisk path for cell ``key``'s image asset.
///
/// One shared set of assets — there is a single OpenDeck page. Lives
/// on ``R:`` (volatile PSRAM ramdisk) because we rewrite it on every
/// (re)connect and per keypress, and flash wear would be wasteful.
pub fn asset_path_for(key: u8) -> String {
	format!("R:host/opendeck/key_{key}.bin")
}

/// Build the OpenDeck page body: a ``cols × rows`` grid of image
/// buttons, each wired to ``host_code_for(k)`` on both press and
/// release. Returned as a bare [`Widget`] for upload via
/// [`Touchy::user_screen_save`][touchy_pad::Touchy::user_screen_save].
///
/// Keys are numbered left-to-right, top-to-bottom from 0 — matching
/// StreamDeck convention.
pub fn build_page(cols: u8, rows: u8) -> Widget {
	let mut children: Vec<Widget> = Vec::with_capacity(cols as usize * rows as usize);
	for r in 0..rows {
		for c in 0..cols {
			let k = r * cols + c;
			let code = host_code_for(k);
			let asset = asset_path_for(k);
			let img = Image { path: asset, ..Default::default() };
			let act_press = Action {
				kind: Some(action::Kind::Host(ActionHost { code })),
			};
			let act_release = act_press.clone();
			children.push(Widget {
				id: format!("opendeck_key_{k}"),
				placement: Some(widget::Placement::Cell(GridCell {
					col: c as u32,
					row: r as u32,
					..Default::default()
				})),
				grow_x: 1,
				grow_y: 1,
				// Dark-grey fill for cells with no image assigned.
				// When an image is loaded it renders on top and hides this.
				styles: vec![Style { bg_color: Some(0x101010), shadow_width: Some(0), ..Default::default() }],
				kind: Some(widget::Kind::ImageButton(ImageButton {
					released: Some(img),
					on_press: vec![act_press],
					on_release: vec![act_release],
					..Default::default()
				})),
				..Default::default()
			});
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
		let page = build_page(5, 3);
		match page.kind.unwrap() {
			widget::Kind::LayoutGrid(g) => {
				assert_eq!(g.cols, 5);
				assert_eq!(g.rows, 3);
				assert_eq!(g.layout.unwrap().children.len(), 15);
			}
			_ => panic!("expected LayoutGrid"),
		}
	}

	#[test]
	fn device_id_uses_serial() {
		assert_eq!(device_id_for("tsim001"), "tp-tsim001");
	}
}
