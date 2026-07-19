"""Stage lb12 — SetPropertyCmd proto + TouchyClient.set_property tests."""

from __future__ import annotations

import queue

import pytest

from touchy_pad import _proto
from touchy_pad.api import Color, Point, TouchyClient
from touchy_pad.api._transport import Transport


class _CaptureTransport(Transport):
    """Captures the last Command sent and replies OK."""

    def __init__(self):
        self.last: _proto.Command | None = None
        self._responses: queue.Queue[bytes] = queue.Queue()

    def send_command(self, payload: bytes) -> None:
        cmd = _proto.Command()
        cmd.ParseFromString(payload)
        self.last = cmd
        self._responses.put(_proto.Response(code=_proto.RESULT_OK).SerializeToString())

    def recv_response(self, timeout_ms: int = 2000) -> bytes:
        return self._responses.get(timeout=timeout_ms / 1000.0)

    def close(self) -> None:
        pass


def _sent(prop, value) -> _proto.SetPropertyCmd:
    t = _CaptureTransport()
    with TouchyClient(t) as c:
        c.set_property("welcome", prop, value)
    assert t.last is not None
    assert t.last.WhichOneof("cmd") == "set_property"
    return t.last.set_property


# ---- proto round-trip ------------------------------------------------------


def test_setproperty_roundtrip_value_oneof():
    cmd = _proto.SetPropertyCmd(widget_id="w", property_name="text", string_value="hi")
    again = _proto.SetPropertyCmd()
    again.ParseFromString(cmd.SerializeToString())
    assert again.widget_id == "w"
    assert again.WhichOneof("property") == "property_name"
    assert again.property_name == "text"
    assert again.WhichOneof("value") == "string_value"
    assert again.string_value == "hi"


def test_setproperty_unset_value_means_remove():
    cmd = _proto.SetPropertyCmd(widget_id="w", property_name="text")
    again = _proto.SetPropertyCmd()
    again.ParseFromString(cmd.SerializeToString())
    assert again.WhichOneof("value") is None


# ---- client value-type mapping --------------------------------------------


def test_set_property_name_vs_id():
    assert _sent("text", "hi").WhichOneof("property") == "property_name"
    by_id = _sent(0x0300, "hi")
    assert by_id.WhichOneof("property") == "property_id"
    assert by_id.property_id == 0x0300


def test_set_property_bool_maps_bool():
    sp = _sent("clickable", True)
    assert sp.WhichOneof("value") == "bool_value"
    assert sp.bool_value is True


def test_set_property_int_maps_int():
    sp = _sent("value", 42)
    assert sp.WhichOneof("value") == "int_value"
    assert sp.int_value == 42


def test_set_property_str_maps_string():
    sp = _sent("text", "New message")
    assert sp.WhichOneof("value") == "string_value"
    assert sp.string_value == "New message"


def test_set_property_color_maps_color():
    sp = _sent("bg_color", Color(0xFF8800))
    assert sp.WhichOneof("value") == "color_value"
    assert sp.color_value == 0xFF8800


def test_set_property_point_maps_point():
    sp = _sent("pos", Point(3, 7))
    assert sp.WhichOneof("value") == "point_value"
    assert (sp.point_value.x, sp.point_value.y) == (3, 7)


def test_set_property_none_removes():
    sp = _sent("text", None)
    assert sp.WhichOneof("value") is None
    assert sp.WhichOneof("property") == "property_name"


def test_set_property_rejects_bad_prop_type():
    with pytest.raises(TypeError):
        _sent(1.5, "x")


def test_set_property_rejects_bad_value_type():
    with pytest.raises(TypeError):
        _sent("text", 1.5)


# ---- simulator ignores it --------------------------------------------------


def test_sim_set_property_warns_and_oks(caplog, tmp_path):
    from touchy_pad.sim.device import SimDevice
    from touchy_pad.sim.fs import SimFs

    dev = SimDevice(SimFs(tmp_path, "sim"))
    cmd = _proto.Command(
        set_property=_proto.SetPropertyCmd(
            widget_id="welcome", property_name="text", string_value="x"
        )
    )
    import logging

    with caplog.at_level(logging.WARNING):
        reply = _proto.Response.FromString(dev.handle_command(cmd.SerializeToString()))
    assert reply.code == _proto.RESULT_OK
    assert any("set_property ignored" in r.message for r in caplog.records)
