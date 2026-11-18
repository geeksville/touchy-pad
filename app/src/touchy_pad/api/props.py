"""Value wrapper types + the batch-entry builder for the runtime property
overrides.

``build_property_override`` maps a Python value to the right
``SetPropertyCmd`` oneof arm. ``bool`` / ``int`` / ``str`` are detected
natively, but a colour and a point would otherwise be indistinguishable
from a plain ``int`` / tuple, so callers wrap those in the explicit
types here:

* :class:`Color` — a ``0xRRGGBB`` colour. Subclasses ``int`` so it behaves
  like one everywhere, but ``isinstance(v, Color)`` lets the mapper pick the
  ``color_value`` arm instead of ``int_value``.
* :class:`Point` — an ``(x, y)`` integer pair mapped to ``point_value``.

One property needs no wrapper because its *name* settles the type: an ``int``
for ``"text_font"`` is a **pixel font size** (stage lb26) and is mapped to its
own ``font_size_value`` arm, validated against the sizes the firmware compiled
(``FontSize`` / ``FONT_SIZES``). That is the live font-size knob::

    pad.set_properties([build_property_override("title", "text_font", 30)])

Build a batch of these and send them all in one RPC via
:meth:`TouchyClient.set_properties` (stage lb14: the batched
``SetPropertiesCmd`` *replaced* the former singular ``set_property`` —
no wire backwards compatibility is kept during early development).
"""

from __future__ import annotations

from dataclasses import dataclass

from .. import _proto

# Stage lb26 — the LVGL property whose value is a *font*. The wire cannot carry
# a `const lv_font_t *`, so an integer value for this property is routed to
# `SetPropertyCmd.font_size_value` (a pixel size the device maps with
# `font_for_size()`) instead of `int_value` — which LVGL would reinterpret as a
# font pointer.
FONT_SIZE_PROP = "text_font"

# The pixel sizes the firmware compiled a font for (proto `FontSize`, the same
# list `touchy_pad.api.FONT_SIZES` publishes). Derived from the enum so this
# module needs no import of `screens` and the two can never drift.
_FONT_SIZES: frozenset[int] = frozenset(v for v in _proto.FontSize.values() if v)


def _font_size_override(size: int) -> int:
    """Validate a ``text_font`` size against the compiled fonts (stage lb26)."""
    px = int(size)
    if px and px not in _FONT_SIZES:
        choices = ", ".join(str(s) for s in sorted(_FONT_SIZES))
        raise ValueError(
            f"build_property_override(): {FONT_SIZE_PROP} must be 0 (theme "
            f"default) or one of {choices} (FontSize.* / FONT_SIZES), got {px} — "
            "the firmware has no font of that size"
        )
    return px


class Color(int):
    """A ``0xRRGGBB`` colour value for :func:`build_property_override`.

    Subclasses :class:`int` so arithmetic / formatting still work, while
    remaining distinguishable from a plain ``int`` (which maps to the
    integer property arm).
    """

    __slots__ = ()

    def __repr__(self) -> str:  # pragma: no cover - cosmetic
        return f"Color(0x{int(self):06X})"


@dataclass(frozen=True)
class Point:
    """An integer ``(x, y)`` point value for :func:`build_property_override`."""

    x: int
    y: int


def build_property_override(
    widget_id: str,
    prop: str | int,
    value: bool | int | str | Color | Point | None,
) -> _proto.SetPropertyCmd:
    """Build one ``SetPropertyCmd`` entry for :meth:`TouchyClient.set_properties`.

    *widget_id* is the target ``Widget.id``. *prop* is the property
    identifier — a ``str`` name (resolved on-device via
    ``lv_obj_property_get_id``, e.g. ``"text"``) or an ``int`` raw
    ``lv_prop_id_t``. *value* selects the payload:

    * ``bool`` → boolean property,
    * ``int`` → integer property,
    * ``str`` → text property (e.g. a label's text),
    * :class:`Color` → ``0xRRGGBB`` colour property,
    * :class:`Point` → ``(x, y)`` point property,
    * ``None`` → **remove** any existing override for this
      ``(widget_id, prop)``.

    The one exception to the ``int`` rule is the property
    ``"text_font"``: an ``int`` there is a **pixel font size** (stage lb26),
    sent as its own wire arm and validated against the sizes the firmware
    compiled (``FontSize`` / ``FONT_SIZES``)::

        pad.set_properties([build_property_override("title", "text_font",
                                                     FontSize.FONT_SIZE_30)])

    ``0`` there means "theme default" (it drops the local font again), and
    ``None`` removes the override as usual. Any *other* value type for
    ``"text_font"`` raises :exc:`TypeError`: LVGL's ``text_font`` value is a
    font pointer, so a colour / string / bool would be read as one. For every
    other property the value type is mapped as listed above.

    The resulting command is session-scoped (RAM only) and sticky: the
    device re-applies it whenever the widget is (re)built and immediately
    if it is already on screen. Targeting a not-yet-loaded widget is fine.
    """
    cmd = _proto.SetPropertyCmd(widget_id=widget_id)
    if isinstance(prop, str):
        cmd.property_name = prop
    elif isinstance(prop, bool):
        raise TypeError("prop must be a property name (str) or id (int), not bool")
    elif isinstance(prop, int):
        cmd.property_id = prop
    else:
        raise TypeError(f"prop must be str (name) or int (id), got {type(prop).__name__}")

    # value → oneof arm. Order matters: bool and Color are int subclasses.
    if value is None:
        pass  # unset value oneof ⇒ remove the override
    elif prop == FONT_SIZE_PROP:
        # Stage lb26 — a pixel size for the text font, and its own arm: LVGL's
        # `text_font` value is a font pointer the wire cannot carry (any other
        # arm would be read as that pointer). Nothing else is meaningful here,
        # so reject it rather than sending a value the device must refuse.
        # NB: `bool` and `Color` are `int` subclasses and `Point` is a plain
        # dataclass — none of them is a pixel size.
        if isinstance(value, bool | Color | Point) or not isinstance(value, int):
            raise TypeError(
                f"build_property_override(): {FONT_SIZE_PROP} takes an int pixel "
                f"size (FontSize.* / FONT_SIZES), or None to remove the override; "
                f"got {type(value).__name__}"
            )
        cmd.font_size_value = _font_size_override(value)
    elif isinstance(value, bool):
        cmd.bool_value = value
    elif isinstance(value, Color):
        cmd.color_value = int(value) & 0xFFFFFF
    elif isinstance(value, Point):
        cmd.point_value.x = value.x
        cmd.point_value.y = value.y
    elif isinstance(value, int):
        cmd.int_value = value
    elif isinstance(value, str):
        cmd.string_value = value
    else:
        raise TypeError(
            "value must be bool / int / str / Color / Point / None, " f"got {type(value).__name__}"
        )
    return cmd
