"""googly-vr / stage lb14 — SetPropertiesCmd batch tests.

Covers the proto round-trip, the ``build_property_override`` value
mapping, ``TouchyClient.set_properties`` (one RPC for a whole batch), and
the simulator's proto-level sticky-override engine (googly-vr plan E1).
"""

from __future__ import annotations

import logging
import queue

import pytest

from touchy_pad import _proto
from touchy_pad.api import Color, Point, TouchyClient, build_property_override
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


def _sent(entries) -> _proto.SetPropertiesCmd:
    t = _CaptureTransport()
    with TouchyClient(t) as c:
        c.set_properties(entries)
    assert t.last is not None
    assert t.last.WhichOneof("cmd") == "set_properties"
    return t.last.set_properties


def _ov(prop, value) -> _proto.SetPropertyCmd:
    return build_property_override("welcome", prop, value)


# ---- proto round-trip -------------------------------------------------------


def test_setproperties_batch_roundtrip():
    cmd = _proto.SetPropertiesCmd(
        props=[
            _proto.SetPropertyCmd(widget_id="a", property_name="x", int_value=1),
            _proto.SetPropertyCmd(widget_id="b", property_name="text", string_value="hi"),
        ]
    )
    again = _proto.SetPropertiesCmd()
    again.ParseFromString(cmd.SerializeToString())
    assert len(again.props) == 2
    assert again.props[0].widget_id == "a"
    assert again.props[0].int_value == 1
    assert again.props[1].WhichOneof("value") == "string_value"


def test_setproperties_unset_value_means_remove():
    cmd = _proto.SetPropertyCmd(widget_id="w", property_name="text")
    again = _proto.SetPropertyCmd()
    again.ParseFromString(cmd.SerializeToString())
    assert again.WhichOneof("value") is None


# ---- build_property_override value mapping ----------------------------------


def test_override_name_vs_id():
    assert _ov("text", "hi").WhichOneof("property") == "property_name"
    by_id = build_property_override("welcome", 0x0300, "hi")
    assert by_id.WhichOneof("property") == "property_id"
    assert by_id.property_id == 0x0300


def test_override_bool_maps_bool():
    assert _ov("clickable", True).bool_value is True


def test_override_int_maps_int():
    assert _ov("value", 42).int_value == 42


def test_override_str_maps_string():
    assert _ov("text", "New message").string_value == "New message"


def test_override_color_maps_color():
    assert _ov("bg_color", Color(0xFF8800)).color_value == 0xFF8800


def test_override_point_maps_point():
    sp = _ov("pos", Point(3, 7))
    assert (sp.point_value.x, sp.point_value.y) == (3, 7)


def test_override_none_removes():
    sp = _ov("text", None)
    assert sp.WhichOneof("value") is None
    assert sp.WhichOneof("property") == "property_name"


def test_override_rejects_bad_prop_type():
    with pytest.raises(TypeError):
        _ov(1.5, "x")


def test_override_rejects_bad_value_type():
    with pytest.raises(TypeError):
        _ov("text", 1.5)


# ---- client batching --------------------------------------------------------


def test_client_sends_whole_batch_in_one_rpc():
    entries = [
        _ov("x", 1),
        _ov("y", 2),
        build_property_override("box", "bg_color", Color(0x123456)),
    ]
    sp = _sent(entries)
    assert len(sp.props) == 3
    assert sp.props[0].int_value == 1
    assert sp.props[1].int_value == 2
    assert sp.props[2].widget_id == "box"
    assert sp.props[2].color_value == 0x123456


def test_client_empty_batch_round_trips():
    sp = _sent([])
    assert len(sp.props) == 0


# ---- simulator override engine (googly-vr plan E1) -------------------------


def _screen_bytes() -> bytes:
    s = _proto.Screen()
    root = s.active
    root.id = "root"
    root.layout_absolute.SetInParent()
    b = root.layout_absolute.layout.children.add()
    b.id = "welcome"
    b.rect.SetInParent()
    b.rect.x, b.rect.y, b.rect.w, b.rect.h = 1, 2, 30, 10
    b.button.text = "hi"
    b.styles.add(bg_color=0x0000FF)
    return s.SerializeToString()


def _sim(tmp_path):
    from touchy_pad.sim.device import SimDevice
    from touchy_pad.sim.fs import SimFs

    fs = SimFs(tmp_path, "sim")
    fs.save("F:host/s/default.pb", _screen_bytes())
    return SimDevice(fs)


def _apply(dev, entries) -> None:
    cmd = _proto.Command(set_properties=_proto.SetPropertiesCmd(props=list(entries)))
    reply = _proto.Response.FromString(dev.handle_command(cmd.SerializeToString()))
    assert reply.code == _proto.RESULT_OK


def _widget(dev, widget_id):
    from touchy_pad.sim.device import _find_widget

    assert dev.active_screen is not None
    return _find_widget(dev.active_screen, widget_id)


def test_sim_applies_geometry_batch(tmp_path):
    dev = _sim(tmp_path)
    _apply(
        dev,
        [
            build_property_override("welcome", "x", 9),
            build_property_override("welcome", "y", 8),
            build_property_override("welcome", "w", 44),
            build_property_override("welcome", "h", 22),
        ],
    )
    r = _widget(dev, "welcome").rect
    assert (r.x, r.y, r.w, r.h) == (9, 8, 44, 22)


def test_sim_applies_color_and_text(tmp_path):
    dev = _sim(tmp_path)
    _apply(
        dev,
        [
            build_property_override("welcome", "bg_color", Color(0xFF8800)),
            build_property_override("welcome", "text", "new text"),
        ],
    )
    w = _widget(dev, "welcome")
    assert w.styles[0].bg_color == 0xFF8800  # replaced, not appended
    assert len(w.styles) == 1
    assert w.button.text == "new text"


def test_sim_removal_restores_pristine(tmp_path):
    dev = _sim(tmp_path)
    _apply(dev, [build_property_override("welcome", "x", 99)])
    assert _widget(dev, "welcome").rect.x == 99
    _apply(dev, [build_property_override("welcome", "x", None)])
    assert _widget(dev, "welcome").rect.x == 1  # original value


def test_sim_overrides_sticky_across_reload(tmp_path):
    dev = _sim(tmp_path)
    _apply(dev, [build_property_override("welcome", "text", "kept")])
    # Reload the same screen — the override must re-apply (firmware's
    # sticky semantics).
    dev._do_screen_load("F:host/s/default.pb")
    assert _widget(dev, "welcome").button.text == "kept"


def test_sim_unsupported_property_warns_but_oks(caplog, tmp_path):
    dev = _sim(tmp_path)
    with caplog.at_level(logging.WARNING):
        _apply(dev, [build_property_override("welcome", "opa", 128)])
    # _apply asserted RESULT_OK; the unsupported entry only WARNs.
    assert any("unsupported property" in r.message for r in caplog.records)
    assert _widget(dev, "welcome") is not None


def test_sim_absent_widget_is_not_an_error(tmp_path):
    dev = _sim(tmp_path)
    _apply(dev, [build_property_override("ghost", "x", 5)])
    # Still OK (checked in _apply) and the widget never materialises.
    assert _widget(dev, "ghost") is None
