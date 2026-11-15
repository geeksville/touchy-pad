//! Build a screen out of raw protobuf structs and show it.
//!
//! Rust has **no screen DSL** (unlike the Python `touchy_pad.api.screens`)
//! — you assemble `touchy_pad::proto::Widget` values directly. This example
//! builds a flex row of labelled buttons, saves it as a *user page*, and pages
//! the stock prev/next chrome to it, so the chrome itself survives.
//!
//! The chrome (`F:host/s/default.pb`) is provisioned by `touchy init`; if the
//! device has never been initialised, run that once first.
//!
//! ```sh
//! cargo run --example screen
//! ```

use touchy_pad::proto::{Action, ActionHost, Button, Label, Layout, LayoutFlex, Widget, action, layout_flex, widget};
use touchy_pad::{DEFAULT_SCREEN_PATH, Touchy};

/// A press/release pair that reports back to the host as `host_code`.
fn host_action(code: u32) -> Action {
	Action {
		kind: Some(action::Kind::Host(ActionHost { code })),
	}
}

#[tokio::main]
async fn main() -> touchy_pad::Result<()> {
	env_logger::init();
	let pad = Touchy::open().await?;

	let mut children = vec![
		// Flex children need no placement — the layout engine positions them.
		Widget {
			id: "title".into(),
			kind: Some(widget::Kind::Label(Label {
				text: "touchy-pad (Rust)".into(),
				..Default::default()
			})),
			..Default::default()
		},
	];

	for (i, name) in ["one", "two", "three"].iter().enumerate() {
		children.push(Widget {
			id: format!("btn_{name}"),
			kind: Some(widget::Kind::Button(Button {
				text: (*name).to_string(),
				// Host actions come back through `Touchy::events()`; use a
				// macro action instead to run USB-HID device-side with no host
				// attached.
				on_press: vec![host_action(i as u32)],
				on_release: vec![host_action(i as u32)],
				..Default::default()
			})),
			..Default::default()
		});
	}

	let root = Widget {
		id: "root".into(),
		// The wire version rides on the **root** widget of the file and the
		// device rejects a mismatch. `Version::Current` is generated from
		// `proto/widgets.proto` — never hand-write the number.
		version: widget::Version::Current as i32,
		kind: Some(widget::Kind::LayoutFlex(LayoutFlex {
			flow: layout_flex::Flow::Row as i32,
			gap: 8,
			layout: Some(Layout { children }),
		})),
		..Default::default()
	};

	// User pages live in `F:host/uscr/<name>.pb` and are what the chrome's
	// `widget_ref(id="page")` pages through.
	pad.user_screen_save("rust_example", &root).await?;
	pad.screen_load(DEFAULT_SCREEN_PATH).await?;
	pad.show_user_screen("rust_example").await?;

	println!("rust_example page is on screen; run `cargo run --example events` to watch touches");
	Ok(())
}
