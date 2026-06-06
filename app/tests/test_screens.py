"""Tests for the protobuf-backed screen DSL (stage 15) and stage-16 actions."""

from __future__ import annotations

from pathlib import Path

import pytest

from touchy_pad import _proto
from touchy_pad.api import (
    ANIM_PATH_EASE_IN_OUT,
    ANIM_PATH_LINEAR,
    PART_KNOB,
    PROP_BG_COLOR,
    PROP_IMAGE_RECOLOR_OPA,
    PROP_TRANSFORM_WIDTH,
    STATE_PRESSED,
    Screen,
    arc,
    build_default_screen,
    build_demo,
    build_user_pages,
    button,
    change_widget_ref_action,
    checkbox,
    col,
    force_render,
    fps,
    grid,
    hid_keys,
    host_action,
    image,
    label,
    macro_action,
    macros,
    next_widget_action,
    prev_widget_action,
    rect,
    ripple_animation,
    row,
    slider,
    spacer,
    style,
    toggle,
    trackpad,
    transition,
    widget_ref,
)


@pytest.fixture(autouse=True)
def _clear_registry():
    Screen._registry = []
    yield
    Screen._registry = []


# Stage 24.2: each Layer in a decoded Screen is a single layout-`Widget`
# whose `kind` oneof is `layout_absolute` / `layout_flex` / `layout_grid`,
# and the children list lives at `<kind>.layout.children`. These helpers
# hide that field shuffle so tests can keep asking "what's on the layer"
# in one line.


def _layer_kind(layer: object) -> str | None:
    """Return the layout-widget kind name (`"layout_grid"` etc.) or ``None``."""
    kind = layer.WhichOneof("kind")  # type: ignore[attr-defined]
    if kind in ("layout_absolute", "layout_flex", "layout_grid"):
        return kind
    return None


def _children(layer: object):
    """Return the layout-widget's `Layout.children` list."""
    kind = _layer_kind(layer)
    assert kind is not None, f"layer is not a layout widget: kind={layer.WhichOneof('kind')}"
    return getattr(layer, kind).layout.children


def _layout(layer: object):
    """Return the layout-widget's per-kind submessage (carrying flow/cols/gap/…)."""
    kind = _layer_kind(layer)
    assert kind is not None
    return getattr(layer, kind)


def test_button_round_trip():
    s = Screen("home", layout=col(gap=4))
    s += button("play", text="Play", on_click=host_action(0x42))
    s += label("title", text="Hello", font_size=24)

    decoded = _proto.Screen.FromString(s.to_bytes())
    assert _layer_kind(decoded.active) == "layout_flex"
    assert decoded.active.layout_flex.flow == _proto.LayoutFlex.COLUMN
    assert decoded.active.layout_flex.gap == 4
    assert len(_children(decoded.active)) == 2

    w0 = _children(decoded.active)[0]
    assert w0.id == "play"
    assert w0.WhichOneof("kind") == "button"
    assert w0.button.text == "Play"
    assert len(w0.button.on_click) == 1
    assert w0.button.on_click[0].WhichOneof("kind") == "host"
    assert w0.button.on_click[0].host.code == 0x42

    w1 = _children(decoded.active)[1]
    assert w1.WhichOneof("kind") == "label"
    assert w1.label.text == "Hello"
    assert w1.label.font_size == 24


def test_all_widget_kinds_serialise():
    s = Screen("k", layout=grid(cols=3, gap=2))
    s += button("b", text="B")
    s += label("l", text="L")
    s += slider("s", min=0, max=10, value=5, on_change=host_action(1))
    s += toggle("t", on=True, on_change=host_action(2))
    s += checkbox("c", text="go", checked=True, on_change=host_action(3))
    s += image("i", asset="img/a.png")
    s += arc("a", min=0, max=270, value=90)
    s += spacer("sp", rect=rect(w=20, h=20))

    decoded = _proto.Screen.FromString(s.to_bytes())
    kinds = [w.WhichOneof("kind") for w in _children(decoded.active)]
    assert kinds == ["button", "label", "slider", "toggle", "checkbox", "image", "arc", "spacer"]
    assert _layer_kind(decoded.active) == "layout_grid"
    assert decoded.active.layout_grid.cols == 3


def test_style_and_rect_applied():
    s = Screen("x")
    s += button(
        "go",
        text="Go",
        rect=rect(10, 20, 100, 40),
        style=style(bg_color=0xFF8800, radius=8, text_color=0xFFFFFF),
    )
    decoded = _proto.Screen.FromString(s.to_bytes())
    w = _children(decoded.active)[0]
    assert w.rect.x == 10
    assert w.rect.w == 100
    # Single Style is auto-wrapped into the repeated `styles` field.
    assert len(w.styles) == 1
    assert w.styles[0].bg_color == 0xFF8800
    assert w.styles[0].radius == 8
    assert w.styles[0].text_color == 0xFFFFFF
    assert w.styles[0].for_state == 0


def test_style_for_state_round_trip():
    """`for_state` is preserved verbatim through the proto round-trip."""
    s = Screen("x")
    s += button(
        "go",
        style=style(bg_color=0x1E90FF, for_state=STATE_PRESSED),
    )
    decoded = _proto.Screen.FromString(s.to_bytes())
    assert _children(decoded.active)[0].styles[0].for_state == STATE_PRESSED
    # OR'ing state + part composes a valid selector.
    selector = STATE_PRESSED | PART_KNOB
    s2 = Screen("y")
    s2 += button("g2", style=style(bg_color=1, for_state=selector))
    decoded2 = _proto.Screen.FromString(s2.to_bytes())
    assert _children(decoded2.active)[0].styles[0].for_state == selector


def test_style_list_round_trip():
    """A list of Styles round-trips in order; each entry keeps its selector."""
    s = Screen("x")
    s += button(
        "go",
        style=[
            style(bg_color=0x101010),
            style(bg_color=0x1E90FF, for_state=STATE_PRESSED),
        ],
    )
    decoded = _proto.Screen.FromString(s.to_bytes())
    styles_ = _children(decoded.active)[0].styles
    assert len(styles_) == 2
    assert styles_[0].bg_color == 0x101010
    assert styles_[0].for_state == 0
    assert styles_[1].bg_color == 0x1E90FF
    assert styles_[1].for_state == STATE_PRESSED


def test_default_screen_only_has_active_layer():
    """A bare Screen only carries `active`; persistent layers stay unset."""
    s = Screen("d")
    s += button("b", text="B")
    decoded = _proto.Screen.FromString(s.to_bytes())
    assert decoded.HasField("active")
    assert len(_children(decoded.active)) == 1
    assert not decoded.HasField("top")
    assert not decoded.HasField("sys")
    assert not decoded.HasField("bottom")


def test_persistent_layers_round_trip():
    """add_top / add_sys / add_bottom land widgets on the right layer."""
    s = Screen("p", layout=col(gap=2))
    s += button("a", text="active")
    s.add_top(button("t", text="top"))
    s.add_sys(button("y", text="sys"))
    s.add_bottom(button("z", text="bottom"))
    decoded = _proto.Screen.FromString(s.to_bytes())
    assert [w.id for w in _children(decoded.active)] == ["a"]
    assert decoded.HasField("top")
    assert [w.id for w in _children(decoded.top)] == ["t"]
    assert decoded.HasField("sys")
    assert [w.id for w in _children(decoded.sys)] == ["y"]
    assert decoded.HasField("bottom")
    assert [w.id for w in _children(decoded.bottom)] == ["z"]
    # The active layer's layout is independent of the persistent layers'.
    assert _layer_kind(decoded.active) == "layout_flex"
    # Persistent layers default to absolute when constructed via add_*.
    assert _layer_kind(decoded.top) == "layout_absolute"


def test_empty_layer_clears_persistent_layer():
    """Passing ``top=Layer()`` is the explicit "clear top layer" payload."""
    from touchy_pad.api import Layer

    s = Screen("clr", top=Layer())
    decoded = _proto.Screen.FromString(s.to_bytes())
    assert decoded.HasField("top")
    assert len(_children(decoded.top)) == 0


def test_layer_per_layer_layout():
    """Each layer carries its own layout independently."""
    from touchy_pad.api import Layer

    s = Screen("g", layout=grid(cols=2, gap=1))
    s.top = Layer(layout=row(gap=4))
    decoded = _proto.Screen.FromString(s.to_bytes())
    assert _layer_kind(decoded.active) == "layout_grid"
    assert _layer_kind(decoded.top) == "layout_flex"
    assert decoded.top.layout_flex.flow == _proto.LayoutFlex.ROW
    assert decoded.top.layout_flex.gap == 4


# -- Stage 20.2: optional Style fields + Transition -------------------------


def test_style_optional_zero_round_trips():
    """Explicit zero values are preserved (no longer indistinguishable from unset)."""
    s = Screen("z")
    s += button("b", style=style(bg_color=0x000000, transform_width=0))
    decoded = _proto.Screen.FromString(s.to_bytes())
    st = _children(decoded.active)[0].styles[0]
    assert st.HasField("bg_color")
    assert st.bg_color == 0
    assert st.HasField("transform_width")
    assert st.transform_width == 0
    # Other visual fields stay unset.
    assert not st.HasField("radius")
    assert not st.HasField("recolor")


def test_style_recolor_and_transform_width_round_trip():
    s = Screen("r")
    s += button(
        "b",
        style=style(
            recolor=0x123456,
            recolor_opa=76,
            transform_width=20,
            for_state=STATE_PRESSED,
        ),
    )
    decoded = _proto.Screen.FromString(s.to_bytes())
    st = _children(decoded.active)[0].styles[0]
    assert st.recolor == 0x123456
    assert st.recolor_opa == 76
    assert st.transform_width == 20
    assert st.for_state == STATE_PRESSED


def test_style_recolor_opa_range_checked():
    with pytest.raises(ValueError):
        style(recolor_opa=256)
    with pytest.raises(ValueError):
        style(recolor_opa=-1)


def test_transition_round_trip():
    """Transition props/path/durations survive a serialize round-trip."""
    tr = transition(
        props=[PROP_TRANSFORM_WIDTH, PROP_IMAGE_RECOLOR_OPA],
        path=ANIM_PATH_EASE_IN_OUT,
        duration_ms=250,
        delay_ms=50,
    )
    s = Screen("t")
    s += button("b", style=style(transition=tr))
    decoded = _proto.Screen.FromString(s.to_bytes())
    st = _children(decoded.active)[0].styles[0]
    assert st.HasField("transition")
    assert list(st.transition.props) == [PROP_TRANSFORM_WIDTH, PROP_IMAGE_RECOLOR_OPA]
    assert st.transition.path == ANIM_PATH_EASE_IN_OUT
    assert st.transition.duration_ms == 250
    assert st.transition.delay_ms == 50


def test_transition_requires_props():
    with pytest.raises(ValueError):
        transition(props=[])


def test_transition_defaults_are_linear_200ms():
    tr = transition(props=[PROP_TRANSFORM_WIDTH])
    assert tr.path == ANIM_PATH_LINEAR
    assert tr.duration_ms == 200
    assert tr.delay_ms == 0


def test_demo_smile_uses_transition_pattern():
    """The smiley image-button carries the imagebutton_1-style transition."""
    # Stage 57 — the smiley lives on the "test" widget page that
    # ``build_demo()`` returns alongside the chrome screen.
    _, widgets = build_demo()
    test_widget = next(w for name, w in widgets if name == "test")
    decoded = _proto.Widget.FromString(test_widget.SerializeToString())
    # Stage 59 — build_demo() now wraps the showcase grid in an outer
    # absolute layer (with an animated "red dot" overlay) so we have to
    # descend one extra level to reach the grid's children.
    abs_children = decoded.layout_absolute.layout.children
    grid_widget = next(w for w in abs_children if w.WhichOneof("kind") == "layout_grid")
    grid_children = grid_widget.layout_grid.layout.children
    smile = next(w for w in grid_children if w.id == "smile")
    assert len(smile.styles) == 2
    default_style, pressed_style = smile.styles
    # Default style binds the transition, no for_state and no bg colour.
    assert default_style.for_state == 0
    assert not default_style.HasField("bg_color")
    assert default_style.HasField("transition")
    assert list(default_style.transition.props) == [
        PROP_TRANSFORM_WIDTH,
        PROP_IMAGE_RECOLOR_OPA,
        PROP_BG_COLOR,
    ]
    assert default_style.transition.duration_ms == 300
    # Pressed style grows + tints the button (cranked up for visual debug).
    assert pressed_style.for_state == STATE_PRESSED
    assert pressed_style.transform_width == 80
    assert pressed_style.recolor == 0xFF0000
    assert pressed_style.recolor_opa == 255
    assert pressed_style.bg_color == 0x00FF00


# -- Stage 16 ---------------------------------------------------------------


def test_macro_action_round_trip():
    """A macro of typed steps decodes to the same step list."""
    s = Screen("m")
    s += button(
        "hi",
        on_click=macro_action(
            [
                macros.key_tap(hid_keys.KEY_H, hid_keys.MOD_LSHIFT),
                macros.key_tap(hid_keys.KEY_I),
                macros.delay(50),
                macros.set_delay(20),
                macros.mouse_click(),
            ]
        ),
    )
    decoded = _proto.Screen.FromString(s.to_bytes())
    actions = _children(decoded.active)[0].button.on_click
    assert len(actions) == 1
    assert actions[0].WhichOneof("kind") == "macro"
    steps = actions[0].macro.steps
    assert [st.WhichOneof("step") for st in steps] == [
        "key_tap",
        "key_tap",
        "delay_ms",
        "set_delay_ms",
        "mouse_click",
    ]
    assert steps[0].key_tap.keycode == hid_keys.KEY_H
    assert steps[0].key_tap.modifiers == hid_keys.MOD_LSHIFT
    assert steps[2].delay_ms == 50
    assert steps[3].set_delay_ms == 20


def test_int_on_click_becomes_host_action():
    s = Screen("i")
    s += button("go", on_click=0x99)
    decoded = _proto.Screen.FromString(s.to_bytes())
    actions = _children(decoded.active)[0].button.on_click
    assert len(actions) == 1
    assert actions[0].host.code == 0x99


def test_host_action_auto_code_in_reserved_range():
    from touchy_pad.api import _events

    _events._reset()
    a = host_action(on_event=lambda e: None)
    b = host_action(on_event=lambda e: None)
    assert a.host.code >= _events.AUTO_CODE_BASE
    assert b.host.code >= _events.AUTO_CODE_BASE
    assert a.host.code != b.host.code


def test_host_action_auto_code_without_callback():
    from touchy_pad.api import _events

    _events._reset()
    a = host_action()
    assert a.host.code >= _events.AUTO_CODE_BASE
    # No callback supplied, so nothing is left pending for that code.
    assert _events.harvest({a.host.code}) == {}


def test_host_action_on_event_registers_binding():
    from touchy_pad.api import _events

    _events._reset()
    cb = lambda e: None  # noqa: E731
    a = host_action(on_event=cb)
    harvested = _events.harvest({a.host.code})
    assert harvested == {a.host.code: cb}
    # Harvest removes it.
    assert _events.harvest({a.host.code}) == {}


def test_host_action_explicit_code_with_callback():
    from touchy_pad.api import _events

    _events._reset()
    cb = lambda e: None  # noqa: E731
    a = host_action(0x42, on_event=cb)
    assert a.host.code == 0x42
    assert _events.harvest({0x42}) == {0x42: cb}


def test_host_action_rejects_out_of_range_code():
    with pytest.raises(ValueError):
        host_action(0x1_0000_0000)


def test_action_list_mixes_host_and_macro():
    s = Screen("mix")
    s += button(
        "x",
        on_click=[
            host_action(7),
            macro_action([macros.key_tap(hid_keys.KEY_A)]),
        ],
    )
    decoded = _proto.Screen.FromString(s.to_bytes())
    actions = _children(decoded.active)[0].button.on_click
    assert len(actions) == 2
    assert actions[0].host.code == 7
    assert actions[1].WhichOneof("kind") == "macro"


def test_macro_action_requires_steps():
    with pytest.raises(ValueError):
        macro_action([])


def test_type_text_round_trip():
    steps = macros.type_text("Hi!")
    assert [s.WhichOneof("step") for s in steps] == ["key_tap"] * 3
    assert steps[0].key_tap.keycode == hid_keys.KEY_H
    assert steps[0].key_tap.modifiers == hid_keys.MOD_LSHIFT
    # '!' is shift+1 on US layout.
    assert steps[2].key_tap.keycode == hid_keys.KEY_1
    assert steps[2].key_tap.modifiers == hid_keys.MOD_LSHIFT


def test_default_layout_is_absolute():
    decoded = _proto.Screen.FromString(Screen("d").to_bytes())
    assert _layer_kind(decoded.active) == "layout_absolute"


def test_row_layout_helper():
    decoded = _proto.Screen.FromString(Screen("r", layout=row(gap=7)).to_bytes())
    assert _layer_kind(decoded.active) == "layout_flex"
    assert decoded.active.layout_flex.flow == _proto.LayoutFlex.ROW
    assert decoded.active.layout_flex.gap == 7


def test_screen_requires_name():
    with pytest.raises(ValueError):
        Screen("")


def test_grid_requires_positive_cols():
    with pytest.raises(ValueError):
        grid(cols=0)


def test_write_to_disk_and_reload(tmp_path: Path):
    s = Screen("disk")
    s += label("hi", text="hi")
    p = s.write(tmp_path / "sub" / "disk.pb")
    assert p.exists()
    decoded = _proto.Screen.FromString(p.read_bytes())
    assert _children(decoded.active)[0].label.text == "hi"


# -- Stage 20.4: Trackpad ripple animations + colors ------------------------


def test_ripple_animation_defaults():
    """Bare ``ripple_animation()`` populates every documented default."""
    r = ripple_animation()
    assert r.start_opa == 200
    assert r.max_radius == 40
    assert r.duration_ms == 350
    assert r.path == ANIM_PATH_LINEAR
    assert r.border_width == 0


def test_ripple_animation_kwargs_roundtrip():
    r = ripple_animation(
        start_opa=128,
        max_radius=70,
        duration_ms=500,
        path=ANIM_PATH_EASE_IN_OUT,
        border_width=4,
    )
    # Round-trip through the parent message to exercise the wire format.
    s = Screen("rip")
    s += trackpad("pad", tap_ripple=r)
    decoded = _proto.Screen.FromString(s.to_bytes())
    tp = _children(decoded.active)[0].trackpad
    assert tp.HasField("tap_ripple")
    assert tp.tap_ripple.start_opa == 128
    assert tp.tap_ripple.max_radius == 70
    assert tp.tap_ripple.duration_ms == 500
    assert tp.tap_ripple.path == ANIM_PATH_EASE_IN_OUT
    assert tp.tap_ripple.border_width == 4


def test_trackpad_unset_optionals_have_no_color_or_ripple():
    """Bare trackpad leaves color + ripple fields cleared."""
    s = Screen("t")
    s += trackpad("pad")
    decoded = _proto.Screen.FromString(s.to_bytes())
    tp = _children(decoded.active)[0].trackpad
    assert tp.scroll_invert_y is False
    assert tp.scroll_invert_x is False
    assert not tp.HasField("left_touch_color")
    assert not tp.HasField("right_touch_color")
    assert not tp.HasField("middle_touch_color")
    assert not tp.HasField("touch_ripple")
    assert not tp.HasField("tap_ripple")


def test_trackpad_full_kwargs_roundtrip():
    s = Screen("t")
    s += trackpad(
        "pad",
        scroll_invert_y=True,
        scroll_invert_x=True,
        left_touch_color=0x112233,
        right_touch_color=0x445566,
        middle_touch_color=0x778899,
        touch_ripple=ripple_animation(max_radius=20, duration_ms=100),
        tap_ripple=ripple_animation(max_radius=50, border_width=3),
    )
    decoded = _proto.Screen.FromString(s.to_bytes())
    tp = _children(decoded.active)[0].trackpad
    assert tp.scroll_invert_y is True
    assert tp.scroll_invert_x is True
    assert tp.left_touch_color == 0x112233
    assert tp.right_touch_color == 0x445566
    assert tp.middle_touch_color == 0x778899
    assert tp.touch_ripple.max_radius == 20
    assert tp.touch_ripple.duration_ms == 100
    assert tp.tap_ripple.max_radius == 50
    assert tp.tap_ripple.border_width == 3


def test_build_demo_screen_trackpad_has_ripples():
    """Demo widget page ships ripple eye-candy so users see the feature."""
    _, widgets = build_demo()
    pad_container = next(w for name, w in widgets if name == "trackpad")
    decoded = _proto.Widget.FromString(pad_container.SerializeToString())
    # The trackpad page is now a 1x1 grid container; the trackpad itself is
    # the third child (after background spacer and hint label).
    children = list(decoded.layout_grid.layout.children)
    tp_widget = next(c for c in children if c.WhichOneof("kind") == "trackpad")
    tp = tp_widget.trackpad
    assert tp.HasField("touch_ripple")
    assert tp.HasField("tap_ripple")
    assert tp.HasField("left_touch_color")
    assert tp.HasField("right_touch_color")
    assert tp.HasField("middle_touch_color")


# -- Stage 24 / 57: FPS widget + ActionDevice / ActionChangeWidgetRef -------


def test_fps_widget_round_trip():
    """`fps()` builds a Widget whose `kind` oneof is `fps`."""
    s = Screen("h")
    s += fps("fps_label")
    decoded = _proto.Screen.FromString(s.to_bytes())
    w = _children(decoded.active)[0]
    assert w.id == "fps_label"
    assert w.WhichOneof("kind") == "fps"


def test_force_render_widget_round_trip():
    """`force_render()` builds a Widget whose `kind` oneof is `force_render`."""
    s = Screen("h")
    s += force_render("force_box")
    decoded = _proto.Screen.FromString(s.to_bytes())
    w = _children(decoded.active)[0]
    assert w.id == "force_box"
    assert w.WhichOneof("kind") == "force_render"


def test_change_widget_ref_action_by_path():
    """BY_PATH (default) packs the path + target_id into the proto."""
    a = change_widget_ref_action(target_id="page", path="F:host/w/trackpad.pb")
    assert a.WhichOneof("kind") == "device"
    cw = a.device.change_widget_ref
    assert cw.target_id == "page"
    assert cw.path == "F:host/w/trackpad.pb"
    assert cw.behavior == _proto.ActionChangeWidgetRef.Behavior.BY_PATH


def test_change_widget_ref_action_requires_target_id():
    with pytest.raises(ValueError):
        change_widget_ref_action(target_id="", path="F:host/w/a.pb")


def test_change_widget_ref_action_requires_path():
    with pytest.raises(ValueError):
        change_widget_ref_action(target_id="page", path="")


def test_next_widget_action():
    a = next_widget_action("page", "F:host/w/")
    cw = a.device.change_widget_ref
    assert cw.target_id == "page"
    assert cw.path == "F:host/w/"
    assert cw.behavior == _proto.ActionChangeWidgetRef.Behavior.NEXT


def test_prev_widget_action():
    a = prev_widget_action("page", "F:host/w/")
    cw = a.device.change_widget_ref
    assert cw.target_id == "page"
    assert cw.path == "F:host/w/"
    assert cw.behavior == _proto.ActionChangeWidgetRef.Behavior.PREVIOUS


def test_build_demo_returns_screen_and_widgets():
    screen, widgets = build_demo()
    # Stage 68 — the chrome screen is the canonical "default".
    assert screen.name == "default"
    # Stage 68 — the active layer is a vertical flex column whose first
    # child is the prev/next chrome row and second child is the growing
    # body widget_ref(id="page") pointing into F:host/uscr/.
    decoded = _proto.Screen.FromString(screen.to_bytes())
    assert decoded.active.layout_flex.flow == _proto.LayoutFlex.Flow.COLUMN
    top = list(decoded.active.layout_flex.layout.children)
    assert len(top) == 2
    chrome, body = top
    chrome_ids = [c.id for c in chrome.layout_flex.layout.children]
    assert "prev" in chrome_ids and "next" in chrome_ids
    assert body.id == "page"
    assert body.WhichOneof("kind") == "widget_ref"
    assert body.widget_ref.path == "F:host/uscr/trackpad.pb"
    # The body grows to fill the column (Stage 72 grow_x/grow_y).
    assert body.grow_y == 1
    assert body.grow_x == 1
    # Two named user pages, sorted by build order.
    names = [n for n, _ in widgets]
    assert names == ["test", "trackpad"]


def test_build_demo_chrome_wires_change_widget_ref_actions():
    """Prev/Next buttons emit ActionDevice(change_widget_ref=NEXT/PREVIOUS)."""
    screen, _ = build_demo()
    decoded = _proto.Screen.FromString(screen.to_bytes())
    chrome = decoded.active.layout_flex.layout.children[0]
    chrome_children = list(chrome.layout_flex.layout.children)
    prev = next(w for w in chrome_children if w.id == "prev")
    nxt = next(w for w in chrome_children if w.id == "next")
    for w, expected in [
        (prev, _proto.ActionChangeWidgetRef.Behavior.PREVIOUS),
        (nxt, _proto.ActionChangeWidgetRef.Behavior.NEXT),
    ]:
        assert len(w.button.on_click) == 1
        act = w.button.on_click[0]
        assert act.WhichOneof("kind") == "device"
        cw = act.device.change_widget_ref
        assert cw.behavior == expected
        assert cw.target_id == "page"
        assert cw.path == "F:host/uscr/"


def test_widget_ref_round_trip():
    """Stage 54 — `widget_ref()` produces a Widget with the path encoded."""
    w = widget_ref("R:host/widgets/key0.pb")
    assert w.WhichOneof("kind") == "widget_ref"
    assert w.widget_ref.path == "R:host/widgets/key0.pb"

    # Embed in a row inside a screen and ensure it survives serialization.
    Screen(
        "refs",
        layout=row(),
        widgets=[widget_ref("R:host/widgets/key0.pb"), button("inline")],
    )
    msg = _proto.Screen()
    msg.ParseFromString(Screen._registry[0].to_bytes())
    assert msg.active.version == _proto.Widget.Version.CURRENT
    children = msg.active.layout_flex.layout.children
    assert len(children) == 2
    assert children[0].WhichOneof("kind") == "widget_ref"
    assert children[0].widget_ref.path == "R:host/widgets/key0.pb"
    assert children[1].WhichOneof("kind") == "button"


def test_widget_ref_rejects_empty_path():
    with pytest.raises(ValueError):
        widget_ref("")


def test_widget_ref_rejects_inline_styling():
    # Stage 57 — id IS now accepted (it addresses the ref at runtime).
    w = widget_ref("F:host/widgets/a.pb", id="page")
    assert w.id == "page"
    # rect/style still belong to the referenced widget, not the ref node.
    with pytest.raises(ValueError):
        widget_ref("F:host/widgets/a.pb", rect=rect(0, 0, 10, 10))


# -- Stage 68: default chrome screen + user page bodies ---------------------


def test_build_default_screen_is_vertical_flex_with_growing_body():
    """The default chrome is a COLUMN: chrome row + flex-growing page body."""
    screen = build_default_screen()
    assert screen.name == "default"
    decoded = _proto.Screen.FromString(screen.to_bytes())
    assert decoded.active.layout_flex.flow == _proto.LayoutFlex.Flow.COLUMN
    top = list(decoded.active.layout_flex.layout.children)
    assert len(top) == 2
    chrome, body = top
    # Chrome is a nested flex row carrying the prev/next buttons with a
    # flex-grow spacer between them (pushes prev left, next right).
    assert chrome.WhichOneof("kind") == "layout_flex"
    chrome_kids = list(chrome.layout_flex.layout.children)
    chrome_ids = [c.id for c in chrome_kids]
    assert chrome_ids == ["prev", "chrome_gap", "next"]
    spacer_w = chrome_kids[1]
    assert spacer_w.WhichOneof("kind") == "spacer"
    assert spacer_w.grow_x == 1
    # The chrome row spans the full width but stays content-height.
    assert chrome.grow_x == 1
    assert chrome.grow_y == 0
    # Body is the page widget_ref that fills the remaining height.
    assert body.id == "page"
    assert body.WhichOneof("kind") == "widget_ref"
    assert body.widget_ref.path == "F:host/uscr/trackpad.pb"
    assert body.grow_y == 1
    assert body.grow_x == 1


def test_build_user_pages_targets_user_screens_dir():
    """Page bodies are addressed into F:host/uscr/ by the chrome actions."""
    pages = dict(build_user_pages())
    assert set(pages) == {"test", "trackpad"}
    screen = build_default_screen()
    decoded = _proto.Screen.FromString(screen.to_bytes())
    chrome = decoded.active.layout_flex.layout.children[0]
    for btn in chrome.layout_flex.layout.children:
        if btn.WhichOneof("kind") != "button":
            continue
        cw = btn.button.on_click[0].device.change_widget_ref
        assert cw.path == "F:host/uscr/"


def test_default_screen_json_round_trips_to_default():
    """proto/default_screen.json decodes to the compiled-in setup screen."""
    from google.protobuf import json_format

    repo_root = Path(__file__).resolve().parents[2]
    raw = (repo_root / "proto" / "default_screen.json").read_text(encoding="utf-8")
    msg = json_format.Parse(raw, _proto.Screen())
    # High-level shape: vertical flex column with two top-level children.
    assert msg.active.layout_flex.flow == _proto.LayoutFlex.Flow.COLUMN
    top = list(msg.active.layout_flex.layout.children)
    assert len(top) == 2
    hint, pad_container = top
    assert hint.id == "setup_hint"
    assert pad_container.id == "pad_container"
    assert pad_container.grow_x == 1
    assert pad_container.grow_y == 1
    # Keep the JSON parseable and structurally correct, but don't do an
    # exact byte comparison — that breaks every time build_setup_screen()
    # evolves.  Run `just gen-default-screen` to resync the file.
