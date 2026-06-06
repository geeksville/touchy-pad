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


# ---------------------------------------------------------------------------
# Stage 83 — UART-bridge discovery
# ---------------------------------------------------------------------------


class _FakePort:
    def __init__(self, device: str, vid: int | None, pid: int | None) -> None:
        self.device = device
        self.vid = vid
        self.pid = pid


def test_discover_keeps_known_vid_pid(monkeypatch, tmp_path) -> None:
    from serial.tools import list_ports

    from touchy_pad import transport_serial as ts

    accessible = tmp_path / "ttyUSB0"
    accessible.write_bytes(b"")
    accessible.chmod(0o600)

    monkeypatch.setattr(
        list_ports,
        "comports",
        lambda: [_FakePort(str(accessible), 0x1A86, 0x7523)],
    )
    # Disable the /host/dev fallback so this test is platform-independent.
    monkeypatch.setattr(ts, "_HOST_DEV_DIR", tmp_path / "no-such-dir")

    assert ts.discover_serial_ports() == [str(accessible)]


def test_discover_drops_unknown_vid_pid(monkeypatch, tmp_path) -> None:
    from serial.tools import list_ports

    from touchy_pad import transport_serial as ts

    p = tmp_path / "ttyOther"
    p.write_bytes(b"")
    p.chmod(0o600)

    monkeypatch.setattr(
        list_ports,
        "comports",
        lambda: [_FakePort(str(p), 0x0403, 0x6001)],  # FTDI — not in table
    )
    monkeypatch.setattr(ts, "_HOST_DEV_DIR", tmp_path / "no-such-dir")

    assert ts.discover_serial_ports() == []


def test_discover_drops_inaccessible_node(monkeypatch, tmp_path) -> None:
    from serial.tools import list_ports

    from touchy_pad import transport_serial as ts

    monkeypatch.setattr(
        list_ports,
        "comports",
        lambda: [_FakePort("/dev/does-not-exist-touchy-test", 0x1A86, 0x7523)],
    )
    monkeypatch.setattr(ts, "_HOST_DEV_DIR", tmp_path / "no-such-dir")

    assert ts.discover_serial_ports() == []


def test_discover_host_dev_fallback(monkeypatch, tmp_path) -> None:
    from serial.tools import list_ports

    from touchy_pad import transport_serial as ts

    fake_host_dev = tmp_path / "host-dev"
    fake_host_dev.mkdir()
    node = fake_host_dev / "ttyUSB0"
    node.write_bytes(b"")
    node.chmod(0o600)

    monkeypatch.setattr(list_ports, "comports", lambda: [])
    monkeypatch.setattr(ts, "_HOST_DEV_DIR", fake_host_dev)

    assert ts.discover_serial_ports() == [str(node)]
