"""In-process loopback transport + round-trip tests for ``TouchyClient``.

These tests exercise the command/response encoding without needing a real
USB device. A trivial fake server reads framed Commands from the host
transport and replies with the matching Response.
"""

from __future__ import annotations

import queue

import pytest

from touchy_pad import _proto
from touchy_pad.api import TouchyClient, TouchyError
from touchy_pad.api._transport import Transport


class LoopbackTransport(Transport):
    """A Transport that hands commands to a callback and queues replies."""

    def __init__(self, server):
        self._server = server
        self._responses: queue.Queue[bytes] = queue.Queue()

    def send_command(self, payload: bytes) -> None:
        cmd = _proto.Command()
        cmd.ParseFromString(payload)
        reply = self._server(cmd, self)
        self._responses.put(reply.SerializeToString())

    def recv_response(self, timeout_ms: int = 2000) -> bytes:
        return self._responses.get(timeout=timeout_ms / 1000.0)

    def close(self) -> None:
        pass


@pytest.fixture
def make_client():
    def factory(server):
        return TouchyClient(LoopbackTransport(server))

    return factory


def test_sys_version_get(make_client):
    def server(cmd, _t):
        assert cmd.WhichOneof("cmd") == "sys_board_info_get"
        return _proto.Response(
            code=_proto.RESULT_OK,
            sys_board_info=_proto.SysBoardInfoResponse(
                protocol_version=_proto.SysBoardInfoResponse.CURRENT,
                firmware_version=42,
                firmware_version_str="0.0.42-test",
                board_name="test_board",
            ),
        )

    with make_client(server) as c:
        v = c.sys_board_info_get()
        assert v.protocol_version == _proto.SysBoardInfoResponse.CURRENT
        assert v.firmware_version == 42
        assert v.firmware_version_str == "0.0.42-test"


def test_screen_wake_ok(make_client):
    def server(cmd, _t):
        assert cmd.WhichOneof("cmd") == "screen_wake"
        return _proto.Response(code=_proto.RESULT_OK)

    with make_client(server) as c:
        c.screen_wake()


def test_screen_sleep_timeout_arg_passed(make_client):
    seen = {}

    def server(cmd, _t):
        # Stage 82 — screen_sleep_timeout is implemented via SetPreferencesCmd.
        seen["timeout"] = cmd.set_preferences.prefs.screen_timeout_ms
        return _proto.Response(code=_proto.RESULT_OK)

    with make_client(server) as c:
        c.screen_sleep_timeout(5000)
    assert seen == {"timeout": 5000}


def test_set_preferences_only_sets_given_field(make_client):
    """A partial SetPreferencesCmd must carry presence only for set fields."""
    seen = {}

    def server(cmd, _t):
        assert cmd.WhichOneof("cmd") == "set_preferences"
        prefs = cmd.set_preferences.prefs
        seen["has_timeout"] = prefs.HasField("screen_timeout_ms")
        seen["has_screen"] = prefs.HasField("current_screen")
        seen["has_log"] = prefs.HasField("min_log_level")
        seen["has_boot"] = prefs.HasField("boot_delay_s")
        return _proto.Response(code=_proto.RESULT_OK)

    with make_client(server) as c:
        c.set_preferences(_proto.PreferencesFile(screen_timeout_ms=10))
    assert seen == {
        "has_timeout": True,
        "has_screen": False,
        "has_log": False,
        "has_boot": False,
    }


def test_set_min_log_level_passed(make_client):
    seen = {}

    def server(cmd, _t):
        assert cmd.WhichOneof("cmd") == "set_preferences"
        seen["level"] = cmd.set_preferences.prefs.min_log_level
        return _proto.Response(code=_proto.RESULT_OK)

    with make_client(server) as c:
        c.set_min_log_level(_proto.LOG_PRIORITY_DEBUG)
    assert seen == {"level": _proto.LOG_PRIORITY_DEBUG}


def test_set_boot_delay_passed(make_client):
    seen = {}

    def server(cmd, _t):
        assert cmd.WhichOneof("cmd") == "set_preferences"
        seen["delay"] = cmd.set_preferences.prefs.boot_delay_s
        return _proto.Response(code=_proto.RESULT_OK)

    with make_client(server) as c:
        c.set_boot_delay(3)
    assert seen == {"delay": 3}


def test_set_backlight_level_passed(make_client):
    seen = {}

    def server(cmd, _t):
        assert cmd.WhichOneof("cmd") == "set_preferences"
        prefs = cmd.set_preferences.prefs
        seen["level"] = prefs.backlight_level
        seen["has_timeout"] = prefs.HasField("screen_timeout_ms")
        seen["has_boot"] = prefs.HasField("boot_delay_s")
        return _proto.Response(code=_proto.RESULT_OK)

    with make_client(server) as c:
        c.set_backlight_level(50)
    assert seen == {"level": 50, "has_timeout": False, "has_boot": False}


def test_get_preferences_returns_prefs(make_client):
    """Stage LB4 — get_preferences round-trips the device's PreferencesFile."""

    def server(cmd, _t):
        assert cmd.WhichOneof("cmd") == "get_preferences"
        return _proto.Response(
            code=_proto.RESULT_OK,
            preferences_read=_proto.PreferencesReadResponse(
                prefs=_proto.PreferencesFile(
                    file_version=_proto.PreferencesFile.Version.CURRENT,
                    backlight_level=42,
                ),
            ),
        )

    with make_client(server) as c:
        prefs = c.get_preferences()
    assert prefs.file_version == _proto.PreferencesFile.Version.CURRENT
    assert prefs.backlight_level == 42


@pytest.mark.parametrize(
    ("count", "expected"),
    [
        (0, "0 B"),
        (512, "512 B"),
        (1024, "1.0 KiB"),
        (1536, "1.5 KiB"),
        (1048576, "1.0 MiB"),
        (1073741824, "1.0 GiB"),
    ],
)
def test_fmt_bytes(count, expected):
    """Stage 81 — board-info byte formatter renders compact units."""
    from touchy_pad.cli import _fmt_bytes

    assert _fmt_bytes(count) == expected


def test_run_actions_passes_actions(make_client):
    seen = {}

    def server(cmd, _t):
        assert cmd.WhichOneof("cmd") == "run_actions"
        seen["actions"] = list(cmd.run_actions.actions)
        return _proto.Response(code=_proto.RESULT_OK)

    act = _proto.Action(
        device=_proto.ActionDevice(
            change_widget_ref=_proto.ActionChangeWidgetRef(
                behavior=_proto.ActionChangeWidgetRef.BY_PATH,
                target_id="page",
                path="F:host/uscr/opendeck.pb",
            )
        )
    )
    with make_client(server) as c:
        c.run_actions([act])
    assert len(seen["actions"]) == 1
    assert seen["actions"][0].device.change_widget_ref.target_id == "page"


def test_xml_save_round_trip(make_client):
    """``file_save`` should drive the streaming write protocol end-to-end."""
    state = {"buf": bytearray(), "path": None, "handle": 0, "closed": False}

    def server(cmd, _t):
        kind = cmd.WhichOneof("cmd")
        if kind == "file_open_write":
            state["path"] = cmd.file_open_write.path
            state["handle"] = 42
            return _proto.Response(
                code=_proto.RESULT_OK,
                file_open_write=_proto.FileOpenWriteResponse(handle=42),
            )
        if kind == "file_write":
            assert cmd.file_write.handle == 42
            state["buf"].extend(cmd.file_write.data)
            return _proto.Response(code=_proto.RESULT_OK)
        if kind == "file_close":
            assert cmd.file_close.handle == 42
            assert cmd.file_close.commit is True
            state["closed"] = True
            return _proto.Response(code=_proto.RESULT_OK)
        raise AssertionError(f"unexpected command {kind!r}")

    with make_client(server) as c:
        c.file_save("F:host/screens/home.xml", "<view/>")
    assert state["path"] == "F:host/screens/home.xml"
    assert bytes(state["buf"]) == b"<view/>"
    assert state["closed"] is True


def test_image_save_binary_round_trip(make_client):
    """Large blobs are split into 4 KiB chunks and reassembled."""
    state = {"buf": bytearray(), "writes": 0}

    def server(cmd, _t):
        kind = cmd.WhichOneof("cmd")
        if kind == "file_open_write":
            return _proto.Response(
                code=_proto.RESULT_OK,
                file_open_write=_proto.FileOpenWriteResponse(handle=1),
            )
        if kind == "file_write":
            state["writes"] += 1
            state["buf"].extend(cmd.file_write.data)
            return _proto.Response(code=_proto.RESULT_OK)
        if kind == "file_close":
            return _proto.Response(code=_proto.RESULT_OK)
        raise AssertionError(f"unexpected command {kind!r}")

    blob = bytes(range(256)) * 4  # 1 KB of arbitrary bytes
    with make_client(server) as c:
        c.file_save("F:host/img/test.png", blob)
    assert bytes(state["buf"]) == blob
    assert state["writes"] >= 1


def test_gif_save_is_uploaded_verbatim(make_client):
    """GIFs bypass LVGL-bin conversion and keep their ``.gif`` path."""
    import io as _io

    from PIL import Image

    state = {"buf": bytearray(), "path": None}

    def server(cmd, _t):
        kind = cmd.WhichOneof("cmd")
        if kind == "file_open_write":
            state["path"] = cmd.file_open_write.path
            return _proto.Response(
                code=_proto.RESULT_OK,
                file_open_write=_proto.FileOpenWriteResponse(handle=1),
            )
        if kind == "file_write":
            state["buf"].extend(cmd.file_write.data)
            return _proto.Response(code=_proto.RESULT_OK)
        if kind == "file_close":
            return _proto.Response(code=_proto.RESULT_OK)
        raise AssertionError(f"unexpected command {kind!r}")

    buf = _io.BytesIO()
    Image.new("P", (8, 6), 0).save(buf, format="GIF")
    gif = buf.getvalue()

    with make_client(server) as c:
        c.file_save("F:host/images/bg.gif", gif)
    # Path kept verbatim (NOT rewritten to .bin) and bytes uploaded as-is.
    assert state["path"] == "F:host/images/bg.gif"
    assert bytes(state["buf"]) == gif
    assert bytes(state["buf"]).startswith((b"GIF87a", b"GIF89a"))


def test_event_consume_empty_returns_none(make_client):
    def server(cmd, _t):
        assert cmd.WhichOneof("cmd") == "event_consume"
        return _proto.Response(code=_proto.RESULT_NOT_FOUND)

    with make_client(server) as c:
        assert c.event_consume() is None


def test_event_consume_returns_event(make_client):
    def server(cmd, _t):
        return _proto.Response(
            code=_proto.RESULT_OK,
            event_consume=_proto.EventConsumeResponse(
                event=_proto.LvEvent(code=7, user_data="btn1", host_code=0x42),
            ),
        )

    with make_client(server) as c:
        evt = c.event_consume()
        assert evt is not None
        assert evt.code == 7
        assert evt.user_data == "btn1"
        assert evt.host_code == 0x42


def test_device_error_raises_touchy_error(make_client):
    def server(cmd, _t):
        return _proto.Response(code=_proto.RESULT_INVALID_ARG)

    with make_client(server) as c:
        with pytest.raises(TouchyError) as exc:
            c.screen_wake()
        assert exc.value.code == _proto.RESULT_INVALID_ARG
        assert exc.value.code_name == "INVALID_ARG"


def test_stream_events_drains_via_polling(make_client):
    """`stream_events` repeatedly calls EventConsume until the queue empties."""
    queued = [
        _proto.LvEvent(code=1, user_data="a", host_code=0x10),
        _proto.LvEvent(code=2, user_data="b", host_code=0x11),
    ]

    def server(cmd, _t):
        if cmd.WhichOneof("cmd") != "event_consume":
            return _proto.Response(code=_proto.RESULT_OK)
        if not queued:
            return _proto.Response(code=_proto.RESULT_NOT_FOUND)
        evt = queued.pop(0)
        return _proto.Response(
            code=_proto.RESULT_OK,
            event_consume=_proto.EventConsumeResponse(event=evt),
        )

    transport = LoopbackTransport(server)
    client = TouchyClient(transport)

    seen: list[tuple[str, str]] = []
    client.on_host_event(0x10, lambda e: seen.append(("hit10", e.user_data)))
    it = client.stream_events(poll_interval_ms=1)
    evt = next(it)
    assert evt.user_data == "a"
    evt = next(it)
    assert evt.user_data == "b"
    assert seen == [("hit10", "a")]
