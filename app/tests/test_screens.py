"""Tests for the protobuf-backed screen DSL (stage 15)."""

from __future__ import annotations

from pathlib import Path

import pytest

from touchy_pad import _proto
from touchy_pad.screens import (
    Screen,
    _collect_from_script,
    action,
    arc,
    button,
    col,
    grid,
    image,
    label,
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
    s += button("play", text="Play", on_click="play_pressed")
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
    assert w0.button.on_click.event == "play_pressed"

    w1 = decoded.widgets[1]
    assert w1.WhichOneof("kind") == "label"
    assert w1.label.text == "Hello"
    assert w1.label.font_size == 24


def test_all_widget_kinds_serialise():
    s = Screen("k", layout=grid(cols=3, gap=2))
    s += button("b", text="B")
    s += label("l", text="L")
    s += slider("s", min=0, max=10, value=5, on_change=action("vol"))
    s += toggle("t", on=True, on_change="mute")
    s += image("i", asset="img/a.png")
    s += arc("a", min=0, max=270, value=90)
    s += spacer("sp", rect=rect(w=20, h=20))

    decoded = _proto.Screen.FromString(s.to_bytes())
    kinds = [w.WhichOneof("kind") for w in decoded.widgets]
    assert kinds == ["button", "label", "slider", "toggle", "image",
                     "arc", "spacer"]
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
