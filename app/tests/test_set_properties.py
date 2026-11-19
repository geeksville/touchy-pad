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
from touchy_pad.api import Color, FontSize, Point, TouchyClient, build_property_override
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


# ---- stage lb26: font sizes travel as their own arm --------------------------


def test_override_text_font_maps_to_the_font_size_arm():
    sp = _ov("text_font", 30)
    assert sp.WhichOneof("property") == "property_name"
    assert sp.WhichOneof("value") == "font_size_value"
    assert sp.font_size_value == 30


def test_override_text_font_accepts_the_enum_member():
    """`FontSize` members ARE the pixel sizes (and ints), so they just work."""
    sp = _ov("text_font", FontSize.FONT_SIZE_8)
    assert sp.WhichOneof("value") == "font_size_value"
    assert sp.font_size_value == FontSize.FONT_SIZE_8 == 8


def test_override_text_font_zero_is_the_font_arm_not_a_removal():
    """0 == FONT_SIZE_DEFAULT (theme default); removal is `None`, not 0."""
    sp = _ov("text_font", FontSize.FONT_SIZE_DEFAULT)
    assert sp.WhichOneof("value") == "font_size_value"
    assert sp.font_size_value == 0
    assert _ov("text_font", None).WhichOneof("value") is None


def test_override_text_font_rejects_uncompiled_size():
    """An uncompiled size would silently theme the text on the device."""
    with pytest.raises(ValueError, match="no font of that size"):
        _ov("text_font", 20)


def test_override_text_font_rejects_non_int_values():
    """`text_font` is a font pointer on the device — only an int size is valid."""
    for bad in ("big", Color(0xFF0000), Point(1, 2), True, 1.5):
        with pytest.raises(TypeError, match="int pixel size"):
            _ov("text_font", bad)


def test_override_other_int_properties_keep_the_int_arm():
    """`text_font` is the only property whose int means a font size."""
    assert _ov("x", 30).WhichOneof("value") == "int_value"
    assert _ov("anim_duration", 4000).int_value == 4000


def test_font_size_value_arm_round_trips():
    cmd = _proto.SetPropertyCmd(
        widget_id="w",
        property_name="text_font",
        font_size_value=_proto.FontSize.FONT_SIZE_30,
    )
    again = _proto.SetPropertyCmd()
    again.ParseFromString(cmd.SerializeToString())
    assert again.WhichOneof("value") == "font_size_value"
    assert again.font_size_value == _proto.FontSize.FONT_SIZE_30


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
    # Stage lb26 — a label, so the font-size override has a realistic target
    # (the sim renders `font_size` on labels and span groups).
    t = root.layout_absolute.layout.children.add()
    t.id = "title"
    t.rect.SetInParent()
    t.rect.x, t.rect.y, t.rect.w, t.rect.h = 0, 0, 40, 12
    t.label.text = "Title"
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


# ---- stage lb26: the sim mirrors the text_font override ----------------------


def _font_styles(dev, widget_id="title"):
    """The `font_size` values carried by the (re-materialised) widget's styles."""
    return [s.font_size for s in _widget(dev, widget_id).styles if s.HasField("font_size")]


def test_sim_applies_text_font_override(tmp_path):
    dev = _sim(tmp_path)
    _apply(dev, [build_property_override("title", "text_font", FontSize.FONT_SIZE_30)])
    assert _font_styles(dev) == [30]


def test_sim_text_font_override_is_sticky_across_reload(tmp_path):
    dev = _sim(tmp_path)
    _apply(dev, [build_property_override("title", "text_font", 8)])
    dev._do_screen_load("F:host/s/default.pb")
    assert _font_styles(dev) == [8]


def test_sim_text_font_default_drops_the_local_font(tmp_path):
    dev = _sim(tmp_path)
    _apply(dev, [build_property_override("title", "text_font", 16)])
    assert _font_styles(dev) == [16]
    _apply(dev, [build_property_override("title", "text_font", FontSize.FONT_SIZE_DEFAULT)])
    assert _font_styles(dev) == []


def test_sim_text_font_removal_restores_pristine(tmp_path):
    dev = _sim(tmp_path)
    _apply(dev, [build_property_override("title", "text_font", 14)])
    _apply(dev, [build_property_override("title", "text_font", None)])
    assert _font_styles(dev) == []


def test_sim_text_font_uncompiled_size_warns_and_is_ignored(caplog, tmp_path):
    """Bypass the host-side validation to prove the sim mirrors the device."""
    dev = _sim(tmp_path)
    entry = _proto.SetPropertyCmd(widget_id="title", property_name="text_font", font_size_value=20)
    with caplog.at_level(logging.WARNING):
        _apply(dev, [entry])
    assert any("no compiled font" in r.message for r in caplog.records)
    assert _font_styles(dev) == []


# ---- stage lb24: a `text` override on a span group is markup -----------------


def _span_group_screen_bytes() -> bytes:
    s = _proto.Screen()
    root = s.active
    root.id = "root"
    root.layout_absolute.SetInParent()
    sg = root.layout_absolute.layout.children.add()
    sg.id = "quip"
    sg.rect.SetInParent()
    sg.rect.x, sg.rect.y, sg.rect.w, sg.rect.h = 0, 0, 300, 120
    sg.span_group.spans.add(text="original", text_color=0x00FF00)
    return s.SerializeToString()


def _sim_with_span_group(tmp_path):
    from touchy_pad.sim.device import SimDevice
    from touchy_pad.sim.fs import SimFs

    fs = SimFs(tmp_path, "sim")
    fs.save("F:host/s/default.pb", _span_group_screen_bytes())
    return SimDevice(fs)


def test_sim_text_override_on_span_group_reparses_markup(tmp_path):
    dev = _sim_with_span_group(tmp_path)
    _apply(dev, [build_property_override("quip", "text", "#A020F0 cocks head#I bet")])
    spans = _widget(dev, "quip").span_group.spans
    assert [(s.text, s.text_color if s.HasField("text_color") else None) for s in spans] == [
        ("cocks head", 0xA020F0),
        ("I bet", None),
    ]


def test_sim_span_group_text_override_removal_restores_pristine(tmp_path):
    dev = _sim_with_span_group(tmp_path)
    _apply(dev, [build_property_override("quip", "text", "replaced")])
    assert [s.text for s in _widget(dev, "quip").span_group.spans] == ["replaced"]
    _apply(dev, [build_property_override("quip", "text", None)])
    spans = _widget(dev, "quip").span_group.spans
    assert [s.text for s in spans] == ["original"]
    assert spans[0].text_color == 0x00FF00


def test_sim_text_override_still_rejects_other_kinds(caplog, tmp_path):
    """A slider has no text; the unsupported-path warning must survive."""
    from touchy_pad.sim.device import SimDevice
    from touchy_pad.sim.fs import SimFs

    s = _proto.Screen()
    s.active.id = "root"
    s.active.layout_absolute.SetInParent()
    sl = s.active.layout_absolute.layout.children.add()
    sl.id = "level"
    sl.rect.SetInParent()
    sl.slider.min, sl.slider.max, sl.slider.value = 0, 10, 5
    fs = SimFs(tmp_path, "sim2")
    fs.save("F:host/s/default.pb", s.SerializeToString())
    dev = SimDevice(fs)
    with caplog.at_level(logging.WARNING):
        _apply(dev, [build_property_override("level", "text", "nope")])
    assert any("only on button/label/span_group" in r.message for r in caplog.records)
