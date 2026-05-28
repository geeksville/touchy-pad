//! Integration test that spins up the Python `touchy simulator` and
//! drives it from the Rust client over TCP (Stage 63).
//!
//! Skipped automatically when `poetry` isn't on PATH (so this passes
//! in pure-Rust CI without the Python toolchain).

use std::net::{TcpListener, TcpStream};
use std::process::{Command, Stdio};
use std::sync::Arc;
use std::time::{Duration, Instant};

use touchy_pad::Touchy;
use touchy_pad::transport::Transport;
use touchy_pad::transport_net::TcpTransport;

fn find_free_port() -> u16 {
	let l = TcpListener::bind("127.0.0.1:0").expect("bind ephemeral");
	l.local_addr().unwrap().port()
}

fn poetry_available() -> bool {
	Command::new("poetry")
		.arg("--version")
		.stdout(Stdio::null())
		.stderr(Stdio::null())
		.status()
		.map(|s| s.success())
		.unwrap_or(false)
}

fn wait_for_port(port: u16, deadline: Instant) -> bool {
	while Instant::now() < deadline {
		if TcpStream::connect_timeout(&format!("127.0.0.1:{port}").parse().unwrap(), Duration::from_millis(100)).is_ok() {
			return true;
		}
		std::thread::sleep(Duration::from_millis(100));
	}
	false
}

#[tokio::test]
async fn sim_tcp_board_info() {
	if !poetry_available() {
		eprintln!("poetry not available — skipping");
		return;
	}
	let port = find_free_port();
	let app_dir = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../app");
	let mut child = Command::new("poetry")
		.args(["run", "touchy", "--sim-serial", "SIMRUST", "simulator", "--headless", "--port", &port.to_string()])
		.current_dir(&app_dir)
		.env_remove("VIRTUAL_ENV")
		.stdout(Stdio::null())
		.stderr(Stdio::null())
		.spawn()
		.expect("spawn touchy simulator");

	let started = wait_for_port(port, Instant::now() + Duration::from_secs(15));
	if !started {
		let _ = child.kill();
		panic!("sim server didn't open port {port}");
	}

	let result = async {
		let t = Arc::new(TcpTransport::connect("127.0.0.1", port).await?) as Arc<dyn Transport>;
		let pad = Touchy::from_transport(t);
		let info = pad.client().sys_board_info_get().await?;
		assert_eq!(info.board_name, "sim");
		touchy_pad::Result::Ok(())
	}
	.await;

	let _ = child.kill();
	let _ = child.wait();
	result.expect("rust ↔ python sim TCP roundtrip");
}
