"""Python DSL for authoring Touchy-Pad screen layouts.

This is the host-side counterpart to the firmware's protobuf-driven screen
renderer (see ``docs/why-not-xml.md`` for the design rationale).

Typical usage::

    from touchy_pad.screens import Screen, button, label, grid

    home = Screen("home", layout=grid(cols=4, gap=8))
    home += label("title", text="Hello, Touchy!", font_size=24)
    home += button("play", text="Play", on_click="play_pressed")
    home += button("stop", text="Stop", on_click="stop_pressed")

    home.write("build/screens/home.pb")

Each ``Screen`` instance serialises to a single ``touchy.Screen`` protobuf
message; the firmware loads it via :class:`TouchyClient.file_save` followed
by :class:`TouchyClient.screen_load`. The :command:`touchy screens push`
CLI does both steps for a whole script at once.

Every helper returns a plain ``touchy.Widget`` (or ``Layout`` / ``Style`` /
``Rect``) protobuf message — there is no extra wrapping class. Callers can
freely mix DSL helpers with hand-built protobufs.
"""

from __future__ import annotations

from collections.abc import Iterable
from pathlib import Path

from . import _proto

__all__ = [
    "Screen",
    "absolute",
    "row",
    "col",
    "grid",
    "rect",
    "style",
    "action",
    "button",
    "label",
    "slider",
    "toggle",
    "image",
    "arc",
    "spacer",
    "build_demo_screen",
]


# ---------------------------------------------------------------------------
# Layout / style / action helpers
# ---------------------------------------------------------------------------

def absolute() -> _proto.Layout:
    """Default layout: widgets are positioned by their ``Rect``."""
    return _proto.Layout(kind=_proto.Layout.ABSOLUTE)


def row(gap: int = 0) -> _proto.Layout:
    """Horizontal flex layout."""
    return _proto.Layout(kind=_proto.Layout.ROW, gap=gap)


def col(gap: int = 0) -> _proto.Layout:
    """Vertical flex layout."""
    return _proto.Layout(kind=_proto.Layout.COL, gap=gap)


def grid(cols: int, gap: int = 0) -> _proto.Layout:
    """Even ``cols``-wide grid with ``gap`` pixels between cells."""
    if cols < 1:
        raise ValueError("grid cols must be >= 1")
    return _proto.Layout(kind=_proto.Layout.GRID, cols=cols, gap=gap)


def rect(x: int = 0, y: int = 0, w: int = 0, h: int = 0) -> _proto.Rect:
    """Pixel-space placement. ``0`` for w/h means "size to content"."""
    return _proto.Rect(x=x, y=y, w=w, h=h)


def style(
    bg_color: int | None = None,
    radius: int | None = None,
    border_w: int | None = None,
    pad: int | None = None,
    text_color: int | None = None,
) -> _proto.Style:
    """Cosmetic overrides; unset fields fall back to theme defaults.

    Colours are packed ``0xRRGGBB`` integers. Pass ``None`` to leave a
    property untouched.
    """
    s = _proto.Style()
    if bg_color is not None:
        s.bg_color = bg_color
    if radius is not None:
        s.radius = radius
    if border_w is not None:
        s.border_w = border_w
    if pad is not None:
        s.pad = pad
    if text_color is not None:
        s.text_color = text_color
    return s


def action(event: str) -> _proto.Action:
    """Forward this widget's events to the host as ``LvEvent(user_data=event)``."""
    return _proto.Action(event=event)


# ---------------------------------------------------------------------------
# Widget factories
# ---------------------------------------------------------------------------

def _widget(
    id: str,
    *,
    rect: _proto.Rect | None,
    style: _proto.Style | None,
) -> _proto.Widget:
    w = _proto.Widget(id=id)
    if rect is not None:
        w.rect.CopyFrom(rect)
    if style is not None:
        w.style.CopyFrom(style)
    return w


def button(
    id: str,
    text: str = "",
    on_click: str | _proto.Action | None = None,
    rect: _proto.Rect | None = None,
    style: _proto.Style | None = None,
) -> _proto.Widget:
    """A clickable button with an optional text label."""
    w = _widget(id, rect=rect, style=style)
    w.button.text = text
    if on_click is not None:
        w.button.on_click.CopyFrom(
            on_click if isinstance(on_click, _proto.Action) else action(on_click)
        )
    return w


def label(
    id: str,
    text: str = "",
    font_size: int = 0,
    rect: _proto.Rect | None = None,
    style: _proto.Style | None = None,
) -> _proto.Widget:
    """Static text. ``font_size = 0`` uses the theme default."""
    w = _widget(id, rect=rect, style=style)
    w.label.text = text
    w.label.font_size = font_size
    return w


def slider(
    id: str,
    min: int = 0,
    max: int = 100,
    value: int = 0,
    on_change: str | _proto.Action | None = None,
    rect: _proto.Rect | None = None,
    style: _proto.Style | None = None,
) -> _proto.Widget:
    """A linear slider over ``[min, max]``."""
    w = _widget(id, rect=rect, style=style)
    w.slider.min = min
    w.slider.max = max
    w.slider.value = value
    if on_change is not None:
        w.slider.on_change.CopyFrom(
            on_change if isinstance(on_change, _proto.Action) else action(on_change)
        )
    return w


def toggle(
    id: str,
    on: bool = False,
    on_change: str | _proto.Action | None = None,
    rect: _proto.Rect | None = None,
    style: _proto.Style | None = None,
) -> _proto.Widget:
    """An on/off switch."""
    w = _widget(id, rect=rect, style=style)
    w.toggle.on = on
    if on_change is not None:
        w.toggle.on_change.CopyFrom(
            on_change if isinstance(on_change, _proto.Action) else action(on_change)
        )
    return w


def image(
    id: str,
    asset: str,
    rect: _proto.Rect | None = None,
    style: _proto.Style | None = None,
) -> _proto.Widget:
    """Display a previously-uploaded image asset (``/from_host/<asset>``)."""
    w = _widget(id, rect=rect, style=style)
    w.image.asset = asset
    return w


def arc(
    id: str,
    min: int = 0,
    max: int = 100,
    value: int = 0,
    rect: _proto.Rect | None = None,
    style: _proto.Style | None = None,
) -> _proto.Widget:
    """A circular arc indicator."""
    w = _widget(id, rect=rect, style=style)
    w.arc.min = min
    w.arc.max = max
    w.arc.value = value
    return w


def spacer(
    id: str = "",
    rect: _proto.Rect | None = None,
    style: _proto.Style | None = None,
) -> _proto.Widget:
    """Invisible placeholder. Useful for padding inside flex/grid layouts."""
    w = _widget(id, rect=rect, style=style)
    w.spacer.SetInParent()
    return w


# ---------------------------------------------------------------------------
# Screen — the top-level container
# ---------------------------------------------------------------------------


class Screen:
    """A single LVGL screen, identified by a unique ``name``.

    Instances are also auto-registered into a module-level list so the
    ``touchy screens push`` CLI can discover every screen a script defines
    without requiring the script to return them explicitly.
    """

    # Populated by every ``Screen.__init__`` call; cleared by
    # :func:`_collect_from_script`.
    _registry: list[Screen] = []

    def __init__(
        self,
        name: str,
        layout: _proto.Layout | None = None,
        widgets: Iterable[_proto.Widget] = (),
    ) -> None:
        if not name:
            raise ValueError("Screen name must be non-empty")
        self.name = name
        self.layout = layout if layout is not None else absolute()
        self.widgets: list[_proto.Widget] = list(widgets)
        Screen._registry.append(self)

    # -- builder-style API --------------------------------------------------

    def add(self, widget: _proto.Widget) -> Screen:
        """Append a widget, returning ``self`` for chaining."""
        self.widgets.append(widget)
        return self

    def __iadd__(self, widget: _proto.Widget) -> Screen:
        self.widgets.append(widget)
        return self

    def extend(self, widgets: Iterable[_proto.Widget]) -> Screen:
        self.widgets.extend(widgets)
        return self

    # -- serialisation ------------------------------------------------------

    def to_proto(self) -> _proto.Screen:
        msg = _proto.Screen(name=self.name)
        msg.layout.CopyFrom(self.layout)
        msg.widgets.extend(self.widgets)
        return msg

    def to_bytes(self) -> bytes:
        return self.to_proto().SerializeToString()

    def write(self, path: str | Path) -> Path:
        """Serialise to ``path``, creating parent directories as needed."""
        p = Path(path)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(self.to_bytes())
        return p

    def __repr__(self) -> str:
        return f"Screen(name={self.name!r}, widgets={len(self.widgets)})"


def build_demo_screen(name: str = "demo") -> Screen:
    """Build a sample screen with four different widget kinds.

    Used by the ``touchy screens demo`` CLI subcommand as a smoke test
    for the stage-15 layout pipeline. Layout is a single-column flex so
    each widget gets a clear, full-width row regardless of display size.
    """
    s = Screen(name, layout=col(gap=12))
    s += label(
        "title",
        text="Touchy-Pad demo",
        font_size=24,
        style=style(text_color=0xFFFFFF),
    )
    s += button("hello", text="Hello", on_click="demo_hello")
    s += slider("level", min=0, max=100, value=42, on_change="demo_level")
    s += toggle("enable", on=True, on_change="demo_enable")
    return s


def _collect_from_script(path: str | Path) -> list[Screen]:
    """Execute ``path`` in a fresh namespace and return every Screen created.

    Used by ``touchy screens push``; exposed here so tests can drive it.
    """
    src = Path(path).read_text(encoding="utf-8")
    Screen._registry = []
    namespace: dict[str, object] = {"__name__": "__touchy_screen_script__",
                                    "__file__": str(path)}
    code = compile(src, str(path), "exec")
    exec(code, namespace)  # noqa: S102 — user explicitly asked to run this file
    return list(Screen._registry)
