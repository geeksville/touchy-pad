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

use touchy_pad::proto::{Action, ActionHost, GridCell, Image, ImageButton, Layout, LayoutGrid, Screen, Widget, action, widget};

/// Two-character OpenDeck device-namespace prefix used by this plugin.
///
/// All device IDs we ``register_device`` must start with this prefix —
/// OpenDeck uses it to route inbound events back to us.
pub const NAMESPACE: &str = "tp";

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

/// Stable, plugin-namespaced device ID derived from USB bus + address.
///
/// The OpenDeck protocol requires the ID to start with [`NAMESPACE`]
/// and to be stable across reconnects. ``bus``/``addr`` are stable for
/// the lifetime of a USB port and a single physical device, so this is
/// "good enough" — better than enumeration order, not as good as a
/// hardware serial.
pub fn device_id_for(bus: u8, addr: u8) -> String {
	format!("{NAMESPACE}-{bus:02x}{addr:02x}")
}

/// PSRAM ramdisk path holding the encoded layout for ``device_id``.
///
/// One file per device — separate from any user/Python-managed
/// screens. Lives on ``R:`` because we rewrite it whenever OpenDeck
/// pushes a new image, and flash wear from per-keypress reloads would
/// be wasteful.
pub fn screen_path_for(device_id: &str) -> String {
	format!("R:host/screens/opendeck_{device_id}.pb")
}

/// PSRAM ramdisk path for cell ``key``'s image asset for ``device_id``.
pub fn asset_path_for(device_id: &str, key: u8) -> String {
	format!("R:host/opendeck/{device_id}/key_{key}.bin")
}

/// Build the device layout: a ``cols × rows`` grid of image buttons,
/// each wired to ``host_code_for(k)`` on both press and release.
///
/// Keys are numbered left-to-right, top-to-bottom from 0 — matching
/// StreamDeck convention.
pub fn build_screen(cols: u8, rows: u8, device_id: &str) -> Screen {
	let mut children: Vec<Widget> = Vec::with_capacity(cols as usize * rows as usize);
	for r in 0..rows {
		for c in 0..cols {
			let k = r * cols + c;
			let code = host_code_for(k);
			let asset = asset_path_for(device_id, k);
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
	let active = Widget {
		id: "opendeck_root".into(),
		version: widget::Version::Current as i32,
		kind: Some(widget::Kind::LayoutGrid(LayoutGrid {
			cols: cols as u32,
			rows: rows as u32,
			gap: 4,
			layout: Some(Layout { children }),
		})),
		..Default::default()
	};
	Screen {
		active: Some(active),
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
	fn build_screen_has_expected_children() {
		let s = build_screen(5, 3, "tp-0102");
		let root = s.active.unwrap();
		match root.kind.unwrap() {
			widget::Kind::LayoutGrid(g) => {
				assert_eq!(g.cols, 5);
				assert_eq!(g.rows, 3);
				assert_eq!(g.layout.unwrap().children.len(), 15);
			}
			_ => panic!("expected LayoutGrid"),
		}
	}
}
