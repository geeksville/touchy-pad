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
    "cell",
    "rect",
    "style",
    "action",
    "host_action",
    "macro_action",
    "button",
    "label",
    "slider",
    "toggle",
    "checkbox",
    "image",
    "arc",
    "spacer",
    "trackpad",
    "log_line",
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


def grid(cols: int, rows: int = 0, gap: int = 0) -> _proto.Layout:
    """LVGL grid layout.

    Divides the parent into ``cols`` equal-width columns (LV_GRID_FR(1)
    each). If ``rows`` is non-zero, also divides the parent into
    ``rows`` equal-height rows; otherwise a single content-sized row
    track is used (the original Stage 15 behaviour). Use :func:`cell`
    to place widgets into specific (col, row) coordinates with
    optional spans.

    See https://lvgl.io/docs/open/examples/layouts/grid for the
    underlying LVGL semantics.
    """
    if cols < 1:
        raise ValueError("grid cols must be >= 1")
    if rows < 0:
        raise ValueError("grid rows must be >= 0")
    return _proto.Layout(kind=_proto.Layout.GRID, cols=cols,
                         rows=rows, gap=gap)


def cell(
    widget: _proto.Widget,
    col: int = 0,
    row: int = 0,
    col_span: int = 1,
    row_span: int = 1,
) -> _proto.Widget:
    """Place ``widget`` inside a grid-layout parent and return it.

    Only meaningful when the enclosing :class:`Screen` was created
    with a :func:`grid` layout. Span defaults to 1x1.
    """
    if col < 0 or row < 0:
        raise ValueError("grid cell col/row must be >= 0")
    if col_span < 1 or row_span < 1:
        raise ValueError("grid cell spans must be >= 1")
    widget.cell.col = col
    widget.cell.row = row
    widget.cell.col_span = col_span
    widget.cell.row_span = row_span
    return widget


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


def action(*args, **kwargs) -> _proto.Action:
    """Build an :class:`_proto.Action`.

    Two forms are supported:

    * ``action(host=<int>)`` — forward the event to the host as an
      ``LvEvent`` carrying ``host_code = <int>``.
    * ``action(macro=[step, ...])`` — a device-side macro built from
      :mod:`touchy_pad.macros` helpers.

    A bare positional integer is treated as the host code (legacy
    convenience matching the pre-stage-16 string form).
    """
    if args and not kwargs:
        if len(args) != 1:
            raise TypeError("action() takes at most one positional argument")
        (value,) = args
        if isinstance(value, int):
            return host_action(value)
        if isinstance(value, list):
            return macro_action(value)
        raise TypeError(f"unsupported positional action value: {value!r}")
    if "host" in kwargs and "macro" not in kwargs:
        return host_action(kwargs["host"])
    if "macro" in kwargs and "host" not in kwargs:
        return macro_action(kwargs["macro"])
    raise TypeError("action() requires exactly one of host= or macro=")


def host_action(code: int) -> _proto.Action:
    """Forward the widget event to the host with the given ``host_code``.

    The host-side :class:`touchy_pad.TouchyClient` dispatches incoming
    ``LvEvent``\\s on ``host_code``: register a callback with
    ``client.on_host_event(code, callback)`` to receive them.
    """
    if code < 0 or code > 0xFFFF_FFFF:
        raise ValueError("host action code must fit in a uint32")
    return _proto.Action(host=_proto.ActionHost(code=code))


def macro_action(steps) -> _proto.Action:
    """Bundle a list of :mod:`touchy_pad.macros` steps as an Action.

    Accepts any iterable of :class:`_proto.MacroStep` instances.
    """
    macro = _proto.ActionMacro()
    macro.steps.extend(list(steps))
    if len(macro.steps) == 0:
        raise ValueError("macro_action requires at least one step")
    return _proto.Action(macro=macro)


def _normalise_actions(actions) -> list[_proto.Action]:
    """Coerce DSL action input into a list of :class:`_proto.Action`.

    Accepts ``None`` (→ empty list), a single ``int`` / ``Action``, or
    any iterable of ``int`` / ``Action`` entries. Plain ints become
    :func:`host_action` codes.
    """
    if actions is None:
        return []
    if isinstance(actions, _proto.Action):
        return [actions]
    if isinstance(actions, int):
        return [host_action(actions)]
    out: list[_proto.Action] = []
    for a in actions:
        if isinstance(a, _proto.Action):
            out.append(a)
        elif isinstance(a, int):
            out.append(host_action(a))
        else:
            raise TypeError(f"unsupported action entry: {a!r}")
    return out


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
    on_click=None,
    rect: _proto.Rect | None = None,
    style: _proto.Style | None = None,
) -> _proto.Widget:
    """A clickable button with an optional text label.

    ``on_click`` accepts a single :class:`_proto.Action`, an ``int`` host
    code, or a list mixing both. Pass ``None`` (the default) for buttons
    that do nothing on click.
    """
    w = _widget(id, rect=rect, style=style)
    w.button.text = text
    w.button.on_click.extend(_normalise_actions(on_click))
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
    on_change=None,
    rect: _proto.Rect | None = None,
    style: _proto.Style | None = None,
) -> _proto.Widget:
    """A linear slider over ``[min, max]``."""
    w = _widget(id, rect=rect, style=style)
    w.slider.min = min
    w.slider.max = max
    w.slider.value = value
    w.slider.on_change.extend(_normalise_actions(on_change))
    return w


def toggle(
    id: str,
    on: bool = False,
    on_change=None,
    rect: _proto.Rect | None = None,
    style: _proto.Style | None = None,
) -> _proto.Widget:
    """An on/off switch."""
    w = _widget(id, rect=rect, style=style)
    w.toggle.on = on
    w.toggle.on_change.extend(_normalise_actions(on_change))
    return w


def checkbox(
    id: str,
    text: str = "",
    checked: bool = False,
    on_change=None,
    rect: _proto.Rect | None = None,
    style: _proto.Style | None = None,
) -> _proto.Widget:
    """A labelled tickbox."""
    w = _widget(id, rect=rect, style=style)
    w.checkbox.text = text
    w.checkbox.checked = checked
    w.checkbox.on_change.extend(_normalise_actions(on_change))
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


def trackpad(
    id: str,
    rect: _proto.Rect | None = None,
    style: _proto.Style | None = None,
) -> _proto.Widget:
    """Multitouch trackpad surface (device-side HID mouse).

    Touches inside the widget become USB HID mouse events on the device:
    one-finger drag → move, 1/2/3-finger tap → left / right / middle
    click. Recognised gestures are echoed to the shared device log sink,
    so placing a :func:`log_line` on the same screen surfaces them to
    the user.
    """
    w = _widget(id, rect=rect, style=style)
    w.trackpad.SetInParent()
    return w


def log_line(
    id: str = "log",
    rect: _proto.Rect | None = None,
    style: _proto.Style | None = None,
) -> _proto.Widget:
    """One-line readout of the most recent device log message.

    Subscribes to the firmware's shared log sink (see
    ``firmware/main/log_line.{h,cpp}``); the :func:`trackpad` widget
    and other subsystems push status lines through that sink.
    """
    w = _widget(id, rect=rect, style=style)
    w.log.SetInParent()
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
    """Build a sample screen exercising stages 16 and 18.

    Used by the ``touchy screens demo`` CLI subcommand as a smoke test
    for the action pipeline and the on-device trackpad widget.

    Layout is a 2-column / 6-row LVGL :func:`grid`. The grid manager
    sizes every track to the parent (LV_GRID_FR(1) each), so the
    layout adapts to the display resolution without any hard-coded
    pixel coordinates.

    Wiring:

    * Left column (rows 0-4): title label, two buttons, a slider, and
      a checkbox.
        - "hello" button → device-side macro that types ``"hi"`` over
          USB HID (no host round-trip).
        - "ping" button → host action 0x100 (demo CLI prints incoming
          events).
        - slider        → host action 0x101 (extra carries int32 LE).
        - checkbox      → host action 0x102 (extra is 1 byte).
    * Right column (rows 0-4, ``row_span=5``): the :func:`trackpad`
      surface for USB HID mouse output.
    * Bottom strip (row 5, ``col_span=2``): a :func:`log_line` that
      mirrors the device's most recent log message — e.g. each
      recognised trackpad gesture.
    """
    from . import hid_keys as k
    from . import macros as m

    s = Screen(name, layout=grid(cols=4, rows=6, gap=8))

    # ── left column: stacked control widgets ───────────────────────────
    s += cell(label("title", text="Demo", font_size=12,
                    style=style(text_color=0xFFFFFF)),
              col=0, row=0)
    s += cell(button("hello", text="Type 'hi'",
                     on_click=macro_action([
                         m.key_tap(k.KEY_H, k.MOD_LSHIFT),
                         m.key_tap(k.KEY_I),
                     ])),
              col=0, row=1)
    s += cell(button("ping", text="Ping host", on_click=host_action(0x100)),
              col=0, row=2)
    s += cell(slider("level", min=0, max=100, value=42,
                     on_change=host_action(0x101)),
              col=0, row=3)
    s += cell(checkbox("enable", text="Enabled", checked=True,
                       on_change=host_action(0x102)),
              col=0, row=4)

    # ── right column: multitouch trackpad spans rows 0..4 ──────────────
    s += cell(trackpad("pad"), col=1, row=0, row_span=5, col_span=3)

    # ── bottom strip: log readout spans both columns ───────────────────
    s += cell(log_line("log"), col=0, row=5, col_span=4)
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
