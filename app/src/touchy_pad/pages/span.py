"""Rich-text page body — the stage-lb24 span-group showcase (and regression page).

Three panes, top to bottom:

1. **The voicebot regression.** A ``label(recolor=True)`` in ``LONG_MODE_DOTS``
   whose coloured run straddles the wrap point. Before stage lb24 the device drew
   that run with LVGL's label recolour parser, which loses its command state at a
   line start, so the closing ``#`` was re-read as an opener: the run's colour was
   lost, the next word swallowed, the line overran the box and the seam glyphs
   were drawn twice (overlapping). It now renders as a span group, so the run
   stays one colour and the wrap is clean. Hard to reproduce in the simulator
   (Qt does no LVGL glyph layout) — this page exists to be eyeballed on hardware.

2. **An explicit ``span_group``** with per-run colours and font sizes, capped at
   two lines with ellipsis.

3. **The same text as a plain label**, for comparison.
"""

from __future__ import annotations

from .. import _proto
from ..api.screens import (
    Layer,
    SpanOverflow,
    TextAlign,
    absolute,
    label,
    rect,
    span,
    span_group,
    style,
)

#: The exact quip from the bug report: the ``#A020F0 cocks head#`` run ends
#: right where the panel wraps, which is what used to corrupt the line.
_REGRESSION_TEXT = (
    '#FF0000 peeks from behind curtains# Oh, Mike\'s been "hiding" somewhere, '
    "avoiding his problems, as usual. #A020F0 cocks head#I bet he's in a cave, "
    "living off grubs and studying the stars."
)


def build() -> tuple[str, _proto.Widget]:
    """Return ``("span", widget)`` for the stage-lb24 rich-text page."""
    # Absolute so each pane's rect is exact — including the regression pane's
    # 464×140 box, which is what makes the coloured run straddle the wrap.
    body = Layer(layout=absolute())

    body += label(
        "recolor_regression",
        text=_REGRESSION_TEXT,
        font_size=30,
        text_align=TextAlign.LEFT,
        long_mode=_proto.LongMode.LONG_MODE_DOTS,
        recolor=True,
        rect=rect(x=8, y=8, w=464, h=140),
        style=style(text_color=0xFFFFFF),
    )

    body += span_group(
        "rich",
        [
            span("span groups: ", color=0x808080),
            span("violet", color=0xA020F0, font_size=30),
            span("  and  ", color=0x808080),
            span("amber", color=0xFFB000, font_size=30),
            span(" — two lines, then an ellipsis.", color=0xFFFFFF),
        ],
        overflow=SpanOverflow.SPAN_OVERFLOW_ELLIPSIS,
        max_lines=2,
        rect=rect(x=8, y=156, w=464, h=52),
    )

    body += label(
        "plain",
        text="(a plain label, for comparison)",
        font_size=16,
        text_align=TextAlign.LEFT,
        rect=rect(x=8, y=216, w=464, h=40),
        style=style(text_color=0xAAAAAA),
    )

    result = _proto.Widget(id="span_page")
    body.copy_into(result)
    return "span", result
