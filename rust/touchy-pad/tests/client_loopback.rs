//! End-to-end Client tests against an in-memory mock Transport.

use std::collections::VecDeque;
use std::sync::Arc;
use std::time::Duration;

use async_trait::async_trait;
use prost::Message;
use tokio::sync::Mutex;

use touchy_pad::TouchyError;
use touchy_pad::client::{Client, PollItem};
use touchy_pad::proto::{Command, EventConsumeResponse, LogPriority, LogRecord, LvEvent, Response, ResultCode, SysBoardInfoResponse, response};
use touchy_pad::transport::Transport;

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
		let resp = self.replies.lock().await.pop_front().expect("test forgot to queue a Response");
		let mut buf = Vec::with_capacity(resp.encoded_len());
		resp.encode(&mut buf).unwrap();
		Ok(buf)
	}
}

fn ok(payload: response::Payload) -> Response {
	Response {
		code: ResultCode::Ok as i32,
		payload: Some(payload),
	}
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
	mock.push(Response {
		code: ResultCode::NotFound as i32,
		payload: None,
	});
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
	mock.push(Response {
		code: ResultCode::InvalidArg as i32,
		payload: None,
	});
	let client = Client::new(mock);
	let err = client.sys_board_info_get().await.unwrap_err();
	match err {
		TouchyError::Device { code, name } => {
			assert_eq!(code, ResultCode::InvalidArg as i32);
			assert!(name.contains("INVALID_ARG"), "name was {name}");
		}
		other => panic!("expected Device error, got {other:?}"),
	}
}

#[tokio::test]
async fn poll_returns_log_record_when_only_logs_pending() {
	let mock = Arc::new(Mock::default());
	mock.push(ok(response::Payload::LogRecord(LogRecord {
		priority: LogPriority::Info as i32,
		message: "hello from device".into(),
		tag: "WIFI".into(),
		timestamp_ms: 1234,
		num_dropped: 0,
	})));
	let client = Client::new(mock);
	match client.poll().await.unwrap().expect("expected an item") {
		PollItem::Log(rec) => {
			assert_eq!(rec.message, "hello from device");
			assert_eq!(rec.tag, "WIFI");
			assert_eq!(rec.priority, LogPriority::Info as i32);
		}
		PollItem::Event(_) => panic!("expected LogRecord, got LvEvent"),
	}
}

#[tokio::test]
async fn event_consume_silently_consumes_log_records() {
	let mock = Arc::new(Mock::default());
	mock.push(ok(response::Payload::LogRecord(LogRecord {
		priority: LogPriority::Warn as i32,
		message: "transient blip".into(),
		tag: "DBG".into(),
		..Default::default()
	})));
	// After the log record, the device reports an empty queue;
	// event_consume() drains the log internally then returns None.
	mock.push(Response {
		code: ResultCode::NotFound as i32,
		payload: None,
	});
	let client = Client::new(mock);
	assert!(client.event_consume().await.unwrap().is_none());
}

#[tokio::test]
async fn event_consume_drains_logs_before_returning_event() {
	let mock = Arc::new(Mock::default());
	for i in 0..3 {
		mock.push(ok(response::Payload::LogRecord(LogRecord {
			priority: LogPriority::Info as i32,
			message: format!("log {i}"),
			tag: "T".into(),
			..Default::default()
		})));
	}
	mock.push(ok(response::Payload::EventConsume(EventConsumeResponse {
		event: Some(LvEvent {
			code: 7,
			user_data: "btn".into(),
			host_code: 99,
			state: None,
		}),
	})));
	let client = Client::new(mock);
	let evt = client.event_consume().await.unwrap().expect("event");
	assert_eq!(evt.host_code, 99);
}

#[tokio::test]
async fn drain_pending_dispatches_logs_and_parks_event() {
	let mock = Arc::new(Mock::default());
	mock.push(ok(response::Payload::LogRecord(LogRecord {
		priority: LogPriority::Info as i32,
		message: "boot log".into(),
		tag: "BOOT".into(),
		..Default::default()
	})));
	mock.push(ok(response::Payload::EventConsume(EventConsumeResponse {
		event: Some(LvEvent {
			code: 1,
			user_data: "early".into(),
			host_code: 7,
			state: None,
		}),
	})));
	let client = Client::new(mock);
	client.drain_pending(256).await.unwrap();
	// Parked event is returned by the next event_consume() with no
	// further transport activity.
	let evt = client.event_consume().await.unwrap().expect("parked event");
	assert_eq!(evt.host_code, 7);
}

// ---- stage lb26: font-size override ----------------------------------------

#[tokio::test]
async fn set_font_size_sends_the_font_size_arm() {
	use touchy_pad::proto::{FontSize, SetPropertiesCmd, command, set_property_cmd};

	let mock = Arc::new(Mock::default());
	mock.push(Response {
		code: ResultCode::Ok as i32,
		payload: None,
	});
	let client = Client::new(mock.clone());
	client.set_font_size("title", FontSize::FontSize30).await.unwrap();

	let sent = mock.sent.lock().await;
	let cmd = Command::decode(sent[0].as_slice()).unwrap();
	match cmd.cmd {
		Some(command::Cmd::SetProperties(SetPropertiesCmd { props })) => {
			assert_eq!(props.len(), 1);
			assert_eq!(props[0].widget_id, "title");
			assert!(matches!(
				props[0].property,
				Some(set_property_cmd::Property::PropertyName(ref n)) if n == "text_font"
			));
			assert!(matches!(props[0].value, Some(set_property_cmd::Value::FontSizeValue(30))));
		}
		other => panic!("expected a SetProperties command, got {other:?}"),
	}
}

#[tokio::test]
async fn font_size_default_lets_a_widget_inherit_the_theme_again() {
	use touchy_pad::proto::{FontSize, SetPropertiesCmd, command, set_property_cmd};

	let mock = Arc::new(Mock::default());
	mock.push(Response {
		code: ResultCode::Ok as i32,
		payload: None,
	});
	let client = Client::new(mock.clone());
	client.set_font_size("title", FontSize::Default).await.unwrap();

	let sent = mock.sent.lock().await;
	let cmd = Command::decode(sent[0].as_slice()).unwrap();
	match cmd.cmd {
		Some(command::Cmd::SetProperties(SetPropertiesCmd { props })) => {
			assert!(matches!(props[0].value, Some(set_property_cmd::Value::FontSizeValue(0))));
		}
		other => panic!("expected a SetProperties command, got {other:?}"),
	}
}

#[test]
fn font_sizes_const_matches_the_enum() {
	use touchy_pad::FontSize;

	// Stage lb26 — the published list is exactly the enum's non-zero members,
	// in ascending order. (prost strips the enum-name prefix, hence
	// `FontSize8` rather than `FontSize_FONT_SIZE_8`.)
	let mut from_enum: Vec<u32> = [FontSize::FontSize8, FontSize::FontSize14, FontSize::FontSize16, FontSize::FontSize24, FontSize::FontSize30]
		.iter()
		.map(|f| *f as u32)
		.collect();
	from_enum.sort_unstable();
	assert_eq!(from_enum.as_slice(), touchy_pad::FONT_SIZES);
	assert_eq!(FontSize::Default as u32, 0);
}
