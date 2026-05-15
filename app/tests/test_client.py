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
        self._events: queue.Queue[bytes] = queue.Queue()

    def send_command(self, payload: bytes) -> None:
        cmd = _proto.Command()
        cmd.ParseFromString(payload)
        reply = self._server(cmd, self)
        self._responses.put(reply.SerializeToString())

    def recv_response(self, timeout_ms: int = 2000) -> bytes:
        return self._responses.get(timeout=timeout_ms / 1000.0)

    def recv_event(self, timeout_ms: int = 0) -> bytes | None:
        try:
            return self._events.get(timeout=timeout_ms / 1000.0)
        except queue.Empty:
            return None

    def push_event(self, event: _proto.Event) -> None:
        self._events.put(event.SerializeToString())

    def close(self) -> None:
        pass


@pytest.fixture
def make_client():
    def factory(server):
        return TouchyClient(LoopbackTransport(server))

    return factory


def test_sys_version_get(make_client):
    def server(cmd, _t):
        assert cmd.WhichOneof("cmd") == "sys_version_get"
        return _proto.Response(
            code=_proto.RESULT_OK,
            sys_version=_proto.SysVersionResponse(
                protocol_version=1,
                firmware_version=42,
                firmware_version_str="0.0.42-test",
            ),
        )

    with make_client(server) as c:
        v = c.sys_version_get()
        assert v.protocol_version == 1
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
        payloads["path"] = cmd.xml_save.path
        payloads["xml"] = cmd.xml_save.xml
        return _proto.Response(code=_proto.RESULT_OK)

    with make_client(server) as c:
        c.xml_save("screens/home.xml", "<view/>")
    assert payloads == {"path": "screens/home.xml", "xml": "<view/>"}


def test_image_save_binary_round_trip(make_client):
    data_seen = {}

    def server(cmd, _t):
        data_seen["data"] = cmd.image_save.data
        return _proto.Response(code=_proto.RESULT_OK)

    blob = bytes(range(256)) * 4  # 1 KB of arbitrary bytes
    with make_client(server) as c:
        c.image_save("img/test.png", blob)
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
                event=_proto.Event(lv=_proto.LvEvent(code=7, user_data="btn1")),
            ),
        )

    with make_client(server) as c:
        evt = c.event_consume()
        assert evt is not None
        assert evt.lv.code == 7
        assert evt.lv.user_data == "btn1"


def test_device_error_raises_touchy_error(make_client):
    from touchy_pad.client import TouchyError

    def server(cmd, _t):
        return _proto.Response(code=_proto.RESULT_INVALID_ARG)

    with make_client(server) as c:
        with pytest.raises(TouchyError) as exc:
            c.screen_wake()
        assert exc.value.code == _proto.RESULT_INVALID_ARG
        assert exc.value.code_name == "RESULT_INVALID_ARG"


def test_stream_events_inline_event(make_client):
    transport = LoopbackTransport(lambda cmd, t: _proto.Response(code=_proto.RESULT_OK))
    transport.push_event(_proto.Event(lv=_proto.LvEvent(code=1, user_data="hi")))
    client = TouchyClient(transport)
    it = client.stream_events(poll_timeout_ms=10)
    evt = next(it)
    assert evt.lv.user_data == "hi"


def test_stream_events_event_ready_triggers_consume(make_client):
    drained_event = _proto.Event(lv=_proto.LvEvent(code=9, user_data="async"))

    def server(cmd, _t):
        assert cmd.WhichOneof("cmd") == "event_consume"
        return _proto.Response(
            code=_proto.RESULT_OK,
            event_consume=_proto.EventConsumeResponse(event=drained_event),
        )

    transport = LoopbackTransport(server)
    transport.push_event(_proto.Event(event_ready=True))
    client = TouchyClient(transport)
    it = client.stream_events(poll_timeout_ms=10)
    evt = next(it)
    assert evt.lv.code == 9
    assert evt.lv.user_data == "async"
