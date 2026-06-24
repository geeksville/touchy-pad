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
	let repo_root = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../");
	let mut child = Command::new("poetry")
		.args(["-P", "app", "run", "touchy", "--sim-serial", "SIMRUST", "simulator", "--headless", "--port", &port.to_string()])
		.current_dir(&repo_root)
		.env_remove("VIRTUAL_ENV")
		.stdout(Stdio::null())
		.stderr(Stdio::piped()) // captured so we can print it on failure
		.spawn()
		.expect("spawn touchy simulator");

	let started = wait_for_port(port, Instant::now() + Duration::from_secs(15));
	if !started {
		let stderr = child.wait_with_output().map(|o| String::from_utf8_lossy(&o.stderr).into_owned()).unwrap_or_default();
		panic!("sim server didn't open port {port}\nsim stderr:\n{stderr}");
	}

	// `wait_for_port` opens and immediately drops a probe TCP connection,
	// which the Python asyncio sim accepts and then resets. Give the sim
	// a moment to finish handling that phantom connection before we send
	// a real RPC — otherwise we race and get "Connection reset by peer".
	// We also retry a few times with backoff so the test is robust even
	// if the sim is briefly slow on a loaded CI runner.
	const MAX_ATTEMPTS: u32 = 5;
	let result: touchy_pad::Result<()> = async {
		for attempt in 0..MAX_ATTEMPTS {
			if attempt == 0 {
				// Initial grace period for the sim to settle after the probe.
				tokio::time::sleep(Duration::from_millis(200)).await;
			} else {
				eprintln!("sim_tcp: attempt {attempt} — retrying in 500 ms");
				tokio::time::sleep(Duration::from_millis(500)).await;
			}
			let t = match TcpTransport::connect("127.0.0.1", port).await {
				Ok(t) => Arc::new(t) as Arc<dyn Transport>,
				Err(e) => {
					eprintln!("sim_tcp: connect error on attempt {attempt}: {e}");
					if attempt + 1 == MAX_ATTEMPTS {
						return Err(e);
					}
					continue;
				}
			};
			let pad = Touchy::from_transport(t);
			match pad.client().sys_board_info_get().await {
				Ok(info) => {
					assert_eq!(info.board_name, "sim");
					pad.close().await;
					return Ok(());
				}
				Err(e) if attempt + 1 < MAX_ATTEMPTS => {
					eprintln!("sim_tcp: RPC error on attempt {attempt}: {e}");
					pad.close().await;
					continue;
				}
				Err(e) => {
					pad.close().await;
					return Err(e);
				}
			}
		}
		unreachable!()
	}
	.await;

	let _ = child.kill();
	// Drain stderr so wait_with_output() doesn't block on the pipe buffer.
	let output = child.wait_with_output().ok();
	if result.is_err() {
		let stderr = output.as_ref().map(|o| String::from_utf8_lossy(&o.stderr).into_owned()).unwrap_or_default();
		eprintln!("sim stderr:\n{stderr}");
	}
	result.expect("rust ↔ python sim TCP roundtrip");
}
