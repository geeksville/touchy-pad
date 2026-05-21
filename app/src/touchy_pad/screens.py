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
    "flex",
    "row",
    "col",
    "grid",
    "cell",
    "rect",
    "style",
    "transition",
    "action",
    "host_action",
    "macro_action",
    "button",
    "label",
    "slider",
    "toggle",
    "checkbox",
    "image",
    "image_button",
    "arc",
    "spacer",
    "trackpad",
    "log_line",
    "build_demo_screen",
    # LvState / LvPart selector bits (see widgets.proto:LvState).
    "STATE_DEFAULT",
    "STATE_CHECKED",
    "STATE_FOCUSED",
    "STATE_FOCUS_KEY",
    "STATE_EDITED",
    "STATE_HOVERED",
    "STATE_PRESSED",
    "STATE_SCROLLED",
    "STATE_DISABLED",
    "PART_MAIN",
    "PART_SCROLLBAR",
    "PART_INDICATOR",
    "PART_KNOB",
    "PART_SELECTED",
    "PART_ITEMS",
    "PART_CURSOR",
    "PART_ANY",
    # StyleProp values for use with `transition(props=[...])`.
    "PROP_BG_COLOR",
    "PROP_BG_OPA",
    "PROP_RADIUS",
    "PROP_BORDER_WIDTH",
    "PROP_BORDER_COLOR",
    "PROP_PAD_TOP",
    "PROP_PAD_BOTTOM",
    "PROP_PAD_LEFT",
    "PROP_PAD_RIGHT",
    "PROP_TEXT_COLOR",
    "PROP_IMAGE_RECOLOR",
    "PROP_IMAGE_RECOLOR_OPA",
    "PROP_TRANSFORM_WIDTH",
    "PROP_TRANSFORM_HEIGHT",
    # AnimPath easing curves for use with `transition(path=...)`.
    "ANIM_PATH_LINEAR",
    "ANIM_PATH_EASE_IN",
    "ANIM_PATH_EASE_OUT",
    "ANIM_PATH_EASE_IN_OUT",
    "ANIM_PATH_OVERSHOOT",
    "ANIM_PATH_BOUNCE",
    "ANIM_PATH_STEP",
]


# ---------------------------------------------------------------------------
# LVGL style selector bits (LvState + LvPart, see widgets.proto)
# ---------------------------------------------------------------------------
#
# Pass these (OR'd together) as `for_state=` to :func:`style` to target a
# specific state and/or sub-part. Default (0) = main part, default state.
STATE_DEFAULT = _proto.LvState.LV_STATE_DEFAULT
STATE_CHECKED = _proto.LvState.LV_STATE_CHECKED
STATE_FOCUSED = _proto.LvState.LV_STATE_FOCUSED
STATE_FOCUS_KEY = _proto.LvState.LV_STATE_FOCUS_KEY
STATE_EDITED = _proto.LvState.LV_STATE_EDITED
STATE_HOVERED = _proto.LvState.LV_STATE_HOVERED
STATE_PRESSED = _proto.LvState.LV_STATE_PRESSED
STATE_SCROLLED = _proto.LvState.LV_STATE_SCROLLED
STATE_DISABLED = _proto.LvState.LV_STATE_DISABLED
PART_MAIN = _proto.LvState.LV_PART_MAIN
PART_SCROLLBAR = _proto.LvState.LV_PART_SCROLLBAR
PART_INDICATOR = _proto.LvState.LV_PART_INDICATOR
PART_KNOB = _proto.LvState.LV_PART_KNOB
PART_SELECTED = _proto.LvState.LV_PART_SELECTED
PART_ITEMS = _proto.LvState.LV_PART_ITEMS
PART_CURSOR = _proto.LvState.LV_PART_CURSOR
PART_ANY = _proto.LvState.LV_PART_ANY


# ---------------------------------------------------------------------------
# StyleProp / AnimPath constants — used with :func:`transition`.
# ---------------------------------------------------------------------------
#
# These mirror the curated wire-stable subsets defined in
# ``widgets.proto`` (``StyleProp`` and ``AnimPath``). The firmware
# translates each into the matching ``LV_STYLE_*`` constant or
# ``lv_anim_path_*`` callback at decode time.
PROP_BG_COLOR = _proto.StyleProp.STYLE_PROP_BG_COLOR
PROP_BG_OPA = _proto.StyleProp.STYLE_PROP_BG_OPA
PROP_RADIUS = _proto.StyleProp.STYLE_PROP_RADIUS
PROP_BORDER_WIDTH = _proto.StyleProp.STYLE_PROP_BORDER_WIDTH
PROP_BORDER_COLOR = _proto.StyleProp.STYLE_PROP_BORDER_COLOR
PROP_PAD_TOP = _proto.StyleProp.STYLE_PROP_PAD_TOP
PROP_PAD_BOTTOM = _proto.StyleProp.STYLE_PROP_PAD_BOTTOM
PROP_PAD_LEFT = _proto.StyleProp.STYLE_PROP_PAD_LEFT
PROP_PAD_RIGHT = _proto.StyleProp.STYLE_PROP_PAD_RIGHT
PROP_TEXT_COLOR = _proto.StyleProp.STYLE_PROP_TEXT_COLOR
PROP_IMAGE_RECOLOR = _proto.StyleProp.STYLE_PROP_IMAGE_RECOLOR
PROP_IMAGE_RECOLOR_OPA = _proto.StyleProp.STYLE_PROP_IMAGE_RECOLOR_OPA
PROP_TRANSFORM_WIDTH = _proto.StyleProp.STYLE_PROP_TRANSFORM_WIDTH
PROP_TRANSFORM_HEIGHT = _proto.StyleProp.STYLE_PROP_TRANSFORM_HEIGHT

ANIM_PATH_LINEAR = _proto.AnimPath.ANIM_PATH_LINEAR
ANIM_PATH_EASE_IN = _proto.AnimPath.ANIM_PATH_EASE_IN
ANIM_PATH_EASE_OUT = _proto.AnimPath.ANIM_PATH_EASE_OUT
ANIM_PATH_EASE_IN_OUT = _proto.AnimPath.ANIM_PATH_EASE_IN_OUT
ANIM_PATH_OVERSHOOT = _proto.AnimPath.ANIM_PATH_OVERSHOOT
ANIM_PATH_BOUNCE = _proto.AnimPath.ANIM_PATH_BOUNCE
ANIM_PATH_STEP = _proto.AnimPath.ANIM_PATH_STEP


# ---------------------------------------------------------------------------
# Layout / style / action helpers
# ---------------------------------------------------------------------------


def absolute() -> _proto.LayoutAbsolute:
    """Default layout: widgets are positioned by their ``Rect``."""
    return _proto.LayoutAbsolute()


def flex(
    flow: _proto.LayoutFlex.Flow = _proto.LayoutFlex.ROW,
    gap: int = 0,
) -> _proto.LayoutFlex:
    """LVGL flex layout.

    ``flow`` is a :class:`_proto.LayoutFlex.Flow` constant
    (e.g. ``LayoutFlex.ROW``, ``LayoutFlex.COLUMN``). The convenience
    aliases :func:`row` and :func:`col` cover the most common cases.
    """
    return _proto.LayoutFlex(flow=flow, gap=gap)


def row(gap: int = 0) -> _proto.LayoutFlex:
    """Horizontal flex layout (LV_FLEX_FLOW_ROW)."""
    return _proto.LayoutFlex(flow=_proto.LayoutFlex.ROW, gap=gap)


def col(gap: int = 0) -> _proto.LayoutFlex:
    """Vertical flex layout (LV_FLEX_FLOW_COLUMN)."""
    return _proto.LayoutFlex(flow=_proto.LayoutFlex.COLUMN, gap=gap)


def grid(cols: int, rows: int = 0, gap: int = 0) -> _proto.LayoutGrid:
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
    return _proto.LayoutGrid(cols=cols, rows=rows, gap=gap)


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
    if col_span != 1:
        widget.cell.col_span = col_span
    if row_span != 1:
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
    for_state: int = 0,
    recolor: int | None = None,
    recolor_opa: int | None = None,
    transform_width: int | None = None,
    transition: _proto.Transition | None = None,
) -> _proto.Style:
    """Cosmetic overrides; unset fields fall back to theme defaults.

    Colours are packed ``0xRRGGBB`` integers. Pass ``None`` to leave a
    property untouched. Every visual field is wire-level ``optional``,
    so explicit zeros (``bg_color=0x000000``, ``transform_width=0``)
    round-trip faithfully — they are *not* treated as "unset".

    ``for_state`` is the LVGL style selector — a bitwise OR of
    ``STATE_*`` and/or ``PART_*`` constants (see
    :class:`_proto.LvState`). The default (0) targets the main part in
    the default state. Example::

        style(bg_color=0x1E90FF, for_state=STATE_PRESSED)

    Other knobs:

    * ``recolor`` (``0xRRGGBB``) and ``recolor_opa`` (0..255) tint
      images / image-buttons; mirrors
      ``lv_style_set_image_recolor`` / ``_image_recolor_opa``.
    * ``transform_width`` adds (or subtracts) pixels from the widget's
      drawn width while the style is active. Combine with
      :func:`transition` for a smooth "grow on press" effect.
    * ``transition`` attaches a :class:`_proto.Transition` so LVGL
      animates the listed properties when the style is added / removed
      from the widget's selector match (e.g. entering / leaving
      ``STATE_PRESSED``). See :func:`transition` for the pattern.

    Stack several ``Style`` instances on a widget by passing a list as
    the factory's ``style=`` argument.
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
    if for_state:
        s.for_state = for_state
    if recolor is not None:
        s.recolor = recolor
    if recolor_opa is not None:
        if recolor_opa < 0 or recolor_opa > 255:
            raise ValueError("recolor_opa must fit in a uint8 (0..255)")
        s.recolor_opa = recolor_opa
    if transform_width is not None:
        s.transform_width = transform_width
    if transition is not None:
        s.transition.CopyFrom(transition)
    return s


def transition(
    props: Iterable[int],
    path: int = ANIM_PATH_LINEAR,
    duration_ms: int = 200,
    delay_ms: int = 0,
) -> _proto.Transition:
    """Smoothly animate style properties when the style becomes active.

    Mirrors ``lv_style_transition_dsc_t``. Pass the returned object as
    the ``transition=`` argument to :func:`style`. When the parent style
    is added or removed from a widget's selector match (e.g. entering /
    leaving ``STATE_PRESSED``) LVGL interpolates every ``prop`` in the
    list from the previous value to the new one along ``path`` over
    ``duration_ms``, starting after ``delay_ms``.

    The classic "bind on the default style → covers both directions"
    pattern works: attach one transition to the default-state Style and
    LVGL uses it for both press and release. For asymmetric in/out
    timings, attach a different transition to the pressed-state Style.

    Example — image-button that widens by 20 px and darkens when
    pressed, both directions animated linearly over 200 ms::

        image_button(
            "smile",
            asset="images/smiley.png",
            style=[
                style(transition=transition(
                    props=[PROP_TRANSFORM_WIDTH, PROP_IMAGE_RECOLOR_OPA],
                    duration_ms=200,
                )),
                style(
                    transform_width=20,
                    recolor=0x000000,
                    recolor_opa=80,        # ≈ LV_OPA_30
                    for_state=STATE_PRESSED,
                ),
            ],
        )

    See https://lvgl.io/docs/open/main-modules/animation for the
    underlying animation engine.
    """
    props_list = [int(p) for p in props]
    if not props_list:
        raise ValueError("transition() requires at least one StyleProp")
    if duration_ms < 0 or delay_ms < 0:
        raise ValueError("transition durations must be non-negative")
    t = _proto.Transition(
        path=path,
        duration_ms=duration_ms,
        delay_ms=delay_ms,
    )
    t.props.extend(props_list)
    return t


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


def _normalise_styles(
    style: _proto.Style | Iterable[_proto.Style] | None,
) -> list[_proto.Style]:
    """Coerce a widget factory's ``style=`` argument to a flat list.

    Accepts ``None``, a single :class:`_proto.Style`, or any iterable of
    them. The returned list is what callers pass to
    ``Widget.styles.extend``.
    """
    if style is None:
        return []
    if isinstance(style, _proto.Style):
        return [style]
    return list(style)


def _widget(
    id: str,
    *,
    rect: _proto.Rect | None,
    style: _proto.Style | Iterable[_proto.Style] | None,
) -> _proto.Widget:
    w = _proto.Widget(id=id)
    if rect is not None:
        w.rect.CopyFrom(rect)
    w.styles.extend(_normalise_styles(style))
    return w


def button(
    id: str,
    text: str = "",
    on_click=None,
    rect: _proto.Rect | None = None,
    style: _proto.Style | Iterable[_proto.Style] | None = None,
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
    style: _proto.Style | Iterable[_proto.Style] | None = None,
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
    style: _proto.Style | Iterable[_proto.Style] | None = None,
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
    style: _proto.Style | Iterable[_proto.Style] | None = None,
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
    style: _proto.Style | Iterable[_proto.Style] | None = None,
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
    style: _proto.Style | Iterable[_proto.Style] | None = None,
    scale: int | float | None = None,
    rotation: int | float | None = None,
) -> _proto.Widget:
    """Display a previously-uploaded image asset (``/from_host/<asset>``).

    ``scale`` is a multiplier (``1.0`` / ``1`` / ``256`` all mean 100%):
    floats are interpreted as a multiplier (``0.5`` = 50%, ``2.0`` =
    200%), ints ``> 16`` are taken as raw LVGL units (256 = 100%) and
    ints ``<= 16`` as a multiplier. ``rotation`` is in degrees (floats
    or ints); fractional degrees are honoured down to 0.1°.
    """
    w = _widget(id, rect=rect, style=style)
    _fill_image(w.image, asset, scale, rotation)
    return w


def image_button(
    id: str,
    asset: str,
    pressed_asset: str | None = None,
    on_click=None,
    rect: _proto.Rect | None = None,
    style: _proto.Style | Iterable[_proto.Style] | None = None,
    scale: int | float | None = None,
    rotation: int | float | None = None,
    pressed_scale: int | float | None = None,
    pressed_rotation: int | float | None = None,
) -> _proto.Widget:
    """Clickable image button backed by uploaded assets.

    The released-state image is described by an embedded ``Image`` (so
    it accepts the same ``asset`` / ``scale`` / ``rotation`` knobs as
    :func:`image`). ``pressed_asset`` is optional: pass ``None`` (the
    default) to leave LVGL's PRESSED state unset; LVGL then renders the
    released image while pressed. ``pressed_scale`` / ``pressed_rotation``
    let the press-state image have its own transform — they default to
    the released-state values when omitted but a ``pressed_asset`` is
    provided. ``on_click`` accepts the same shapes as :func:`button`'s
    ``on_click``.
    """
    w = _widget(id, rect=rect, style=style)
    _fill_image(w.image_button.released, asset, scale, rotation)
    # If the caller asked for a press-state visual change (different
    # asset, scale, or rotation) populate the embedded `pressed` Image.
    # Falling back to the released asset/scale/rotation lets a caller
    # change just one field without re-specifying the bitmap.
    has_press_override = (
        pressed_asset is not None or pressed_scale is not None or pressed_rotation is not None
    )
    if has_press_override:
        _fill_image(
            w.image_button.pressed,
            pressed_asset if pressed_asset is not None else asset,
            pressed_scale if pressed_scale is not None else scale,
            pressed_rotation if pressed_rotation is not None else rotation,
        )
    w.image_button.on_click.extend(_normalise_actions(on_click))
    return w


def _fill_image(
    msg: _proto.Image,
    asset: str,
    scale: int | float | None,
    rotation: int | float | None,
) -> None:
    """Populate a ``touchy.Image`` submessage from DSL kwargs."""
    # Match the host-side conversion done by ``TouchyClient.file_save``:
    # any BMP/PNG/JPEG/GIF/WebP gets converted to LVGL ``.bin`` and
    # renamed accordingly, so the asset path stored in the screen has
    # to track that rename or LVGL won't find the file on flash.
    from .lvgl_image import rewrite_to_bin_path

    msg.asset = rewrite_to_bin_path(asset)
    if scale is not None:
        msg.scale = _encode_scale(scale)
    if rotation is not None:
        msg.rotation = _encode_rotation(rotation)


def _encode_scale(value: int | float) -> int:
    """Encode a user-friendly scale value as LVGL's 1/256-unit integer.

    ``float`` → multiplier (``1.0`` = 100%). Small ints (``<= 16``) are
    also treated as a multiplier so ``scale=2`` means 200%; larger ints
    pass through verbatim (``256`` = 100%).
    """
    if isinstance(value, float):
        units = round(value * 256)
    elif isinstance(value, int):
        units = value * 256 if value <= 16 else value
    else:
        raise TypeError(f"scale must be int or float, got {type(value).__name__}")
    if units < 0 or units > 0xFFFF:
        raise ValueError(f"scale out of range (0..65535 in 1/256 units): {units}")
    return units


def _encode_rotation(value: int | float) -> int:
    """Encode rotation in degrees (any sign) as LVGL's tenths-of-degree int."""
    if not isinstance(value, int | float):
        raise TypeError(f"rotation must be int or float, got {type(value).__name__}")
    return int(round(float(value) * 10))


def arc(
    id: str,
    min: int = 0,
    max: int = 100,
    value: int = 0,
    rect: _proto.Rect | None = None,
    style: _proto.Style | Iterable[_proto.Style] | None = None,
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
    style: _proto.Style | Iterable[_proto.Style] | None = None,
) -> _proto.Widget:
    """Invisible placeholder. Useful for padding inside flex/grid layouts."""
    w = _widget(id, rect=rect, style=style)
    w.spacer.SetInParent()
    return w


def trackpad(
    id: str,
    rect: _proto.Rect | None = None,
    style: _proto.Style | Iterable[_proto.Style] | None = None,
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
    style: _proto.Style | Iterable[_proto.Style] | None = None,
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
        layout: _proto.LayoutAbsolute | _proto.LayoutFlex | _proto.LayoutGrid | None = None,
        widgets: Iterable[_proto.Widget] = (),
    ) -> None:
        if not name:
            raise ValueError("Screen name must be non-empty")
        self.name = name
        self.layout: _proto.LayoutAbsolute | _proto.LayoutFlex | _proto.LayoutGrid = (
            layout if layout is not None else absolute()
        )
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
        msg = _proto.Screen(name=self.name, version=_proto.Screen.Version.CURRENT)
        if isinstance(self.layout, _proto.LayoutFlex):
            msg.flex.CopyFrom(self.layout)
        elif isinstance(self.layout, _proto.LayoutGrid):
            msg.grid.CopyFrom(self.layout)
        else:
            msg.absolute.SetInParent()
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
        - smiley image-button (Stage 20) → host action 0x103. The asset
          lives at ``/from_host/images/smiley.png`` (auto-converted to
          LVGL's native .bin format on upload) and is uploaded by the
          ``touchy screens demo`` command alongside this screen.
    * Right column (rows 0-3, ``row_span=4``): the :func:`trackpad`
      surface for USB HID mouse output.
    * Bottom strip (row 5, ``col_span=2``): a :func:`log_line` that
      mirrors the device's most recent log message — e.g. each
      recognised trackpad gesture.
    """
    from . import hid_keys as k
    from . import macros as m

    s = Screen(name, layout=grid(cols=4, rows=6, gap=8))

    # ── left column: stacked control widgets ───────────────────────────
    # The "hello" button mirrors the smiley's transition pattern on a
    # non-image widget so we can tell whether transitions are working in
    # general or only on image buttons. Default style carries a 200 ms
    # linear transition over (TRANSFORM_WIDTH, BG_COLOR); pressed state
    # widens by 20 px and tints the background red.
    s += cell(
        button(
            "hello",
            text="Type 'hi'",
            on_click=macro_action(
                [
                    m.key_tap(k.KEY_H, k.MOD_LSHIFT),
                    m.key_tap(k.KEY_I),
                ]
            ),
            style=[
                style(
                    transition=transition(
                        props=[PROP_TRANSFORM_WIDTH, PROP_BG_COLOR],
                        path=ANIM_PATH_LINEAR,
                        duration_ms=200,
                    )
                ),
                style(
                    transform_width=20,
                    bg_color=0xCC2222,
                    for_state=STATE_PRESSED,
                ),
            ],
        ),
        col=0,
        row=0,
    )
    s += cell(button("ping", text="Ping host", on_click=host_action(0x100)), col=0, row=1)
    s += cell(slider("level", min=0, max=100, value=42, on_change=host_action(0x101)), col=0, row=2)
    s += cell(
        checkbox("enable", text="Enabled", checked=True, on_change=host_action(0x102)), col=0, row=3
    )

    # ── right column: multitouch trackpad spans rows 0..3 ──────────────
    s += cell(trackpad("pad"), col=1, row=0, row_span=5, col_span=3)

    # ── Stage 20.2 smiley image-button: row 4, left column ─────────────
    # Demonstrates style transitions on an image button. Effects are
    # cranked up for visual debugging — if these don't show but the
    # "Type 'hi'" button's transition does, the bug is specific to
    # lv_imagebutton's rendering path.
    s += cell(
        image_button(
            "smile",
            asset="images/smiley.png",
            on_click=host_action(0x103),
            scale=2.0,  # 200% — source asset is only 16x16 px
            pressed_scale=2.5,  # bump scale while held for visible feedback
            style=[
                style(
                    transition=transition(
                        props=[PROP_TRANSFORM_WIDTH, PROP_IMAGE_RECOLOR_OPA, PROP_BG_COLOR],
                        path=ANIM_PATH_LINEAR,
                        duration_ms=300,
                    )
                ),
                style(
                    transform_width=80,  # +80 px on each side
                    recolor=0xFF0000,
                    recolor_opa=255,  # full red tint
                    bg_color=0x00FF00,  # bright green background
                    for_state=STATE_PRESSED,
                ),
            ],
        ),
        col=0,
        row=4,
    )

    # ── bottom strip: log readout spans both columns ───────────────────
    s += cell(log_line("log"), col=0, row=5, col_span=4)
    return s


def _collect_from_script(path: str | Path) -> list[Screen]:
    """Execute ``path`` in a fresh namespace and return every Screen created.

    Used by ``touchy screens push``; exposed here so tests can drive it.
    """
    src = Path(path).read_text(encoding="utf-8")
    Screen._registry = []
    namespace: dict[str, object] = {"__name__": "__touchy_screen_script__", "__file__": str(path)}
    code = compile(src, str(path), "exec")
    exec(code, namespace)  # noqa: S102 — user explicitly asked to run this file
    return list(Screen._registry)
