"""Stage lb12 — value wrapper types for :meth:`TouchyClient.set_property`.

``set_property`` maps a Python value to the right ``SetPropertyCmd`` oneof
arm. ``bool`` / ``int`` / ``str`` are detected natively, but a colour and a
point would otherwise be indistinguishable from a plain ``int`` / tuple, so
callers wrap those in the explicit types here:

* :class:`Color` — a ``0xRRGGBB`` colour. Subclasses ``int`` so it behaves
  like one everywhere, but ``isinstance(v, Color)`` lets the mapper pick the
  ``color_value`` arm instead of ``int_value``.
* :class:`Point` — an ``(x, y)`` integer pair mapped to ``point_value``.
"""

from __future__ import annotations

from dataclasses import dataclass


class Color(int):
    """A ``0xRRGGBB`` colour value for :meth:`TouchyClient.set_property`.

    Subclasses :class:`int` so arithmetic / formatting still work, while
    remaining distinguishable from a plain ``int`` (which maps to the
    integer property arm).
    """

    __slots__ = ()

    def __repr__(self) -> str:  # pragma: no cover - cosmetic
        return f"Color(0x{int(self):06X})"


@dataclass(frozen=True)
class Point:
    """An integer ``(x, y)`` point value for :meth:`TouchyClient.set_property`."""

    x: int
    y: int
