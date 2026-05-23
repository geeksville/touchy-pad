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

from .. import _proto

__all__ = [
    "Screen",
    "Layer",
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
    "device_action",
    "switch_screen_action",
    "next_screen_action",
    "prev_screen_action",
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
    "ripple_animation",
    "log_line",
    "fps",
    "force_render",
    "build_demo_screen",
    "build_demo_screens",
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
            asset="F:host/images/smiley.png",
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


# ---------------------------------------------------------------------------
# Device-side actions (Stage 24)
# ---------------------------------------------------------------------------
#
# ``ActionDevice`` is the third Action subtype, alongside ``ActionHost`` and
# ``ActionMacro``. Unlike ``ActionMacro`` (which only replays HID events)
# device actions ask the firmware to change its own state — currently only
# *which* screen is loaded, via ``ActionSwitchScreen``. They run entirely
# on-device with no host round-trip, so paged UIs keep responding when
# the host computer is asleep or unplugged.


def device_action(device: _proto.ActionDevice) -> _proto.Action:
    """Wrap a pre-built :class:`_proto.ActionDevice` as an :class:`Action`.

    Most callers want the higher-level :func:`switch_screen_action` /
    :func:`next_screen_action` / :func:`prev_screen_action` helpers
    instead; this exists for forward-compatibility with new
    ``ActionDevice`` sub-kinds the firmware may add later.
    """
    return _proto.Action(device=device)


def switch_screen_action(
    path: str | None = None,
    behavior: int | None = None,
    *,
    name: str | None = None,
) -> _proto.Action:
    """Build an Action that swaps the active screen on-device.

    Parameters
    ----------
    path:
        Full drive-prefixed path of the target screen (e.g.
        ``"F:host/screens/home.pb"``), matching the path :class:`Screen`
        uploads to. Required when ``behavior`` is
        :attr:`ActionSwitchScreen.BY_PATH` (the default), ignored
        otherwise.
    behavior:
        One of :attr:`ActionSwitchScreen.BY_PATH` (0),
        :attr:`ActionSwitchScreen.NEXT` (1) or
        :attr:`ActionSwitchScreen.PREVIOUS` (2). NEXT/PREVIOUS step
        through the firmware's registry in stable iteration order
        (alphabetical, because the registry is a ``std::map``) and wrap
        around at the ends.
    name:
        Deprecated alias for ``path``, kept so pre-stage-51 callers
        still compile. If both are given, ``path`` wins. When only
        ``name`` is supplied we assume the screen lives in flash and
        construct the canonical ``F:host/screens/<name>.pb`` path.

    The convenience wrappers :func:`next_screen_action` and
    :func:`prev_screen_action` cover the most common case.
    """
    Behavior = _proto.ActionSwitchScreen.Behavior  # noqa: N806
    if behavior is None:
        behavior = Behavior.BY_PATH
    if path is None and name:
        # Back-compat: synthesise the canonical flash path.
        path = f"F:host/screens/{name}.pb"
    if behavior == Behavior.BY_PATH:
        if not path:
            raise ValueError("switch_screen_action(BY_PATH) requires path=")
        ss = _proto.ActionSwitchScreen(behavior=behavior, path=path)
    else:
        # NEXT / PREVIOUS ignore `path`; we drop any value silently so
        # callers can pass switch_screen_action(path="x", behavior=NEXT)
        # without it being a confusing error.
        ss = _proto.ActionSwitchScreen(behavior=behavior)
    return device_action(_proto.ActionDevice(switch_screen=ss))


def next_screen_action() -> _proto.Action:
    """Step to the next screen in the device registry (wraps around)."""
    return switch_screen_action(behavior=_proto.ActionSwitchScreen.NEXT)


def prev_screen_action() -> _proto.Action:
    """Step to the previous screen in the device registry (wraps around)."""
    return switch_screen_action(behavior=_proto.ActionSwitchScreen.PREVIOUS)


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
    on_press=None,
    on_release=None,
    rect: _proto.Rect | None = None,
    style: _proto.Style | Iterable[_proto.Style] | None = None,
) -> _proto.Widget:
    """A clickable button with an optional text label.

    ``on_click`` fires once on a completed press+release (and is
    suppressed when the press is cancelled by a scroll or press-lost).
    ``on_press`` / ``on_release`` fire on the raw edges so hold-aware
    host code (StreamDeck plugins, MIDI note-on/note-off, ...) can see
    both transitions; the press-lost case also triggers ``on_release``
    so callers always observe matching press/release pairs. The host
    distinguishes edges via ``LvEvent.code`` (1=PRESSED, 8=RELEASED,
    7=CLICKED). Each parameter accepts a single :class:`_proto.Action`,
    an ``int`` host code, or a list mixing both; pass ``None`` (the
    default) for buttons that don't react on that edge.
    """
    w = _widget(id, rect=rect, style=style)
    w.button.text = text
    w.button.on_click.extend(_normalise_actions(on_click))
    w.button.on_press.extend(_normalise_actions(on_press))
    w.button.on_release.extend(_normalise_actions(on_release))
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
    on_press=None,
    on_release=None,
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
    w.image_button.on_press.extend(_normalise_actions(on_press))
    w.image_button.on_release.extend(_normalise_actions(on_release))
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


def ripple_animation(
    *,
    start_opa: int = 200,
    max_radius: int = 40,
    duration_ms: int = 350,
    path: int = ANIM_PATH_LINEAR,
    border_width: int = 0,
) -> _proto.RippleAnimation:
    """Touch-feedback ripple descriptor.

    A circle grows from radius 0 to ``max_radius`` while its opacity
    fades from ``start_opa`` (0-255) to 0 over ``duration_ms``, eased by
    ``path``. ``border_width=0`` (default) renders a filled disc;
    ``> 0`` renders a hollow ring of that thickness. Color is *not*
    specified here — :func:`trackpad` picks it at spawn time from its
    per-finger-count palette so one descriptor can be reused across
    gestures.
    """
    r = _proto.RippleAnimation()
    r.start_opa = start_opa
    r.max_radius = max_radius
    r.duration_ms = duration_ms
    r.path = path
    r.border_width = border_width
    return r


def trackpad(
    id: str,
    rect: _proto.Rect | None = None,
    style: _proto.Style | Iterable[_proto.Style] | None = None,
    *,
    scroll_invert_y: bool = False,
    scroll_invert_x: bool = False,
    left_touch_color: int | None = None,
    right_touch_color: int | None = None,
    middle_touch_color: int | None = None,
    touch_ripple: _proto.RippleAnimation | None = None,
    tap_ripple: _proto.RippleAnimation | None = None,
    scrollbar_color: int | None = None,
    tap_max_ms: int | None = None,
) -> _proto.Widget:
    """Multitouch trackpad surface (device-side HID mouse).

    Touches inside the widget become USB HID mouse events on the device:
    one-finger drag → move, two-finger drag → scroll wheel,
    1/2/3-finger tap → left / right / middle click. Recognised gestures
    are echoed to the shared device log sink, so placing a
    :func:`log_line` on the same screen surfaces them to the user.

    Set ``scroll_invert_y`` / ``scroll_invert_x`` true for macOS-style
    "natural scrolling".

    Optional ripple animations:

    * ``touch_ripple`` spawns at each finger touch-down.
    * ``tap_ripple`` spawns at the centroid of the touched fingers when
      a tap (no drag) is recognised on release.

    Both pick their color from ``left_touch_color`` /
    ``right_touch_color`` / ``middle_touch_color`` (0xRRGGBB) based on
    the gesture's finger count, matching the click convention. Unset
    colors fall back to firmware defaults (cyan / orange / magenta).

    ``scrollbar_color`` (0xRRGGBB), if set, enables a thin progress bar
    that grows along the active scroll axis when a two-finger scroll
    starts and fades out when all fingers lift. ``tap_max_ms`` overrides
    the tap-vs-drag hold threshold (default 200 ms on-device).
    """
    w = _widget(id, rect=rect, style=style)
    tp = w.trackpad
    tp.scroll_invert_y = scroll_invert_y
    tp.scroll_invert_x = scroll_invert_x
    if left_touch_color is not None:
        tp.left_touch_color = left_touch_color
    if right_touch_color is not None:
        tp.right_touch_color = right_touch_color
    if middle_touch_color is not None:
        tp.middle_touch_color = middle_touch_color
    if touch_ripple is not None:
        tp.touch_ripple.CopyFrom(touch_ripple)
    if tap_ripple is not None:
        tp.tap_ripple.CopyFrom(tap_ripple)
    if scrollbar_color is not None:
        tp.scrollbar_color = scrollbar_color
    if tap_max_ms is not None:
        tp.tap_max_ms = tap_max_ms
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


def fps(
    id: str = "fps",
    rect: _proto.Rect | None = None,
    style: _proto.Style | Iterable[_proto.Style] | None = None,
) -> _proto.Widget:
    """Live frames-per-second readout for the active LVGL display.

    Renders as a small label that the firmware refreshes ~twice per
    second from a counter incremented on every display flush. Useful
    while iterating on layouts to spot the cost of expensive widgets
    (image scaling, transitions, ...).
    """
    w = _widget(id, rect=rect, style=style)
    w.fps.SetInParent()
    return w


def force_render(
    id: str = "force_render",
    rect: _proto.Rect | None = None,
    style: _proto.Style | Iterable[_proto.Style] | None = None,
) -> _proto.Widget:
    """Dev / benchmark toggle that pins LVGL to maximum redraw rate.

    Renders on-device as an LVGL checkbox labelled ``"Force"``. While
    checked, the firmware schedules a 1 ms LV timer that invalidates
    the active screen each tick, so LVGL keeps the rendering pipeline
    busy at the display's maximum rate. Pair with :func:`fps` to read
    off the worst-case frame rate for the current layout.

    Has no host-visible Actions — the checkbox's on-change effect is
    handled entirely on-device.
    """
    w = _widget(id, rect=rect, style=style)
    w.force_render.SetInParent()
    return w


# ---------------------------------------------------------------------------
# Screen — the top-level container
# ---------------------------------------------------------------------------


class Layer:
    """One LVGL layer's worth of widgets + layout manager.

    Stage 24.1 — an LVGL display has four stacked screens (bottom, the
    active screen, top, sys; see
    `<https://lvgl.io/docs/open/main-modules/display/screen_layers>`_).
    A :class:`Screen` carries one ``Layer`` per LVGL layer. Most users
    only touch the active layer (via the :class:`Screen` ctor + ``+=``
    sugar); ``Layer`` is exposed for the rare case where a script wants
    to construct a persistent layer (``top`` / ``sys`` / ``bottom``)
    explicitly.

    An empty ``Layer()`` is the explicit "clear this layer" payload: a
    :class:`Screen` carrying ``top=Layer()`` will wipe whatever the
    previous screen put on ``lv_layer_top()``. Omitting the kwarg
    instead leaves the previous content untouched.
    """

    def __init__(
        self,
        layout: _proto.LayoutAbsolute | _proto.LayoutFlex | _proto.LayoutGrid | None = None,
        widgets: Iterable[_proto.Widget] = (),
    ) -> None:
        self.layout: _proto.LayoutAbsolute | _proto.LayoutFlex | _proto.LayoutGrid = (
            layout if layout is not None else absolute()
        )
        self.widgets: list[_proto.Widget] = list(widgets)

    def add(self, widget: _proto.Widget) -> Layer:
        self.widgets.append(widget)
        return self

    def __iadd__(self, widget: _proto.Widget) -> Layer:
        self.widgets.append(widget)
        return self

    def copy_into(self, msg: _proto.Widget) -> None:
        """Populate a proto ``Widget`` as a layout-widget in place.

        Stage 24.2 — Layer is no longer its own proto message; each LVGL
        layer in a ``Screen`` is now a single ``Widget`` whose ``kind``
        is one of ``LayoutAbsolute`` / ``LayoutFlex`` / ``LayoutGrid``.
        This method fills ``msg`` with the appropriate layout-widget
        kind plus a ``Layout.children`` list built from ``self.widgets``.
        """
        if isinstance(self.layout, _proto.LayoutFlex):
            msg.layout_flex.CopyFrom(self.layout)
            del msg.layout_flex.layout.children[:]
            msg.layout_flex.layout.children.extend(self.widgets)
        elif isinstance(self.layout, _proto.LayoutGrid):
            msg.layout_grid.CopyFrom(self.layout)
            del msg.layout_grid.layout.children[:]
            msg.layout_grid.layout.children.extend(self.widgets)
        else:
            msg.layout_absolute.SetInParent()
            del msg.layout_absolute.layout.children[:]
            msg.layout_absolute.layout.children.extend(self.widgets)


class Screen:
    """A single LVGL screen, identified by a unique ``name``.

    Instances are also auto-registered into a module-level list so the
    ``touchy screens push`` CLI can discover every screen a script defines
    without requiring the script to return them explicitly.

    Stage 24.1 — ``Screen`` now exposes four LVGL layers
    (`<https://lvgl.io/docs/open/main-modules/display/screen_layers>`_):

    * ``active`` — the active screen LVGL swaps in via ``lv_screen_load``.
      The ``layout=`` / ``widgets=`` ctor arguments and the ``add`` /
      ``+=`` / ``extend`` helpers all target this layer; authoring code
      that doesn't care about LVGL layers should ignore the others.
    * ``top``, ``sys``, ``bottom`` — LVGL's persistent layers (top, sys,
      bottom). They are NOT cleared when LVGL switches screens, so widgets
      on them stay on screen until a later ``Screen`` explicitly replaces
      that layer. Build them with :func:`add_top` / :func:`add_sys` /
      :func:`add_bottom`. Pass an empty :class:`Layer` (e.g. ``top=Layer()``)
      to clear a layer when switching to this screen; omit the kwarg to
      leave the previous screen's widgets on that layer untouched.
    """

    # Every ``Screen.__init__`` appends to this class-level list so
    # tests can grab the most-recently-constructed instance(s) without
    # plumbing the result through their fixtures. Cleared between
    # tests by the ``_clear_registry`` autouse fixture in
    # ``tests/test_screens.py``.
    _registry: list[Screen] = []

    def __init__(
        self,
        name: str,
        layout: _proto.LayoutAbsolute | _proto.LayoutFlex | _proto.LayoutGrid | None = None,
        widgets: Iterable[_proto.Widget] = (),
        *,
        top: Layer | None = None,
        sys: Layer | None = None,
        bottom: Layer | None = None,
    ) -> None:
        if not name:
            raise ValueError("Screen name must be non-empty")
        self.name = name
        self.active = Layer(layout=layout, widgets=widgets)
        self.top = top
        self.sys = sys
        self.bottom = bottom
        Screen._registry.append(self)

    # -- back-compat passthroughs for the active layer ----------------------

    @property
    def layout(self) -> _proto.LayoutAbsolute | _proto.LayoutFlex | _proto.LayoutGrid:
        return self.active.layout

    @layout.setter
    def layout(self, value: _proto.LayoutAbsolute | _proto.LayoutFlex | _proto.LayoutGrid) -> None:
        self.active.layout = value

    @property
    def widgets(self) -> list[_proto.Widget]:
        return self.active.widgets

    # -- builder-style API --------------------------------------------------

    def add(self, widget: _proto.Widget) -> Screen:
        """Append a widget to the active layer, returning ``self`` for chaining."""
        self.active.widgets.append(widget)
        return self

    def __iadd__(self, widget: _proto.Widget) -> Screen:
        self.active.widgets.append(widget)
        return self

    def extend(self, widgets: Iterable[_proto.Widget]) -> Screen:
        self.active.widgets.extend(widgets)
        return self

    # -- persistent-layer builders -----------------------------------------

    def _ensure_layer(self, attr: str) -> Layer:
        cur = getattr(self, attr)
        if cur is None:
            cur = Layer()
            setattr(self, attr, cur)
        return cur

    def add_top(self, widget: _proto.Widget) -> Screen:
        """Append a widget to the ``top`` LVGL layer (above the active screen)."""
        self._ensure_layer("top").widgets.append(widget)
        return self

    def add_sys(self, widget: _proto.Widget) -> Screen:
        """Append a widget to the ``sys`` LVGL layer (above ``top``)."""
        self._ensure_layer("sys").widgets.append(widget)
        return self

    def add_bottom(self, widget: _proto.Widget) -> Screen:
        """Append a widget to the ``bottom`` LVGL layer (below the active screen)."""
        self._ensure_layer("bottom").widgets.append(widget)
        return self

    # -- serialisation ------------------------------------------------------

    def to_proto(self) -> _proto.Screen:
        msg = _proto.Screen(name=self.name, version=_proto.Screen.Version.CURRENT)
        self.active.copy_into(msg.active)
        if self.top is not None:
            self.top.copy_into(msg.top)
        if self.sys is not None:
            self.sys.copy_into(msg.sys)
        if self.bottom is not None:
            self.bottom.copy_into(msg.bottom)
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
        return f"Screen(name={self.name!r}, widgets={len(self.active.widgets)})"


def build_demo_screens() -> list[Screen]:
    """Build the demo screen set exercising stages 16 / 18 / 20 / 24.

    Returns two ``Screen`` instances meant to be uploaded together (the
    ``touchy screens demo`` CLI subcommand does this). They share a
    common header row of navigation controls so the user can flip
    between them on-device with no host involvement:

      ``[ Prev | FPS | Next ]``

    using Stage-24 ``ActionSwitchScreen``-typed actions on the Prev/Next
    buttons. The :func:`fps` widget in the middle is a live frames-per-
    second readout courtesy of the same stage.

    Screens:

    * ``home`` — full-bleed :func:`trackpad` for USB HID mouse output,
      so the device is immediately useful after the demo upload.
    * ``test`` — the original Stage-16/18/20 widget showcase: hello
      macro button, ping/slider/checkbox emitting host actions, the
      Stage-20 image button, and a :func:`log_line` at the bottom that
      mirrors the device's most recent log message.

    The first screen returned (``home``) becomes the boot default
    because the firmware autoloads the registry's lexicographically
    first entry on first boot; subsequent boots restore whichever
    screen was last viewed (Stage-24 prefs persistence).
    """
    from . import hid_keys as k
    from . import macros as m

    def header(screen: Screen) -> None:
        """Push the shared Prev/Next navigation strip onto the LVGL top
        layer.

        Stage 24.1 — the top layer persists across screen switches so
        the navigation chrome survives without re-transmission. We use
        the same 4×8 grid geometry as the active layer so the header
        cells line up with the rest of the UI, and row 0 of the active
        layer is left empty to make room. Both screens still re-emit
        the header so a cold boot to either one shows the chrome.
        """
        screen.top = Layer(layout=grid(cols=4, rows=8, gap=8))
        screen.add_top(
            cell(button("prev", text="< Prev", on_click=prev_screen_action()), col=0, row=0)
        )
        screen.add_top(
            cell(button("next", text="Next >", on_click=next_screen_action()), col=3, row=0)
        )

    # ── home: trackpad-only ───────────────────────────────────────────
    home = Screen("home", layout=grid(cols=4, rows=8, gap=8))
    header(home)
    home += cell(
        trackpad(
            "pad",
            # dual_tap_window_ms=100,
            # tap_max_ms=100,
            # Cyan / orange / magenta map to 1- / 2- / 3-finger gestures
            # so the user can see which click the firmware is about to
            # synthesise as their fingers land.
            left_touch_color=0x00BFFF,
            right_touch_color=0xFFA500,
            middle_touch_color=0xFF44FF,
            touch_ripple=ripple_animation(
                start_opa=180,
                max_radius=45,
                duration_ms=400,
                path=ANIM_PATH_EASE_OUT,
            ),
            tap_ripple=ripple_animation(
                start_opa=255,
                max_radius=70,
                duration_ms=300,
                path=ANIM_PATH_EASE_OUT,
                border_width=4,
            ),
        ),
        col=0,
        row=1,
        col_span=4,
        row_span=7,
    )

    # ── test: widget showcase + log line ──────────────────────────────
    # Same 3-col grid for header continuity; 8 rows so each widget gets
    # its own track and the log line still has full width at the bottom.
    test = Screen("test", layout=grid(cols=4, rows=8, gap=8))
    header(test)

    # Type-"hi" macro button on the left (mirrors the smiley's transition
    # pattern on a non-image widget so we can tell whether transitions
    # work in general or only on image buttons).
    test += cell(
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
        row=1,
    )
    test += cell(
        button("ping", text="Ping host", on_click=host_action(0x100)),
        col=1,
        row=1,
    )
    # Dev-only force-render checkbox, kept next to the FPS readout in
    # the header so a benchmarker can watch the number while toggling.
    test += cell(
        force_render("force"),
        col=2,
        row=1,
    )
    test += cell(fps("fps"), col=3, row=1)
    test += cell(
        slider("level", min=0, max=100, value=42, on_change=host_action(0x101)),
        col=0,
        row=2,
        col_span=3,
    )
    test += cell(
        checkbox("enable", text="Enabled", checked=True, on_change=host_action(0x102)),
        col=0,
        row=3,
    )

    # Stage-20.2 smiley image-button with cranked-up press feedback.
    test += cell(
        image_button(
            "smile",
            asset="F:host/images/smiley.png",
            on_click=host_action(0x103),
            scale=2.0,
            pressed_scale=2.5,
            style=[
                style(
                    transition=transition(
                        props=[PROP_TRANSFORM_WIDTH, PROP_IMAGE_RECOLOR_OPA, PROP_BG_COLOR],
                        path=ANIM_PATH_LINEAR,
                        duration_ms=300,
                    )
                ),
                style(
                    transform_width=80,
                    recolor=0xFF0000,
                    recolor_opa=255,
                    bg_color=0x00FF00,
                    for_state=STATE_PRESSED,
                ),
            ],
        ),
        col=1,
        row=3,
        col_span=2,
    )

    # Log strip spans the bottom 4 rows so multi-line wrapped messages
    # stay readable.
    test += cell(log_line("log"), col=0, row=4, col_span=4, row_span=4)

    return [home, test]


def build_demo_screen(name: str = "demo") -> Screen:
    """Back-compat shim returning the trackpad-bearing demo screen.

    Pre-Stage-24 callers expected one screen; the demo is now split
    into two. We return the ``home`` screen because it carries the
    multitouch trackpad ("pad") that the previous demo led with, and
    that's what existing tests / docs key off.
    """
    screens = build_demo_screens()
    target = next(s for s in screens if s.name == "home")
    target.name = name
    return target
