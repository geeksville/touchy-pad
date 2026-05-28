//! Round-trip + edge-case tests for the wire framing helpers.

use touchy_pad::TouchyError;
use touchy_pad::transport::{MAX_FRAME, pack, unpack};

#[test]
fn round_trip_small() {
	let payload = b"hello, touchy";
	let frame = pack(payload).unwrap();
	assert_eq!(frame.len(), 4 + payload.len());
	let (out, consumed) = unpack(&frame).unwrap();
	assert_eq!(out, payload);
	assert_eq!(consumed, frame.len());
}

#[test]
fn round_trip_empty() {
	let frame = pack(&[]).unwrap();
	assert_eq!(frame, vec![0, 0, 0, 0]);
	let (out, consumed) = unpack(&frame).unwrap();
	assert!(out.is_empty());
	assert_eq!(consumed, 4);
}

#[test]
fn pack_rejects_oversize() {
	let huge = vec![0u8; MAX_FRAME + 1];
	let err = pack(&huge).unwrap_err();
	assert!(matches!(err, TouchyError::Framing(_)), "got {err:?}");
}

#[test]
fn unpack_rejects_oversize_header() {
	let mut frame = ((MAX_FRAME + 1) as u32).to_le_bytes().to_vec();
	frame.push(0);
	let err = unpack(&frame).unwrap_err();
	assert!(matches!(err, TouchyError::Framing(_)));
}

#[test]
fn unpack_rejects_short_header() {
	let err = unpack(&[0, 0, 0]).unwrap_err();
	assert!(matches!(err, TouchyError::Framing(_)));
}

#[test]
fn unpack_rejects_truncated() {
	// header says 8 bytes but only 3 follow
	let frame = [8, 0, 0, 0, 1, 2, 3];
	let err = unpack(&frame).unwrap_err();
	assert!(matches!(err, TouchyError::Framing(_)));
}
