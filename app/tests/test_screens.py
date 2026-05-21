"""Tests for the protobuf-backed screen DSL (stage 15) and stage-16 actions."""

from __future__ import annotations

from pathlib import Path

import pytest

from touchy_pad import _proto, hid_keys, macros
from touchy_pad.screens import (
    ANIM_PATH_EASE_IN_OUT,
    ANIM_PATH_LINEAR,
    PART_KNOB,
    PROP_BG_COLOR,
    PROP_IMAGE_RECOLOR_OPA,
    PROP_TRANSFORM_WIDTH,
    STATE_PRESSED,
    Screen,
    _collect_from_script,
    arc,
    build_demo_screen,
    button,
    checkbox,
    col,
    grid,
    host_action,
    image,
    label,
    macro_action,
    rect,
    row,
    slider,
    spacer,
    style,
    toggle,
    transition,
)


@pytest.fixture(autouse=True)
def _clear_registry():
    Screen._registry = []
    yield
    Screen._registry = []


def test_button_round_trip():
    s = Screen("home", layout=col(gap=4))
    s += button("play", text="Play", on_click=host_action(0x42))
    s += label("title", text="Hello", font_size=24)

    decoded = _proto.Screen.FromString(s.to_bytes())
    assert decoded.name == "home"
    assert decoded.WhichOneof("layout") == "flex"
    assert decoded.flex.flow == _proto.LayoutFlex.COLUMN
    assert decoded.flex.gap == 4
    assert len(decoded.widgets) == 2

    w0 = decoded.widgets[0]
    assert w0.id == "play"
    assert w0.WhichOneof("kind") == "button"
    assert w0.button.text == "Play"
    assert len(w0.button.on_click) == 1
    assert w0.button.on_click[0].WhichOneof("kind") == "host"
    assert w0.button.on_click[0].host.code == 0x42

    w1 = decoded.widgets[1]
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
    kinds = [w.WhichOneof("kind") for w in decoded.widgets]
    assert kinds == ["button", "label", "slider", "toggle", "checkbox", "image", "arc", "spacer"]
    assert decoded.WhichOneof("layout") == "grid"
    assert decoded.grid.cols == 3


def test_style_and_rect_applied():
    s = Screen("x")
    s += button(
        "go",
        text="Go",
        rect=rect(10, 20, 100, 40),
        style=style(bg_color=0xFF8800, radius=8, text_color=0xFFFFFF),
    )
    decoded = _proto.Screen.FromString(s.to_bytes())
    w = decoded.widgets[0]
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
    assert decoded.widgets[0].styles[0].for_state == STATE_PRESSED
    # OR'ing state + part composes a valid selector.
    selector = STATE_PRESSED | PART_KNOB
    s2 = Screen("y")
    s2 += button("g2", style=style(bg_color=1, for_state=selector))
    decoded2 = _proto.Screen.FromString(s2.to_bytes())
    assert decoded2.widgets[0].styles[0].for_state == selector


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
    styles_ = decoded.widgets[0].styles
    assert len(styles_) == 2
    assert styles_[0].bg_color == 0x101010
    assert styles_[0].for_state == 0
    assert styles_[1].bg_color == 0x1E90FF
    assert styles_[1].for_state == STATE_PRESSED


def test_screen_version_is_six():
    """Stage 20.3 wire-format bump: Screen.Version.CURRENT == 6.

    Bumped when ``ImageButton`` was refactored to embed an ``Image``
    submessage (so it could share ``scale`` / ``rotation`` with plain
    ``Image`` widgets) and ``pressed_asset`` was promoted to a full
    ``pressed`` ``Image``.
    """
    s = Screen("v")
    decoded = _proto.Screen.FromString(s.to_bytes())
    assert decoded.version == _proto.Screen.Version.CURRENT
    assert int(_proto.Screen.Version.CURRENT) == 6


# -- Stage 20.2: optional Style fields + Transition -------------------------


def test_style_optional_zero_round_trips():
    """Explicit zero values are preserved (no longer indistinguishable from unset)."""
    s = Screen("z")
    s += button("b", style=style(bg_color=0x000000, transform_width=0))
    decoded = _proto.Screen.FromString(s.to_bytes())
    st = decoded.widgets[0].styles[0]
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
    st = decoded.widgets[0].styles[0]
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
    st = decoded.widgets[0].styles[0]
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
    s = build_demo_screen("demo")
    decoded = _proto.Screen.FromString(s.to_bytes())
    smile = next(w for w in decoded.widgets if w.id == "smile")
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
    actions = decoded.widgets[0].button.on_click
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
    actions = decoded.widgets[0].button.on_click
    assert len(actions) == 1
    assert actions[0].host.code == 0x99


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
    actions = decoded.widgets[0].button.on_click
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
    assert decoded.WhichOneof("layout") == "absolute"


def test_row_layout_helper():
    decoded = _proto.Screen.FromString(Screen("r", layout=row(gap=7)).to_bytes())
    assert decoded.WhichOneof("layout") == "flex"
    assert decoded.flex.flow == _proto.LayoutFlex.ROW
    assert decoded.flex.gap == 7


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
    assert decoded.name == "disk"
    assert decoded.widgets[0].label.text == "hi"


def test_collect_from_script(tmp_path: Path):
    script = tmp_path / "layout.py"
    script.write_text(
        "from touchy_pad.screens import Screen, button\n"
        "a = Screen('one')\n"
        "a += button('go', text='Go')\n"
        "b = Screen('two')\n"
    )
    found = _collect_from_script(script)
    assert [s.name for s in found] == ["one", "two"]
    # Second invocation should not include the first batch.
    found2 = _collect_from_script(script)
    assert [s.name for s in found2] == ["one", "two"]
