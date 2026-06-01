"""Round-trip + resync tests for the Stage 64.3 self-synchronising frame."""

from __future__ import annotations

import pytest

from touchy_pad.transport import (
    _MAGIC,
    _MAX_FRAME,
    TransportError,
    _crc8,
    _FrameDecoder,
    _pack,
    _unpack,
)

# MAGIC(2) + LEN(2) + CRC8(1)
_OVERHEAD = 5


def test_round_trip_small() -> None:
    payload = b"hello, touchy"
    frame = _pack(payload)
    assert len(frame) == _OVERHEAD + len(payload)
    assert frame[:2] == _MAGIC
    assert _unpack(frame) == payload


def test_round_trip_empty() -> None:
    frame = _pack(b"")
    # MAGIC, LEN=0, CRC over the two zero LEN bytes.
    assert frame == _MAGIC + b"\x00\x00" + bytes((_crc8(b"\x00\x00"),))
    assert _unpack(frame) == b""


def test_pack_rejects_oversize() -> None:
    with pytest.raises(TransportError):
        _pack(b"\x00" * (_MAX_FRAME + 1))


def test_unpack_rejects_bad_crc() -> None:
    frame = bytearray(_pack(b"data"))
    frame[-1] ^= 0xFF
    with pytest.raises(TransportError):
        _unpack(bytes(frame))


def test_unpack_rejects_missing_magic() -> None:
    frame = bytearray(_pack(b"data"))
    frame[0] = 0x00
    with pytest.raises(TransportError):
        _unpack(bytes(frame))


def test_unpack_rejects_short_frame() -> None:
    with pytest.raises(TransportError):
        _unpack(_MAGIC + b"\x00")


def test_unpack_rejects_truncated() -> None:
    # LEN says 8 but only 3 payload bytes follow.
    frame = _MAGIC + b"\x08\x00" + b"\x01\x02\x03"
    with pytest.raises(TransportError):
        _unpack(frame)


def test_decoder_round_trip_multiple() -> None:
    dec = _FrameDecoder()
    dec.feed(_pack(b"one") + _pack(b"two") + _pack(b"three"))
    assert dec.next_frame() == b"one"
    assert dec.next_frame() == b"two"
    assert dec.next_frame() == b"three"
    assert dec.next_frame() is None


def test_decoder_resyncs_after_garbage() -> None:
    dec = _FrameDecoder()
    dec.feed(b"\xde\xad\xbe\xef")
    dec.feed(_pack(b"payload"))
    assert dec.next_frame() == b"payload"


def test_decoder_resyncs_after_bad_crc() -> None:
    dec = _FrameDecoder()
    bad = bytearray(_pack(b"corrupt"))
    bad[-1] ^= 0xFF
    dec.feed(bytes(bad))
    dec.feed(_pack(b"good"))
    assert dec.next_frame() == b"good"


def test_decoder_handles_byte_at_a_time() -> None:
    frame = _pack(b"dribble")
    dec = _FrameDecoder()
    for i, b in enumerate(frame):
        dec.feed(bytes((b,)))
        if i + 1 < len(frame):
            assert dec.next_frame() is None
    assert dec.next_frame() == b"dribble"


def test_decoder_split_magic_across_chunks() -> None:
    frame = _pack(b"split")
    dec = _FrameDecoder()
    dec.feed(frame[:1])  # only the first magic byte
    assert dec.next_frame() is None
    dec.feed(frame[1:])
    assert dec.next_frame() == b"split"


def test_crc8_known_vector() -> None:
    # CRC-8/SMBUS (poly 0x07, init 0x00) of b"123456789" == 0xF4.
    assert _crc8(b"123456789") == 0xF4
