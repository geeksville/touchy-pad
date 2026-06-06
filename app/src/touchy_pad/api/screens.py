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

import logging
from collections.abc import Iterable
from pathlib import Path

from .. import _proto
from . import _events

logger = logging.getLogger(__name__)

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
    "grow",
    "style",
    "transition",
    "anim_track",
    "animation",
    "action",
    "host_action",
    "macro_action",
    "device_action",
    "change_widget_ref_action",
    "next_widget_action",
    "prev_widget_action",
    "button",
    "label",
    "slider",
    "toggle",
    "checkbox",
    "image",
    "image_button",
    "arc",
    "spacer",
    "widget_ref",
    "trackpad",
    "ripple_animation",
    "log_line",
    "fps",
    "force_render",
    "build_demo_screen",
    "build_demo",
    "build_default_screen",
    "build_setup_screen",
    "build_user_pages",
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
    # Stage 59 — geometry / opacity props for declarative `animation(...)`.
    "PROP_X",
    "PROP_Y",
    "PROP_WIDTH",
    "PROP_HEIGHT",
    "PROP_OPA",
    # AnimPath easing curves for use with `transition(path=...)`.
    "ANIM_PATH_LINEAR",
    "ANIM_PATH_EASE_IN",
    "ANIM_PATH_EASE_OUT",
    "ANIM_PATH_EASE_IN_OUT",
    "ANIM_PATH_OVERSHOOT",
    "ANIM_PATH_BOUNCE",
    "ANIM_PATH_STEP",
    # Label horizontal text alignment.
    "TEXT_ALIGN_AUTO",
    "TEXT_ALIGN_LEFT",
    "TEXT_ALIGN_CENTER",
    "TEXT_ALIGN_RIGHT",
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
# Stage 59 — geometry / opacity props for `animation(...)`.
PROP_X = _proto.StyleProp.STYLE_PROP_X
PROP_Y = _proto.StyleProp.STYLE_PROP_Y
PROP_WIDTH = _proto.StyleProp.STYLE_PROP_WIDTH
PROP_HEIGHT = _proto.StyleProp.STYLE_PROP_HEIGHT
PROP_OPA = _proto.StyleProp.STYLE_PROP_OPA

ANIM_PATH_LINEAR = _proto.AnimPath.ANIM_PATH_LINEAR
ANIM_PATH_EASE_IN = _proto.AnimPath.ANIM_PATH_EASE_IN
ANIM_PATH_EASE_OUT = _proto.AnimPath.ANIM_PATH_EASE_OUT
ANIM_PATH_EASE_IN_OUT = _proto.AnimPath.ANIM_PATH_EASE_IN_OUT
ANIM_PATH_OVERSHOOT = _proto.AnimPath.ANIM_PATH_OVERSHOOT
ANIM_PATH_BOUNCE = _proto.AnimPath.ANIM_PATH_BOUNCE
ANIM_PATH_STEP = _proto.AnimPath.ANIM_PATH_STEP

# Horizontal text alignment for `label(..., text_align=...)` (see
# widgets.proto:TextAlign). Maps to LVGL `lv_text_align_t`.
TEXT_ALIGN_AUTO = _proto.TextAlign.TEXT_ALIGN_AUTO
TEXT_ALIGN_LEFT = _proto.TextAlign.TEXT_ALIGN_LEFT
TEXT_ALIGN_CENTER = _proto.TextAlign.TEXT_ALIGN_CENTER
TEXT_ALIGN_RIGHT = _proto.TextAlign.TEXT_ALIGN_RIGHT


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
    *,
    grow_x: int = 0,
    grow_y: int = 0,
) -> _proto.Widget:
    """Place ``widget`` inside a grid-layout parent and return it.

    Only meaningful when the enclosing :class:`Screen` was created
    with a :func:`grid` layout. Span defaults to 1x1.

    By default the widget is content-sized and centred in its cell. Pass
    ``grow_x`` / ``grow_y`` (any value ``> 0``) to stretch it to fill the
    cell on that axis — a convenience wrapper around :func:`grow`.
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
    if grow_x:
        widget.grow_x = int(grow_x)
    if grow_y:
        widget.grow_y = int(grow_y)
    return widget


def rect(x: int = 0, y: int = 0, w: int = 0, h: int = 0) -> _proto.Rect:
    """Pixel-space placement. ``0`` for w/h means "size to content"."""
    return _proto.Rect(x=x, y=y, w=w, h=h)


def grow(widget: _proto.Widget, *, x: int = 0, y: int = 0) -> _proto.Widget:
    """Opt ``widget`` into "grow to fill" on either axis; returns it.

    By default every widget is content-sized within its layout slot — a
    button shrinks to its label rather than stretching to fill the row.
    Set ``x`` / ``y`` to grow the widget along that axis:

    * **Flex main axis** (``x`` under a :func:`row`, ``y`` under a
      :func:`col`): the value is an LVGL ``flex_grow`` *factor* — children
      share the parent's free main-axis space in proportion to their
      factors (so ``2`` grows twice as fast as ``1``).
    * **Flex cross axis** (``y`` under a :func:`row`, ``x`` under a
      :func:`col`) and **grid** (both axes): there is no proportional
      grow, so any value ``> 0`` simply means "fill" (full width/height
      under flex, ``STRETCH`` under grid). The magnitude is ignored.

    ``bool`` is accepted too (``grow(w, x=True)`` ≡ ``grow(w, x=1)``).
    """
    widget.grow_x = int(x)
    widget.grow_y = int(y)
    return widget


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


def host_action(
    code: int | None = None,
    *,
    on_event: _events.HostEventCallback | None = None,
) -> _proto.Action:
    """Forward the widget event to the host.

    Two styles are supported:

    * **Callback-first (recommended).** Pass ``on_event=`` and let the
      library allocate a unique ``host_code`` for you::

          button("ping", on_click=host_action(on_event=lambda e: print(e)))

      The callback is registered automatically when the screen / widget
      containing this action is uploaded via ``pad.screen_save(...)`` /
      ``pad.widget_save(...)``; no separate ``pad.on_host_event`` call is
      needed. Auto-allocated codes start at
      :data:`touchy_pad.api._events.AUTO_CODE_BASE` (``0x10000``).

    * **Explicit code (low level).** Pass a numeric ``code`` (keep it
      **below** ``0x10000`` to avoid the auto range) and dispatch it
      yourself with ``pad.on_host_event(code, callback)``. An ``on_event=``
      callback may still be supplied to bind it to that explicit code.

    The host-side :class:`touchy_pad.TouchyClient` dispatches incoming
    ``LvEvent``\\s on ``host_code``; the callback receives the full
    :class:`_proto.LvEvent` (inspect ``user_data``, ``value``,
    ``checked``, ...).
    """
    if code is None:
        code = _events.alloc_code(on_event)
    else:
        if code < 0 or code > 0xFFFF_FFFF:
            raise ValueError("host action code must fit in a uint32")
        if on_event is not None:
            _events.register_binding(code, on_event)
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
# device actions ask the firmware to change its own state — currently
# rebinding a ``WidgetRef`` to a different widget file, via Stage 57's
# ``ActionChangeWidgetRef``. They run entirely on-device with no host
# round-trip, so paged UIs keep responding when the host computer is
# asleep or unplugged.


def device_action(device: _proto.ActionDevice) -> _proto.Action:
    """Wrap a pre-built :class:`_proto.ActionDevice` as an :class:`Action`.

    Most callers want the higher-level :func:`change_widget_ref_action` /
    :func:`next_widget_action` / :func:`prev_widget_action` helpers
    instead; this exists for forward-compatibility with new
    ``ActionDevice`` sub-kinds the firmware may add later.
    """
    return _proto.Action(device=device)


def change_widget_ref_action(
    target_id: str,
    path: str,
    *,
    behavior: int | None = None,
) -> _proto.Action:
    """Build an Action that rebinds an on-screen ``WidgetRef`` (Stage 57).

    Parameters
    ----------
    target_id:
        ``Widget.id`` of the *outer* widget whose ``kind`` is
        ``widget_ref``. The firmware finds the matching ref in the
        currently-loaded screen and rebinds it. Required.
    path:
        For ``BY_PATH`` (default): full drive-prefixed path to a widget
        ``.pb`` file (e.g. ``"F:host/w/trackpad.pb"``).
        For ``NEXT`` / ``PREVIOUS``: drive-prefixed *directory* (e.g.
        ``"F:host/w/"``) — the firmware enumerates ``*.pb`` files and
        steps ±1 from the ref's current path.
    behavior:
        One of :attr:`ActionChangeWidgetRef.BY_PATH` (0),
        :attr:`ActionChangeWidgetRef.NEXT` (1) or
        :attr:`ActionChangeWidgetRef.PREVIOUS` (2).

    Changes are RAM-only; reloading the screen reverts to the originally
    encoded path. See also :func:`next_widget_action` /
    :func:`prev_widget_action`.
    """
    Behavior = _proto.ActionChangeWidgetRef.Behavior  # noqa: N806
    if behavior is None:
        behavior = Behavior.BY_PATH
    if not target_id:
        raise ValueError("change_widget_ref_action requires target_id=")
    if not path:
        raise ValueError("change_widget_ref_action requires path=")
    msg = _proto.ActionChangeWidgetRef(behavior=behavior, path=path, target_id=target_id)
    return device_action(_proto.ActionDevice(change_widget_ref=msg))


def next_widget_action(target_id: str, directory: str) -> _proto.Action:
    """Step a ``WidgetRef`` to the next file in *directory* (wraps around)."""
    return change_widget_ref_action(
        target_id, directory, behavior=_proto.ActionChangeWidgetRef.NEXT
    )


def prev_widget_action(target_id: str, directory: str) -> _proto.Action:
    """Step a ``WidgetRef`` to the previous file in *directory* (wraps around)."""
    return change_widget_ref_action(
        target_id, directory, behavior=_proto.ActionChangeWidgetRef.PREVIOUS
    )


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


def _normalise_animations(
    animations: _proto.Animation | Iterable[_proto.Animation] | None,
) -> list[_proto.Animation]:
    """Coerce a widget factory's ``animations=`` argument to a flat list.

    Stage 59 — accepts ``None``, a single :class:`_proto.Animation`, or
    any iterable of them. The result is what callers pass to
    ``Widget.animations.extend``.
    """
    if animations is None:
        return []
    if isinstance(animations, _proto.Animation):
        return [animations]
    return list(animations)


def anim_track(
    prop: int,
    start: int,
    end: int,
) -> _proto.AnimTrack:
    """One animated style property inside an :func:`animation`.

    ``prop`` is a ``PROP_*`` constant (e.g. :data:`PROP_X`,
    :data:`PROP_OPA`). ``start`` / ``end`` are the initial and final
    integer values — pixels for X/Y/WIDTH/HEIGHT, 0–255 for OPA,
    RGB888 / RGB565 ints for colour props.
    """
    return _proto.AnimTrack(prop=prop, start=start, end=end)


def animation(
    *tracks: _proto.AnimTrack,
    duration_ms: int = 1000,
    path: int = None,  # type: ignore[assignment]   # default filled below
    repeat_count: int = 0,
    repeat_delay_ms: int = 0,
    reverse: bool = False,
    reverse_delay_ms: int = 0,
    reverse_duration_ms: int = 0,
    start_delay_ms: int = 0,
) -> _proto.Animation:
    """Build an :class:`_proto.Animation` describing N parallel tracks.

    Stage 59. Mirrors LVGL's `lv_anim_t` knobs:

    * ``duration_ms`` — forward leg duration.
    * ``path`` — easing curve (an ``ANIM_PATH_*`` constant); defaults
      to :data:`ANIM_PATH_LINEAR`.
    * ``repeat_count`` — ``0`` means infinite (mapped to
      ``LV_ANIM_REPEAT_INFINITE`` on the device), otherwise the total
      number of forward (or forward+reverse) cycles.
    * ``repeat_delay_ms`` — pause between cycles.
    * ``reverse`` — when ``True``, animate back to ``start`` after the
      forward leg (ping-pong). ``reverse_delay_ms`` / ``reverse_duration_ms``
      configure the reverse leg; a zero ``reverse_duration_ms`` reuses
      ``duration_ms``.
    * ``start_delay_ms`` — one-shot pre-roll before the first cycle.
    """
    if path is None:
        path = ANIM_PATH_LINEAR
    a = _proto.Animation(
        duration_ms=duration_ms,
        path=path,
        repeat_count=repeat_count,
        repeat_delay_ms=repeat_delay_ms,
        reverse=reverse,
        reverse_delay_ms=reverse_delay_ms,
        reverse_duration_ms=reverse_duration_ms,
        start_delay_ms=start_delay_ms,
    )
    a.tracks.extend(tracks)
    return a


def _widget(
    id: str,
    *,
    rect: _proto.Rect | None,
    style: _proto.Style | Iterable[_proto.Style] | None,
    animations: _proto.Animation | Iterable[_proto.Animation] | None = None,
) -> _proto.Widget:
    w = _proto.Widget(id=id)
    if rect is not None:
        w.rect.CopyFrom(rect)
    w.styles.extend(_normalise_styles(style))
    w.animations.extend(_normalise_animations(animations))
    return w


def button(
    id: str,
    text: str = "",
    on_click=None,
    on_press=None,
    on_release=None,
    rect: _proto.Rect | None = None,
    style: _proto.Style | Iterable[_proto.Style] | None = None,
    animations: _proto.Animation | Iterable[_proto.Animation] | None = None,
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
    w = _widget(id, rect=rect, style=style, animations=animations)
    w.button.text = text
    w.button.on_click.extend(_normalise_actions(on_click))
    w.button.on_press.extend(_normalise_actions(on_press))
    w.button.on_release.extend(_normalise_actions(on_release))
    return w


def label(
    id: str,
    text: str = "",
    font_size: int = 0,
    text_align: int = TEXT_ALIGN_AUTO,
    rect: _proto.Rect | None = None,
    style: _proto.Style | Iterable[_proto.Style] | None = None,
    animations: _proto.Animation | Iterable[_proto.Animation] | None = None,
) -> _proto.Widget:
    """Static text. ``font_size = 0`` uses the theme default.

    ``text_align`` controls horizontal alignment of the text *within the
    label's box* (``TEXT_ALIGN_*``; default ``AUTO`` follows LVGL). To
    actually centre across the screen, also give the label full width
    (e.g. ``grow(label(...), x=1)`` under a column).
    """
    w = _widget(id, rect=rect, style=style, animations=animations)
    w.label.text = text
    w.label.font_size = font_size
    w.label.text_align = text_align
    return w


def slider(
    id: str,
    min: int = 0,
    max: int = 100,
    value: int = 0,
    on_change=None,
    rect: _proto.Rect | None = None,
    style: _proto.Style | Iterable[_proto.Style] | None = None,
    animations: _proto.Animation | Iterable[_proto.Animation] | None = None,
) -> _proto.Widget:
    """A linear slider over ``[min, max]``."""
    w = _widget(id, rect=rect, style=style, animations=animations)
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
    animations: _proto.Animation | Iterable[_proto.Animation] | None = None,
) -> _proto.Widget:
    """An on/off switch."""
    w = _widget(id, rect=rect, style=style, animations=animations)
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
    animations: _proto.Animation | Iterable[_proto.Animation] | None = None,
) -> _proto.Widget:
    """A labelled tickbox."""
    w = _widget(id, rect=rect, style=style, animations=animations)
    w.checkbox.text = text
    w.checkbox.checked = checked
    w.checkbox.on_change.extend(_normalise_actions(on_change))
    return w


def image(
    id: str,
    asset: str,
    rect: _proto.Rect | None = None,
    style: _proto.Style | Iterable[_proto.Style] | None = None,
    animations: _proto.Animation | Iterable[_proto.Animation] | None = None,
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
    w = _widget(id, rect=rect, style=style, animations=animations)
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
    animations: _proto.Animation | Iterable[_proto.Animation] | None = None,
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
    w = _widget(id, rect=rect, style=style, animations=animations)
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

    msg.path = rewrite_to_bin_path(asset)
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
    animations: _proto.Animation | Iterable[_proto.Animation] | None = None,
) -> _proto.Widget:
    """A circular arc indicator."""
    w = _widget(id, rect=rect, style=style, animations=animations)
    w.arc.min = min
    w.arc.max = max
    w.arc.value = value
    return w


def spacer(
    id: str = "",
    rect: _proto.Rect | None = None,
    style: _proto.Style | Iterable[_proto.Style] | None = None,
    animations: _proto.Animation | Iterable[_proto.Animation] | None = None,
) -> _proto.Widget:
    """Invisible placeholder. Useful for padding inside flex/grid layouts."""
    w = _widget(id, rect=rect, style=style, animations=animations)
    w.spacer.SetInParent()
    return w


def widget_ref(
    path: str,
    *,
    id: str = "",
    rect: _proto.Rect | None = None,
    cell: _proto.GridCell | None = None,
    style: _proto.Style | Iterable[_proto.Style] | None = None,
) -> _proto.Widget:
    """Stage 54 — embed a `Widget` stored in a separate ``.pb`` file.

    *path* is the drive-prefixed device path to a serialized
    :class:`touchy.Widget` (e.g. ``"R:host/w/key0.pb"``). At
    screen-build time the firmware reads, decodes, and splices the
    referenced widget into the parent layout in place of this ref —
    semantically equivalent to inlining the widget here.

    *path* is the only payload of the ref itself; *rect* / *cell* /
    *style* are intentionally **not** serialized — those belong to the
    referenced widget. They are rejected with ``ValueError`` if passed.

    Stage 57 — *id* **is** accepted and stamped on the outer ref widget.
    Set it when you want :func:`change_widget_ref_action` to address
    this ref at runtime (firmware looks up refs by their outer
    ``Widget.id``). Defaults to the empty string when omitted.
    """
    if not path:
        raise ValueError("widget_ref requires a non-empty path")
    if rect is not None or cell is not None or style is not None:
        raise ValueError(
            "widget_ref does not accept rect/cell/style — those belong "
            "to the referenced widget itself"
        )
    w = _proto.Widget()
    if id:
        w.id = id
    w.widget_ref.path = path
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
    animations: _proto.Animation | Iterable[_proto.Animation] | None = None,
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
    w = _widget(id, rect=rect, style=style, animations=animations)
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
    animations: _proto.Animation | Iterable[_proto.Animation] | None = None,
) -> _proto.Widget:
    """One-line readout of the most recent device log message.

    Subscribes to the firmware's shared log sink (see
    ``firmware/main/log_line.{h,cpp}``); the :func:`trackpad` widget
    and other subsystems push status lines through that sink.
    """
    w = _widget(id, rect=rect, style=style, animations=animations)
    w.log.SetInParent()
    return w


def fps(
    id: str = "fps",
    rect: _proto.Rect | None = None,
    style: _proto.Style | Iterable[_proto.Style] | None = None,
    animations: _proto.Animation | Iterable[_proto.Animation] | None = None,
) -> _proto.Widget:
    """Live frames-per-second readout for the active LVGL display.

    Renders as a small label that the firmware refreshes ~twice per
    second from a counter incremented on every display flush. Useful
    while iterating on layouts to spot the cost of expensive widgets
    (image scaling, transitions, ...).
    """
    w = _widget(id, rect=rect, style=style, animations=animations)
    w.fps.SetInParent()
    return w


def force_render(
    id: str = "force_render",
    rect: _proto.Rect | None = None,
    style: _proto.Style | Iterable[_proto.Style] | None = None,
    animations: _proto.Animation | Iterable[_proto.Animation] | None = None,
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
    w = _widget(id, rect=rect, style=style, animations=animations)
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
        # Stage 56: the wire-format version lives on the root `Widget`
        # of each file. For screen files that's `active.version`.
        msg = _proto.Screen()
        self.active.copy_into(msg.active)
        msg.active.version = _proto.Widget.Version.CURRENT
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


def build_default_screen() -> Screen:
    """Build the Stage-68 default *chrome* screen (``F:host/s/default.pb``).

    A vertical flex column with two children:

    * a content-sized top chrome row holding ``< Prev`` / ``Next >``
      buttons (sized to their content — the column flow shrinks the row
      to just the button heights). The row opts into full width via
      ``grow(..., x=1)`` so the prev/next buttons sit at the screen edges.
    * a ``widget_ref(id="page")`` **body** that grows to fill the rest of
      the screen — ``grow(..., x=1, y=1)``: ``y=1`` is the column main-axis
      flex-grow (fills remaining height), ``x=1`` fills the cross-axis
      width. It initially points at :data:`USER_SCREENS_DIR`
      ``trackpad.pb``; the prev/next buttons step the ref through
      :data:`USER_SCREENS_DIR` on-device.

    Users normally leave this screen alone and just push page bodies into
    ``host/uscr/`` (see :func:`build_user_pages`); replacing
    ``host/s/default.pb`` is the escape hatch for a fully custom layout.
    """
    from ..paths import USER_SCREENS_DIR, user_screen_path

    PAGE_ID = "page"

    screen = Screen("default", layout=col(gap=8))

    # Content-sized chrome row (a nested flex row → no main-axis grow, so
    # it shrinks to the buttons' height inside the column flow), opted into
    # full width with grow(x=1). A flex-grow spacer between the buttons
    # pushes `< Prev` to the left edge and `Next >` to the right edge.
    chrome = Layer(layout=row(gap=8))
    chrome += button(
        "prev",
        text="< Prev",
        on_click=prev_widget_action(PAGE_ID, USER_SCREENS_DIR),
    )
    chrome += grow(spacer("chrome_gap"), x=1)
    chrome += button(
        "next",
        text="Next >",
        on_click=next_widget_action(PAGE_ID, USER_SCREENS_DIR),
    )
    chrome_widget = _proto.Widget()
    chrome.copy_into(chrome_widget)
    grow(chrome_widget, x=1)  # span the full screen width
    screen += chrome_widget

    # Body — fills the remaining height (y=1 = column main-axis flex-grow)
    # and the full width (x=1 = cross-axis fill) below the chrome row.
    body = widget_ref(user_screen_path("trackpad"), id=PAGE_ID)
    grow(body, x=1, y=1)
    screen += body

    return screen


def build_setup_screen() -> Screen:
    """Build the compiled-in firmware fallback screen.

    This is the screen a *virgin* device shows when no host screens have
    been provisioned yet (``proto/default_screen.json`` →
    ``firmware/main/default_screen_pb.h``). Unlike
    :func:`build_default_screen` it is fully self-contained — no
    ``widget_ref`` into ``F:host/uscr/`` (which doesn't exist yet) and no
    prev/next chrome (there are no other pages to step to). It is a
    vertical flex column with:

    * a content-sized hint label telling the user to run ``touchy init``;
    * an inlined :func:`trackpad` that grows to fill the rest of the
      screen, so the device is immediately usable as a touchpad.
    """
    screen = Screen("default", layout=col(gap=8))

    hint = label(
        "setup_hint",
        text="Run 'touchy init' to set up",
        text_align=TEXT_ALIGN_CENTER,
        style=style(text_color=0xFFFFFF),
    )
    grow(hint, x=1)  # full width so the centred text spans the screen
    screen += hint

    from touchy_pad.pages import trackpad as _trackpad_page

    _, pad = _trackpad_page.build()
    grow(pad, x=1, y=1)  # fill the remaining height + full width
    screen += pad

    return screen


def build_user_pages() -> list[tuple[str, _proto.Widget]]:
    """Build the demo user-screen page bodies for ``F:host/uscr/``.

    Returns a list of ``(name, Widget)`` pairs the caller uploads via
    :meth:`Touchy.user_screen_save` (CLI: ``touchy screen init`` /
    ``touchy screen demo``). Lexicographic order in that directory is
    ``test`` then ``trackpad``, so the chrome's Next/Prev wrap predictably.

    * ``"trackpad"`` — full-bleed multitouch trackpad (USB HID mouse).
      Written by ``screen init`` so the device has a usable page out of
      the box.  See :mod:`touchy_pad.api.pages.trackpad`.
    * ``"test"`` — Stage 16 / 18 / 20 widget showcase (hello / ping /
      force / fps / slider / checkbox / smiley image-button / log line),
      with Stage-67 inline ``host_action(on_event=...)`` callbacks. Added
      by ``screen demo`` on top of the baseline.  See
      :mod:`touchy_pad.api.pages.test`.
    """
    from touchy_pad.pages import test as _test_page
    from touchy_pad.pages import trackpad as _trackpad_page

    return [_test_page.build(), _trackpad_page.build()]


def build_demo() -> tuple[Screen, list[tuple[str, _proto.Widget]]]:
    """Back-compat shim: the Stage-68 default chrome + user page bodies.

    Returns ``(build_default_screen(), build_user_pages())``. Prefer the
    two builders directly — the chrome now lives in
    ``F:host/s/default.pb`` and the pages in ``F:host/uscr/`` (see
    :func:`build_default_screen` / :func:`build_user_pages`).
    """
    return build_default_screen(), build_user_pages()


def build_demo_screen(name: str = "default") -> Screen:
    """Back-compat shim returning the Stage-68 default chrome screen."""
    screen = build_default_screen()
    screen.name = name
    return screen
