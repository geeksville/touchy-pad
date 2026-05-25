"""Tests for the public :mod:`touchy_pad.api` facade.

Exercises :class:`Touchy` (lifecycle, ``screen_save`` overloads, event
callback dispatch) and the firmware-version compatibility check, all
backed by an in-process loopback transport so no USB device is needed.
"""

from __future__ import annotations

import json
import queue
import threading
import time
from pathlib import Path

import pytest

from touchy_pad import Transport, _proto
from touchy_pad.api import (
    MINIMUM_FIRMWARE_VERSION,
    IncompatibleFirmwareError,
    Screen,
    Touchy,
    button,
    protobuf,
    touchy_open,
)

# ---------------------------------------------------------------------------
# loopback transport — copy of test_client.LoopbackTransport with a
# configurable event queue so ``stream_events()`` doesn't block forever.
# ---------------------------------------------------------------------------


class FakeTransport(Transport):
    def __init__(self, server) -> None:
        self._server = server
        self._responses: queue.Queue[bytes] = queue.Queue()
        self.closed = False

    def send_command(self, payload: bytes) -> None:
        if self.closed:
            raise OSError("transport closed")
        cmd = _proto.Command()
        cmd.ParseFromString(payload)
        reply = self._server(cmd, self)
        self._responses.put(reply.SerializeToString())

    def recv_response(self, timeout_ms: int = 2000) -> bytes:
        return self._responses.get(timeout=timeout_ms / 1000.0)

    def close(self) -> None:
        self.closed = True


def _make_server(*, protocol_version: int = MINIMUM_FIRMWARE_VERSION):
    """Return a server callback that handles the basic command set."""
    saved: dict[str, bytes] = {}
    events: queue.Queue[_proto.LvEvent] = queue.Queue()
    loaded: list[str] = []
    # Streaming-write transaction state: handle → (path, accumulated bytes).
    writes: dict[int, tuple[str, bytearray]] = {}
    next_handle = [1]

    def server(cmd, _t):
        kind = cmd.WhichOneof("cmd")
        if kind == "sys_board_info_get":
            return _proto.Response(
                code=_proto.RESULT_OK,
                sys_board_info=_proto.SysBoardInfoResponse(
                    protocol_version=protocol_version,
                    firmware_version=1,
                    firmware_version_str="test",
                    board_name="test_board",
                ),
            )
        if kind == "file_open_write":
            h = next_handle[0]
            next_handle[0] += 1
            writes[h] = (cmd.file_open_write.path, bytearray())
            return _proto.Response(
                code=_proto.RESULT_OK,
                file_open_write=_proto.FileOpenWriteResponse(handle=h),
            )
        if kind == "file_write":
            entry = writes.get(cmd.file_write.handle)
            if entry is None:
                return _proto.Response(code=_proto.RESULT_IO_ERROR)
            entry[1].extend(cmd.file_write.data)
            return _proto.Response(code=_proto.RESULT_OK)
        if kind == "file_close":
            entry = writes.pop(cmd.file_close.handle, None)
            if entry is None:
                return _proto.Response(code=_proto.RESULT_IO_ERROR)
            if cmd.file_close.commit:
                saved[entry[0]] = bytes(entry[1])
            return _proto.Response(code=_proto.RESULT_OK)
        if kind == "file_delete":
            path = cmd.file_delete.path
            # Treat `"F:host"` (or any prefix path) as a subtree wipe.
            removed = [k for k in saved if k == path or k.startswith(path + "/")]
            for k in removed:
                saved.pop(k, None)
            return _proto.Response(code=_proto.RESULT_OK)
        if kind == "screen_load":
            loaded.append(cmd.screen_load.path)
            return _proto.Response(code=_proto.RESULT_OK)
        if kind == "event_consume":
            try:
                evt = events.get_nowait()
            except queue.Empty:
                return _proto.Response(code=_proto.RESULT_NOT_FOUND)
            return _proto.Response(
                code=_proto.RESULT_OK,
                event_consume=_proto.EventConsumeResponse(event=evt),
            )
        return _proto.Response(code=_proto.RESULT_OK)

    server.saved = saved  # type: ignore[attr-defined]
    server.events = events  # type: ignore[attr-defined]
    server.loaded = loaded  # type: ignore[attr-defined]
    return server


@pytest.fixture
def open_pad():
    """Factory that opens a :class:`Touchy` against a fresh fake server."""
    pads: list[Touchy] = []

    def factory(*, protocol_version: int = MINIMUM_FIRMWARE_VERSION):
        server = _make_server(protocol_version=protocol_version)
        pad = touchy_open(transport=FakeTransport(server))
        pads.append(pad)
        return pad, server

    yield factory

    for pad in pads:
        pad.close()


# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------


def test_open_returns_touchy(open_pad):
    pad, _ = open_pad()
    assert isinstance(pad, Touchy)


def test_context_manager_closes(open_pad):
    server = _make_server()
    t = FakeTransport(server)
    with touchy_open(transport=t) as pad:
        assert isinstance(pad, Touchy)
    assert t.closed is True


def test_firmware_too_old_raises():
    too_old = max(0, MINIMUM_FIRMWARE_VERSION - 1)
    if too_old == MINIMUM_FIRMWARE_VERSION:
        pytest.skip("MINIMUM_FIRMWARE_VERSION is 0 — no 'older' value to test")
    server = _make_server(protocol_version=too_old)
    with pytest.raises(IncompatibleFirmwareError):
        touchy_open(transport=FakeTransport(server))


def test_screen_save_accepts_host_dsl(open_pad):
    pad, server = open_pad()
    s = Screen("home")
    s += button("go", text="Go")
    pad.screen_save(s)
    assert "F:host/screens/home.pb" in server.saved


def test_screen_save_accepts_protobuf_message(open_pad):
    pad, server = open_pad()
    msg = protobuf.Screen(version=protobuf.Screen.Version.CURRENT)
    pad.screen_save(msg, name="raw")
    assert "F:host/screens/raw.pb" in server.saved


def test_screen_save_accepts_dict(open_pad):
    pad, server = open_pad()
    pad.screen_save({"version": "CURRENT"}, name="dictish")
    assert "F:host/screens/dictish.pb" in server.saved


def test_screen_save_accepts_json_path(open_pad, tmp_path: Path):
    pad, server = open_pad()
    p = tmp_path / "pathish.json"
    p.write_text(json.dumps({"version": "CURRENT"}))
    pad.screen_save(p, name="pathish")
    assert "F:host/screens/pathish.pb" in server.saved


def test_screen_save_name_override(open_pad):
    pad, server = open_pad()
    s = Screen("default")
    pad.screen_save(s, name="renamed")
    assert "F:host/screens/renamed.pb" in server.saved
    assert "F:host/screens/default.pb" not in server.saved


def test_screen_save_requires_a_name(open_pad):
    pad, _ = open_pad()
    with pytest.raises(ValueError):
        pad.screen_save(protobuf.Screen())


def test_screen_load_and_file_reset_wrappers(open_pad):
    pad, server = open_pad()
    pad.file_reset()
    pad.screen_load("F:host/screens/home.pb")
    assert server.loaded == ["F:host/screens/home.pb"]


def test_on_host_event_dispatches_callback(open_pad):
    pad, server = open_pad()
    seen: list[int] = []
    done = threading.Event()

    def cb(evt):
        seen.append(evt.host_code)
        done.set()

    pad.on_host_event(0x100, cb)
    server.events.put(_proto.LvEvent(host_code=0x100, user_data="btn"))

    assert done.wait(timeout=2.0), "callback was never invoked"
    assert seen == [0x100]


def test_on_host_event_supports_multiple_callbacks(open_pad):
    pad, server = open_pad()
    a: list[int] = []
    b: list[int] = []
    barrier = threading.Event()

    def cb_a(evt):
        a.append(evt.host_code)

    def cb_b(evt):
        b.append(evt.host_code)
        barrier.set()

    pad.on_host_event(0x200, cb_a)
    pad.on_host_event(0x200, cb_b)
    server.events.put(_proto.LvEvent(host_code=0x200))

    assert barrier.wait(timeout=2.0)
    # Give cb_a a moment if it ran after cb_b for any reason.
    time.sleep(0.05)
    assert a == [0x200]
    assert b == [0x200]
