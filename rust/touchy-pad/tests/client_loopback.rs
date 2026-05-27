//! End-to-end Client tests against an in-memory mock Transport.

use std::collections::VecDeque;
use std::sync::Arc;
use std::time::Duration;

use async_trait::async_trait;
use prost::Message;
use tokio::sync::Mutex;

use touchy_pad::client::Client;
use touchy_pad::proto::{Command, EventConsumeResponse, LvEvent, Response, ResultCode, SysBoardInfoResponse, response};
use touchy_pad::transport::Transport;
use touchy_pad::TouchyError;

#[derive(Default)]
struct Mock {
	sent: Mutex<Vec<Vec<u8>>>,
	replies: Mutex<VecDeque<Response>>,
}

impl Mock {
	fn push(&self, resp: Response) {
		self.replies.try_lock().unwrap().push_back(resp);
	}
}

#[async_trait]
impl Transport for Mock {
	async fn send_command(&self, payload: &[u8]) -> touchy_pad::Result<()> {
		self.sent.lock().await.push(payload.to_vec());
		Ok(())
	}
	async fn recv_response(&self, _: Duration) -> touchy_pad::Result<Vec<u8>> {
		let resp = self
			.replies
			.lock()
			.await
			.pop_front()
			.expect("test forgot to queue a Response");
		let mut buf = Vec::with_capacity(resp.encoded_len());
		resp.encode(&mut buf).unwrap();
		Ok(buf)
	}
}

fn ok(payload: response::Payload) -> Response {
	Response { code: ResultCode::ResultOk as i32, payload: Some(payload) }
}

#[tokio::test]
async fn sys_board_info_round_trip() {
	let mock = Arc::new(Mock::default());
	mock.push(ok(response::Payload::SysBoardInfo(SysBoardInfoResponse {
		board_name: "test-board".into(),
		firmware_version_str: "0.0.0".into(),
		..Default::default()
	})));
	let client = Client::new(mock.clone());
	let info = client.sys_board_info_get().await.unwrap();
	assert_eq!(info.board_name, "test-board");
	// Verify the command we sent decodes to SysBoardInfoGet.
	let sent = &mock.sent.lock().await[0];
	let cmd = Command::decode(sent.as_slice()).unwrap();
	assert!(matches!(cmd.cmd, Some(touchy_pad::proto::command::Cmd::SysBoardInfoGet(_))));
}

#[tokio::test]
async fn event_consume_empty_returns_none() {
	let mock = Arc::new(Mock::default());
	mock.push(Response { code: ResultCode::ResultNotFound as i32, payload: None });
	let client = Client::new(mock);
	assert!(client.event_consume().await.unwrap().is_none());
}

#[tokio::test]
async fn event_consume_delivers_event() {
	let mock = Arc::new(Mock::default());
	mock.push(ok(response::Payload::EventConsume(EventConsumeResponse {
		event: Some(LvEvent {
			code: 7,
			user_data: "btn_red".into(),
			host_code: 42,
			state: None,
		}),
	})));
	let client = Client::new(mock);
	let evt = client.event_consume().await.unwrap().expect("event");
	assert_eq!(evt.host_code, 42);
	assert_eq!(evt.user_data, "btn_red");
}

#[tokio::test]
async fn non_ok_code_becomes_device_error() {
	let mock = Arc::new(Mock::default());
	// arbitrary non-OK code
	mock.push(Response { code: ResultCode::ResultInvalidArg as i32, payload: None });
	let client = Client::new(mock);
	let err = client.sys_board_info_get().await.unwrap_err();
	match err {
		TouchyError::Device { code, name } => {
			assert_eq!(code, ResultCode::ResultInvalidArg as i32);
			assert!(name.contains("INVALID_ARG"), "name was {name}");
		}
		other => panic!("expected Device error, got {other:?}"),
	}
}
