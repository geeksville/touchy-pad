//! Tiny CLI exercising the `touchy-pad` Rust API against real hardware.
//!
//! Subcommands:
//!
//! * `info` — print board info from `sys_board_info_get`.
//! * `demo` — upload a tiny screen with a few image buttons and
//!   activate it.
//! * `listen` — run `demo`, then stream events forever, logging each
//!   touch the user makes.

use anyhow::Result;
use clap::{Parser, Subcommand};
use touchy_pad::Touchy;
use touchy_pad::proto::{Action, ActionHost, Image, ImageButton, Layout, LayoutAbsolute, LvEventCode, Rect, Widget, action, lv_event, widget};

#[derive(Parser, Debug)]
#[command(name = "touchy-demo", about = "Demo CLI for the touchy-pad Rust API")]
struct Cli {
	#[command(subcommand)]
	cmd: Cmd,
}

#[derive(Subcommand, Debug)]
enum Cmd {
	/// Print `sys_board_info_get` output and exit.
	Info,
	/// Upload + load a small screen with a few image buttons.
	Demo,
	/// Run `demo`, then stream events forever.
	Listen,
}

#[tokio::main]
async fn main() -> Result<()> {
	env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
	let cli = Cli::parse();
	match cli.cmd {
		Cmd::Info => cmd_info().await,
		Cmd::Demo => cmd_demo().await,
		Cmd::Listen => cmd_listen().await,
	}
}

async fn cmd_info() -> Result<()> {
	let pad = Touchy::open().await?;
	let info = pad.client().sys_board_info_get().await?;
	println!("{info:#?}");
	Ok(())
}

/// A 64x64 solid-colour PNG generated at runtime so the demo doesn't
/// have to ship any binary assets.
fn solid_tile_png(r: u8, g: u8, b: u8) -> Vec<u8> {
	use std::io::Cursor;
	let mut img = ::image::RgbImage::new(64, 64);
	for px in img.pixels_mut() {
		*px = ::image::Rgb([r, g, b]);
	}
	let mut out = Vec::new();
	::image::DynamicImage::ImageRgb8(img)
		.write_to(&mut Cursor::new(&mut out), ::image::ImageFormat::Png)
		.expect("encode png");
	out
}

async fn upload_demo(pad: &Touchy) -> Result<()> {
	let colors: [(u8, u8, u8, &str); 3] = [(200, 50, 50, "red"), (50, 200, 50, "green"), (50, 50, 200, "blue")];

	let mut children: Vec<Widget> = Vec::new();
	for (i, (r, g, b, name)) in colors.iter().enumerate() {
		// Upload PNG; the host transparently converts to LVGL .bin
		// and the device sees the rewritten path.
		let png = solid_tile_png(*r, *g, *b);
		let bin_path = pad.file_save(&format!("R:host/demo/{name}.png"), &png).await?;
		log::info!("uploaded {bin_path}");

		let img = Image { path: bin_path, ..Default::default() };
		let action = Action {
			kind: Some(action::Kind::Host(ActionHost { code: i as u32 })),
		};
		children.push(Widget {
			id: format!("btn_{name}"),
			placement: Some(widget::Placement::Rect(Rect {
				x: 16 + (i as i32) * 80,
				y: 16,
				w: 64,
				h: 64,
			})),
			kind: Some(widget::Kind::ImageButton(ImageButton {
				released: Some(img),
				on_press: vec![action.clone()],
				on_release: vec![action],
				..Default::default()
			})),
			..Default::default()
		});
	}

	// Root layout-widget — the body of one user-screen page.
	// Saved to F:host/uscr/rust_demo.pb so the default chrome's
	// widget_ref(id="page") can page through it with Prev / Next.
	let root = Widget {
		id: "root".into(),
		version: touchy_pad::proto::widget::Version::Current as i32,
		kind: Some(widget::Kind::LayoutAbsolute(LayoutAbsolute { layout: Some(Layout { children }) })),
		..Default::default()
	};

	pad.user_screen_save("rust_demo", &root).await?;
	pad.screen_load(touchy_pad::DEFAULT_SCREEN_PATH).await?;
	log::info!("rust_demo user-screen uploaded; default chrome loaded");
	Ok(())
}

async fn cmd_demo() -> Result<()> {
	let pad = Touchy::open().await?;
	upload_demo(&pad).await?;
	Ok(())
}

async fn cmd_listen() -> Result<()> {
	let pad = Touchy::open().await?;
	upload_demo(&pad).await?;
	let mut rx = pad.events().await.expect("events() already taken");
	log::info!("listening for events… ^C to quit");
	while let Some(evt) = rx.recv().await {
		let state = match &evt.state {
			Some(lv_event::State::Value(v)) => format!("value={v}"),
			Some(lv_event::State::Checked(c)) => format!("checked={c}"),
			None => "-".to_string(),
		};
		let code_name = LvEventCode::try_from(evt.code as i32).map(|c| format!("{c:?}")).unwrap_or_else(|_| format!("{}", evt.code));
		println!("event: code={code_name} user_data={:?} host_code={} state={}", evt.user_data, evt.host_code, state);
	}
	Ok(())
}
