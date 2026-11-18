"""The ``#RRGGBB text#`` markup parser, shared by the DSL and the simulator.

Stage lb24. The *authoritative* implementation lives in the firmware
(``firmware/main/widgets/widget_span.cpp``): the device parses the markup into
LVGL spans, both when it builds a ``label(recolor=True)`` and when a runtime
``text`` property override arrives. This module is the Python mirror of that
parser, used for two things:

* turning a markup string into explicit ``touchy.Span`` messages up front
  (:func:`parse_recolor_markup`), for hosts that prefer to author spans
  themselves; and
* rendering a recoloured label in the simulator (``touchy_pad.sim.widgets``),
  which shows the same colour runs the panel will.

It deliberately lives outside ``touchy_pad.api`` so the simulator can import it
without pulling in (or cycling through) the public API package.

Grammar (LVGL's own label-recolour semantics, minus its state-carrying defect):

* a run opens at ``#`` + exactly six hex digits + a space — the space is
  consumed, so ``"#FF0000 one# two"`` renders ``one`` red and ``" two"`` in the
  label's base colour;
* a run closes at the next ``#``;
* an unmatched ``#`` is literal text;
* an empty run (``"#FF0000 #"``) contributes nothing;
* an unterminated run keeps its colour to the end of the string.

Note that ``#`` is always a command delimiter — LVGL's ``##`` escape is dead code
in 9.5, so a literal ``#`` is swallowed and hosts strip it themselves.
"""

from __future__ import annotations

from . import _proto

__all__ = ["parse_recolor_markup"]

_HEX_DIGITS = frozenset("0123456789abcdefABCDEF")


def _is_opener(text: str, i: int) -> bool:
    """True when a run opens at ``text[i]`` (six hex digits + a space)."""
    if i + 7 > len(text):
        return False
    for k in range(6):
        if text[i + k] not in _HEX_DIGITS:
            return False
    return text[i + 6] == " "


def parse_recolor_markup(text: str) -> list[_proto.Span]:
    """Split ``#RRGGBB text#`` markup into one :class:`touchy.Span` per run.

    Returns spans in draw order. A span's ``text_color`` is left *unset* for
    text outside a run (so it inherits the group's own text colour) and set to
    the packed ``0xRRGGBB`` value for text inside one. Empty runs are dropped.
    """
    spans: list[_proto.Span] = []

    def flush(begin: int, end: int, color: int | None) -> None:
        if end <= begin:
            return
        span = _proto.Span(text=text[begin:end])
        if color is not None:
            span.text_color = color
        spans.append(span)

    seg = 0  # start of the segment being accumulated
    color: int | None = None  # None = outside a run
    i = 0
    n = len(text)
    while i < n:
        if text[i] != "#":
            i += 1
            continue
        if color is None and _is_opener(text, i + 1):
            flush(seg, i, None)
            color = int(text[i + 1 : i + 7], 16)
            i += 8  # '#', six hex digits, the consumed space
            seg = i
        elif color is not None:
            flush(seg, i, color)
            color = None
            i += 1
            seg = i
        else:
            # A '#' that cannot open a run and is not inside one: literal text.
            i += 1
    flush(seg, n, color)
    return spans
