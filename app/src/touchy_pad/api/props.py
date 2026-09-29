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

Build a batch of these and send them all in one RPC via
:meth:`TouchyClient.set_properties` (stage lb14: the batched
``SetPropertiesCmd`` *replaced* the former singular ``set_property`` —
no wire backwards compatibility is kept during early development).
"""

from __future__ import annotations

from dataclasses import dataclass

from .. import _proto


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
