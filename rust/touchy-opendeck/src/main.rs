//! `touchy-opendeck` — OpenDeck device plugin for Touchy-Pad hardware.
//!
//! Run by OpenDeck as a child process with WebSocket connection args
//! on `argv`. See `docs/opendeck-device-plugin.md` for the protocol
//! and `README.md` for an architecture overview.

mod layout;
mod plugin;

use openaction::OpenActionResult;
use plugin::TouchyPlugin;

#[tokio::main(flavor = "multi_thread")]
async fn main() -> OpenActionResult<()> {
	// Logs must go to stderr — OpenDeck multiplexes the JSON socket
	// over the plugin's stdio and stdout writes corrupt it.
	let _ = simplelog::TermLogger::init(
		log::LevelFilter::Info,
		simplelog::ConfigBuilder::new().set_time_format_rfc3339().build(),
		simplelog::TerminalMode::Stderr,
		simplelog::ColorChoice::Never,
	);

	// `set_global_event_handler` needs a `&'static dyn GlobalEventHandler`.
	// The plugin lives for the whole process lifetime, so leak it.
	let plugin: &'static TouchyPlugin = Box::leak(Box::new(TouchyPlugin::new()));
	openaction::global_events::set_global_event_handler(plugin);

	openaction::run(std::env::args().collect()).await
}
