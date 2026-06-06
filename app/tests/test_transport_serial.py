"""PTY-loopback tests for :class:`SerialTransport`.

Uses a pseudo-terminal pair so the test exercises the real pyserial code
path (framing, write/flush, resyncing read) without any hardware. POSIX
only; skipped on Windows where ``os.openpty`` is unavailable.
"""

from __future__ import annotations

import os
import sys
import threading

import pytest

serial = pytest.importorskip("serial")

from touchy_pad.transport import _FrameDecoder, _pack  # noqa: E402
from touchy_pad.transport_serial import BAUD_RATE, SerialTransport  # noqa: E402

if not hasattr(os, "openpty"):  # pragma: no cover - platform dependent
    pytest.skip("openpty unavailable on this platform", allow_module_level=True)

if sys.platform == "darwin":  # pragma: no cover - macOS CI has no serial hardware
    # macOS PTY + pyserial can hang in tcdrain() on close; serial hardware
    # is never present in CI anyway.
    pytest.skip("serial PTY tests skipped on macOS", allow_module_level=True)


def _pty_port() -> tuple[int, str]:
    """Open a pty pair, returning (master_fd, slave_device_path)."""
    master_fd, slave_fd = os.openpty()
    slave_name = os.ttyname(slave_fd)
    os.close(slave_fd)  # pyserial reopens it by name
    return master_fd, slave_name


def test_baud_rate_constant() -> None:
    assert BAUD_RATE == 460800


def test_send_command_writes_a_frame() -> None:
    master_fd, slave_name = _pty_port()
    try:
        transport = SerialTransport(slave_name, timeout_ms=1000)
        try:
            transport.send_command(b"ping")
            # Read what landed on the master side and decode it.
            dec = _FrameDecoder()
            dec.feed(os.read(master_fd, 4096))
            assert dec.next_frame() == b"ping"
        finally:
            transport.close()
    finally:
        os.close(master_fd)


def test_recv_response_decodes_a_frame() -> None:
    master_fd, slave_name = _pty_port()
    try:
        transport = SerialTransport(slave_name, timeout_ms=2000)
        try:
            # Echo a framed reply from the master side in a worker thread.
            def _reply() -> None:
                os.write(master_fd, _pack(b"pong"))

            threading.Thread(target=_reply, daemon=True).start()
            assert transport.recv_response() == b"pong"
        finally:
            transport.close()
    finally:
        os.close(master_fd)


def test_recv_response_resyncs_past_garbage() -> None:
    master_fd, slave_name = _pty_port()
    try:
        transport = SerialTransport(slave_name, timeout_ms=2000)
        try:

            def _reply() -> None:
                # Boot-log noise then a valid frame.
                os.write(master_fd, b"boot: hello world\r\n" + _pack(b"ok"))

            threading.Thread(target=_reply, daemon=True).start()
            assert transport.recv_response() == b"ok"
        finally:
            transport.close()
    finally:
        os.close(master_fd)
