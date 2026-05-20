"""Tests for the protobuf-backed screen DSL (stage 15) and stage-16 actions."""

from __future__ import annotations

from pathlib import Path

import pytest

from touchy_pad import _proto, hid_keys, macros
from touchy_pad.screens import (
    Screen,
    _collect_from_script,
    arc,
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
    assert decoded.layout.kind == _proto.Layout.COL
    assert decoded.layout.gap == 4
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
    assert kinds == ["button", "label", "slider", "toggle", "checkbox",
                     "image", "arc", "spacer"]
    assert decoded.layout.kind == _proto.Layout.GRID
    assert decoded.layout.cols == 3


def test_style_and_rect_applied():
    s = Screen("x")
    s += button(
        "go",
        text="Go",
        rect=rect(10, 20, 100, 40),
        style=style(bg_color=0xff8800, radius=8, text_color=0xffffff),
    )
    decoded = _proto.Screen.FromString(s.to_bytes())
    w = decoded.widgets[0]
    assert w.rect.x == 10
    assert w.rect.w == 100
    assert w.style.bg_color == 0xff8800
    assert w.style.radius == 8
    assert w.style.text_color == 0xffffff


# -- Stage 16 ---------------------------------------------------------------


def test_macro_action_round_trip():
    """A macro of typed steps decodes to the same step list."""
    s = Screen("m")
    s += button("hi", on_click=macro_action([
        macros.key_tap(hid_keys.KEY_H, hid_keys.MOD_LSHIFT),
        macros.key_tap(hid_keys.KEY_I),
        macros.delay(50),
        macros.set_delay(20),
        macros.mouse_click(),
    ]))
    decoded = _proto.Screen.FromString(s.to_bytes())
    actions = decoded.widgets[0].button.on_click
    assert len(actions) == 1
    assert actions[0].WhichOneof("kind") == "macro"
    steps = actions[0].macro.steps
    assert [st.WhichOneof("step") for st in steps] == [
        "key_tap", "key_tap", "delay_ms", "set_delay_ms", "mouse_click",
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
    s += button("x", on_click=[
        host_action(7),
        macro_action([macros.key_tap(hid_keys.KEY_A)]),
    ])
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
    assert decoded.layout.kind == _proto.Layout.ABSOLUTE


def test_row_layout_helper():
    decoded = _proto.Screen.FromString(Screen("r", layout=row(gap=7)).to_bytes())
    assert decoded.layout.kind == _proto.Layout.ROW
    assert decoded.layout.gap == 7


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
