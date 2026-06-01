//! Round-trip + edge-case tests for the wire framing helpers.

use touchy_pad::TouchyError;
use touchy_pad::transport::{FrameDecoder, MAGIC, MAX_FRAME, crc8, pack, unpack};

// Frame overhead: MAGIC(2) + LEN(2) + CRC8(1).
const OVERHEAD: usize = 5;

#[test]
fn round_trip_small() {
	let payload = b"hello, touchy";
	let frame = pack(payload).unwrap();
	assert_eq!(frame.len(), OVERHEAD + payload.len());
	assert_eq!(&frame[..2], &MAGIC);
	let (out, consumed) = unpack(&frame).unwrap();
	assert_eq!(out, payload);
	assert_eq!(consumed, frame.len());
}

#[test]
fn round_trip_empty() {
	let frame = pack(&[]).unwrap();
	// MAGIC, LEN=0, CRC over the two LEN bytes (both zero).
	assert_eq!(frame, vec![MAGIC[0], MAGIC[1], 0, 0, crc8(&[0, 0])]);
	let (out, consumed) = unpack(&frame).unwrap();
	assert!(out.is_empty());
	assert_eq!(consumed, OVERHEAD);
}

#[test]
fn pack_rejects_oversize() {
	let huge = vec![0u8; MAX_FRAME + 1];
	let err = pack(&huge).unwrap_err();
	assert!(matches!(err, TouchyError::Framing(_)), "got {err:?}");
}

#[test]
fn unpack_rejects_bad_crc() {
	let mut frame = pack(b"data").unwrap();
	let last = frame.len() - 1;
	frame[last] ^= 0xFF; // corrupt the CRC
	let err = unpack(&frame).unwrap_err();
	assert!(matches!(err, TouchyError::Framing(_)));
}

#[test]
fn unpack_rejects_missing_magic() {
	let mut frame = pack(b"data").unwrap();
	frame[0] = 0x00; // clobber MAGIC
	let err = unpack(&frame).unwrap_err();
	assert!(matches!(err, TouchyError::Framing(_)));
}

#[test]
fn unpack_rejects_short_header() {
	let err = unpack(&[MAGIC[0], MAGIC[1], 0]).unwrap_err();
	assert!(matches!(err, TouchyError::Framing(_)));
}

#[test]
fn unpack_rejects_truncated() {
	// LEN says 8 bytes but only 3 follow.
	let frame = [MAGIC[0], MAGIC[1], 8, 0, 1, 2, 3];
	let err = unpack(&frame).unwrap_err();
	assert!(matches!(err, TouchyError::Framing(_)));
}

#[test]
fn decoder_round_trip_multiple() {
	let mut dec = FrameDecoder::new();
	let mut stream = Vec::new();
	stream.extend_from_slice(&pack(b"one").unwrap());
	stream.extend_from_slice(&pack(b"two").unwrap());
	stream.extend_from_slice(&pack(b"three").unwrap());
	dec.feed(&stream);
	assert_eq!(dec.next_frame().unwrap(), b"one");
	assert_eq!(dec.next_frame().unwrap(), b"two");
	assert_eq!(dec.next_frame().unwrap(), b"three");
	assert!(dec.next_frame().is_none());
}

#[test]
fn decoder_resyncs_after_garbage() {
	let mut dec = FrameDecoder::new();
	dec.feed(&[0xDE, 0xAD, 0xBE, 0xEF]);
	dec.feed(&pack(b"payload").unwrap());
	assert_eq!(dec.next_frame().unwrap(), b"payload");
}

#[test]
fn decoder_resyncs_after_bad_crc() {
	let mut dec = FrameDecoder::new();
	let mut bad = pack(b"corrupt").unwrap();
	let last = bad.len() - 1;
	bad[last] ^= 0xFF; // corrupt CRC of first frame
	dec.feed(&bad);
	dec.feed(&pack(b"good").unwrap());
	assert_eq!(dec.next_frame().unwrap(), b"good");
}

#[test]
fn decoder_handles_byte_at_a_time() {
	let frame = pack(b"dribble").unwrap();
	let mut dec = FrameDecoder::new();
	for (i, b) in frame.iter().enumerate() {
		dec.feed(&[*b]);
		if i + 1 < frame.len() {
			assert!(dec.next_frame().is_none());
		}
	}
	assert_eq!(dec.next_frame().unwrap(), b"dribble");
}
