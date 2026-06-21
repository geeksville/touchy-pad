//! `Touchy::events` integration test driven by a programmable mock
//! Transport that delivers a small canned event stream.

use std::collections::VecDeque;
use std::sync::Arc;
use std::time::Duration;

use async_trait::async_trait;
use prost::Message;
use tokio::sync::Mutex;

use touchy_pad::Touchy;
use touchy_pad::proto::{Command, EventConsumeResponse, LvEvent, Response, ResultCode, command, response};
use touchy_pad::transport::Transport;

#[derive(Default)]
struct Mock {
	events: Mutex<VecDeque<LvEvent>>,
}

#[async_trait]
impl Transport for Mock {
	async fn send_command(&self, payload: &[u8]) -> touchy_pad::Result<()> {
		// We only need to validate that EventConsume keeps coming;
		// other commands aren't exercised by this test.
		let cmd = Command::decode(payload).expect("decode command");
		assert!(matches!(cmd.cmd, Some(command::Cmd::EventConsume(_))), "unexpected command sent during pad-events test: {:?}", cmd.cmd);
		Ok(())
	}
	async fn recv_response(&self, _: Duration) -> touchy_pad::Result<Vec<u8>> {
		let next = self.events.lock().await.pop_front();
		let resp = match next {
			Some(evt) => Response {
				code: ResultCode::Ok as i32,
				payload: Some(response::Payload::EventConsume(EventConsumeResponse { event: Some(evt) })),
			},
			None => Response {
				code: ResultCode::NotFound as i32,
				payload: None,
			},
		};
		let mut buf = Vec::with_capacity(resp.encoded_len());
		resp.encode(&mut buf).unwrap();
		Ok(buf)
	}
}

#[tokio::test]
async fn events_are_streamed_through() {
	let mock = Arc::new(Mock::default());
	{
		let mut q = mock.events.lock().await;
		q.push_back(LvEvent {
			code: 1,
			user_data: "btn_a".into(),
			host_code: 10,
			state: None,
		});
		q.push_back(LvEvent {
			code: 11,
			user_data: "btn_a".into(),
			host_code: 10,
			state: None,
		});
	}

	let pad = Touchy::from_transport(mock as Arc<dyn Transport>);
	let mut rx = pad.events().await.expect("events receiver");

	let first = tokio::time::timeout(Duration::from_secs(2), rx.recv()).await.unwrap().expect("event 1");
	assert_eq!(first.host_code, 10);
	assert_eq!(first.code, 1);
	let second = tokio::time::timeout(Duration::from_secs(2), rx.recv()).await.unwrap().expect("event 2");
	assert_eq!(second.code, 11);

	pad.close().await;
}

#[tokio::test]
async fn events_take_is_one_shot() {
	let mock = Arc::new(Mock::default());
	let pad = Touchy::from_transport(mock as Arc<dyn Transport>);
	assert!(pad.events().await.is_some());
	assert!(pad.events().await.is_none(), "second events() call must return None");
	pad.close().await;
}
