"""Tests for the public :mod:`touchy_pad.api` facade.

Exercises :class:`Touchy` (lifecycle, ``screen_save`` overloads, event
callback dispatch) and the firmware-version compatibility check, all
backed by an in-process loopback transport so no USB device is needed.
"""

from __future__ import annotations

import json
import logging
import queue
import threading
import time
from pathlib import Path

import pytest

from touchy_pad import _proto
from touchy_pad.api import (
    MINIMUM_FIRMWARE_VERSION,
    IncompatibleFirmwareError,
    Screen,
    Touchy,
    TouchyClient,
    TouchyError,
    button,
    protobuf,
    touchy_open,
)
from touchy_pad.api._transport import Transport

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


class DesyncFirstTransport(Transport):
    """Returns a leftover frame first, then normal replies (killed-host sim).

    Models the device's reply FIFO after a host was hard-killed mid-RPC: the
    orphaned frame is read as the answer to the *next* command.
    """

    def __init__(self, server, stale: list[bytes]) -> None:
        self._inner = FakeTransport(server)
        self._stale = list(stale)

    def send_command(self, payload: bytes) -> None:
        self._inner.send_command(payload)

    def recv_response(self, timeout_ms: int = 2000) -> bytes:
        if self._stale:
            return self._stale.pop(0)
        return self._inner.recv_response(timeout_ms)

    def close(self) -> None:
        self._inner.close()


class _ForeignOkForever(Transport):
    """Every read returns a foreign ``OK`` — a permanently desynced FIFO."""

    def send_command(self, payload: bytes) -> None:
        pass

    def recv_response(self, timeout_ms: int = 2000) -> bytes:
        return _proto.Response(code=_proto.RESULT_OK).SerializeToString()

    def close(self) -> None:
        pass


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
        if kind == "set_preferences":
            # Stage 82 — screen switching goes through SetPreferencesCmd.
            if cmd.set_preferences.prefs.HasField("current_screen"):
                loaded.append(cmd.set_preferences.prefs.current_screen)
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


def test_touchy_open_retries_a_default_board_info_reply():
    """A leftover OK reply parses as a default board — flush and retry once."""
    server = _make_server()
    stale = [_proto.Response(code=_proto.RESULT_OK).SerializeToString()]
    pad = touchy_open(transport=DesyncFirstTransport(server, stale))
    try:
        assert pad.board_info.protocol_version == MINIMUM_FIRMWARE_VERSION
        assert pad.board_info.board_name == "test_board"
    finally:
        pad.close()


def test_touchy_open_rejects_repeated_default_board_info():
    """Two default replies mean the FIFO is desynced — fail loudly."""
    with pytest.raises(TouchyError) as info:
        touchy_open(transport=_ForeignOkForever())
    assert "desynchronised" in str(info.value)


def test_screen_save_accepts_host_dsl(open_pad):
    pad, server = open_pad()
    s = Screen("home")
    s += button("go", text="Go")
    pad.screen_save(s)
    assert "F:host/s/home.pb" in server.saved


def test_screen_save_accepts_protobuf_message(open_pad):
    pad, server = open_pad()
    msg = protobuf.Screen()
    msg.active.version = protobuf.Widget.Version.CURRENT
    pad.screen_save(msg, name="raw")
    assert "F:host/s/raw.pb" in server.saved


def test_screen_save_accepts_dict(open_pad):
    pad, server = open_pad()
    pad.screen_save({"active": {"version": "CURRENT"}}, name="dictish")
    assert "F:host/s/dictish.pb" in server.saved


def test_screen_save_accepts_json_path(open_pad, tmp_path: Path):
    pad, server = open_pad()
    p = tmp_path / "pathish.json"
    p.write_text(json.dumps({"active": {"version": "CURRENT"}}))
    pad.screen_save(p, name="pathish")
    assert "F:host/s/pathish.pb" in server.saved


def test_screen_save_name_override(open_pad):
    pad, server = open_pad()
    s = Screen("default")
    pad.screen_save(s, name="renamed")
    assert "F:host/s/renamed.pb" in server.saved
    assert "F:host/s/default.pb" not in server.saved


def test_screen_save_requires_a_name(open_pad):
    pad, _ = open_pad()
    with pytest.raises(ValueError):
        pad.screen_save(protobuf.Screen())


def test_screen_load_and_file_reset_wrappers(open_pad):
    pad, server = open_pad()
    pad.file_reset()
    pad.screen_load("F:host/s/home.pb")
    assert server.loaded == ["F:host/s/home.pb"]


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


def test_screen_save_registers_inline_callback(open_pad):
    """Stage 67: host_action(on_event=...) wires up automatically on upload."""
    from touchy_pad.api import _events, host_action

    _events._reset()
    seen: list[object] = []
    done = threading.Event()

    pad, server = open_pad()
    s = Screen("inline")
    s += button(
        "go",
        text="Go",
        on_click=host_action(
            on_event=lambda e: (seen.append(e.user_data), done.set()),
        ),
    )
    # Capture the auto-allocated code from the built screen.
    proto = s.to_proto()
    code = pad._collect_host_codes(proto).pop()
    assert code >= _events.AUTO_CODE_BASE

    pad.screen_save(s)
    # After upload the pending binding is harvested (no longer pending).
    assert _events.harvest({code}) == {}

    server.events.put(_proto.LvEvent(host_code=code, user_data="go"))
    assert done.wait(timeout=2.0), "inline callback was never invoked"
    assert seen == ["go"]


def test_widget_save_registers_inline_callback(open_pad):
    """Inline callbacks on a standalone widget are harvested by widget_save."""
    from touchy_pad.api import _events, host_action

    _events._reset()
    fired = threading.Event()

    pad, server = open_pad()
    w = button(
        "ping",
        text="Ping",
        on_click=host_action(on_event=lambda e: fired.set()),
    )
    code = pad._collect_host_codes(w).pop()
    pad.widget_save("ping", w)
    assert _events.harvest({code}) == {}

    server.events.put(_proto.LvEvent(host_code=code))
    assert fired.wait(timeout=2.0)


def test_user_screen_save_writes_to_user_screens_dir(open_pad):
    """user_screen_save uploads page bodies under F:host/uscr/."""
    pad, server = open_pad()
    w = button("pad", text="Pad")
    path = pad.user_screen_save("trackpad", w)
    assert path == "F:host/uscr/trackpad.pb"
    assert "F:host/uscr/trackpad.pb" in server.saved


def test_user_screen_save_registers_inline_callback(open_pad):
    """Inline callbacks on a page body are harvested by user_screen_save."""
    from touchy_pad.api import _events, host_action

    _events._reset()
    fired = threading.Event()

    pad, server = open_pad()
    w = button(
        "ping",
        text="Ping",
        on_click=host_action(on_event=lambda e: fired.set()),
    )
    code = pad._collect_host_codes(w).pop()
    pad.user_screen_save("page1", w)
    assert _events.harvest({code}) == {}

    server.events.put(_proto.LvEvent(host_code=code))
    assert fired.wait(timeout=2.0)


# ---------------------------------------------------------------------------
# background event-thread health (``event_thread_alive``)
# ---------------------------------------------------------------------------


def _wait_for(predicate, *, timeout: float = 2.0) -> bool:
    """Poll *predicate* until it is true or *timeout* expires."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.01)
    return predicate()


class _IdleClient:
    """Client stub whose event stream ends immediately (no events, no error)."""

    def __init__(self) -> None:
        self.closed = False

    def stream_events(self):
        return iter(())

    def close(self) -> None:
        self.closed = True


def _device_errors(caplog: pytest.LogCaptureFixture) -> list[logging.LogRecord]:
    return [r for r in caplog.records if r.name == "touchy_pad.api.device"]


def test_event_thread_alive_while_reader_runs(open_pad):
    pad, server = open_pad()
    seen = threading.Event()
    pad.on_host_event(0x300, lambda evt: seen.set())

    assert pad.event_thread_alive is True

    server.events.put(_proto.LvEvent(host_code=0x300))
    assert seen.wait(timeout=2.0), "event was never dispatched"
    assert pad.event_thread_alive is True


def test_transport_failure_stops_reader_and_logs_traceback(caplog):
    transport = FakeTransport(_make_server())
    pad = touchy_open(transport=transport)
    try:
        assert pad.event_thread_alive is True
        with caplog.at_level(logging.ERROR, logger="touchy_pad.api.device"):
            # Drop the link underneath the reader, as a yanked cable would.
            transport.close()
            assert _wait_for(
                lambda: not pad.event_thread_alive
            ), "reader did not stop after the transport failed"

        fatal = [r for r in _device_errors(caplog) if "stopped unexpectedly" in r.getMessage()]
        assert fatal, "the fatal reader exit was not logged"
        assert fatal[0].exc_info is not None, "the log record has no traceback"
    finally:
        pad.close()


def test_clean_stream_end_marks_reader_dead_without_a_fatal_log(caplog):
    pad = Touchy(_IdleClient(), start_event_thread=True)
    try:
        with caplog.at_level(logging.ERROR, logger="touchy_pad.api.device"):
            assert _wait_for(
                lambda: not pad.event_thread_alive
            ), "reader did not report itself dead after the stream ended"
        assert not [r for r in _device_errors(caplog) if "stopped unexpectedly" in r.getMessage()]
    finally:
        pad.close()


def test_close_is_quiet_and_marks_reader_dead(caplog):
    pad = touchy_open(transport=FakeTransport(_make_server()))
    assert pad.event_thread_alive is True

    with caplog.at_level(logging.ERROR, logger="touchy_pad.api.device"):
        pad.close()

    assert pad.event_thread_alive is False
    assert not [
        r for r in _device_errors(caplog) if "stopped unexpectedly" in r.getMessage()
    ], "a normal close must not log a fatal reader error"

    pad.close()  # repeated close keeps working
    assert pad.event_thread_alive is False


def test_event_thread_alive_is_false_when_reader_disabled():
    pad = Touchy(TouchyClient(FakeTransport(_make_server())), start_event_thread=False)
    try:
        assert pad.event_thread_alive is False
    finally:
        pad.close()


def test_failing_callback_is_logged_and_reader_survives(open_pad, caplog):
    pad, server = open_pad()

    def boom(evt):
        raise RuntimeError("callback exploded")

    later = threading.Event()
    pad.on_host_event(0x400, boom)
    pad.on_host_event(0x401, lambda evt: later.set())

    with caplog.at_level(logging.ERROR, logger="touchy_pad.api.device"):
        server.events.put(_proto.LvEvent(host_code=0x400))
        assert _wait_for(
            lambda: any("callback failed" in r.getMessage() for r in _device_errors(caplog))
        ), "the callback failure was not logged"

        # The reader survives the failure and dispatches the next event.
        server.events.put(_proto.LvEvent(host_code=0x401))
        assert later.wait(timeout=2.0), "the reader stopped after a callback raised"

    assert pad.event_thread_alive is True
    failure = [r for r in _device_errors(caplog) if "callback failed" in r.getMessage()][0]
    assert failure.exc_info is not None
    assert "callback exploded" in str(failure.exc_info[1])
