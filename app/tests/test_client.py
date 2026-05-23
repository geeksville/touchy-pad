"""In-process loopback transport + round-trip tests for ``TouchyClient``.

These tests exercise the command/response encoding without needing a real
USB device. A trivial fake server reads framed Commands from the host
transport and replies with the matching Response.
"""

from __future__ import annotations

import queue

import pytest

from touchy_pad import TouchyClient, Transport, _proto


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
        seen["timeout"] = cmd.screen_sleep_timeout.timeout_ms
        return _proto.Response(code=_proto.RESULT_OK)

    with make_client(server) as c:
        c.screen_sleep_timeout(5000)
    assert seen == {"timeout": 5000}


def test_xml_save_round_trip(make_client):
    payloads = {}

    def server(cmd, _t):
        payloads["path"] = cmd.file_save.path
        payloads["data"] = cmd.file_save.data
        return _proto.Response(code=_proto.RESULT_OK)

    with make_client(server) as c:
        c.file_save("screens/home.xml", "<view/>")
    assert payloads == {"path": "screens/home.xml", "data": b"<view/>"}


def test_image_save_binary_round_trip(make_client):
    data_seen = {}

    def server(cmd, _t):
        data_seen["data"] = cmd.file_save.data
        return _proto.Response(code=_proto.RESULT_OK)

    blob = bytes(range(256)) * 4  # 1 KB of arbitrary bytes
    with make_client(server) as c:
        c.file_save("img/test.png", blob)
    assert data_seen["data"] == blob


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
    from touchy_pad.client import TouchyError

    def server(cmd, _t):
        return _proto.Response(code=_proto.RESULT_INVALID_ARG)

    with make_client(server) as c:
        with pytest.raises(TouchyError) as exc:
            c.screen_wake()
        assert exc.value.code == _proto.RESULT_INVALID_ARG
        assert exc.value.code_name == "RESULT_INVALID_ARG"


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
