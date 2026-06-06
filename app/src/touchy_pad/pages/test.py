"""Test/demo page body — Stage 16/18/20 widget showcase with host callbacks."""

from __future__ import annotations

import logging

from .. import _proto
from ..api.screens import (
    ANIM_PATH_EASE_IN_OUT,
    ANIM_PATH_LINEAR,
    PROP_BG_COLOR,
    PROP_HEIGHT,
    PROP_IMAGE_RECOLOR_OPA,
    PROP_TRANSFORM_WIDTH,
    PROP_WIDTH,
    PROP_X,
    STATE_PRESSED,
    Layer,
    absolute,
    anim_track,
    animation,
    cell,
    checkbox,
    force_render,
    fps,
    grid,
    host_action,
    image_button,
    log_line,
    macro_action,
    rect,
    slider,
    spacer,
    style,
    transition,
)
from ..api.screens import button as _button

logger = logging.getLogger(__name__)


def build() -> tuple[str, _proto.Widget]:
    """Return ``("test", widget)`` for the Stage-16/18/20 widget showcase.

    The page is an absolute layer wrapping a 4×7 grid of controls (button,
    slider, checkbox, image-button, log-line, fps, force-render) with a
    freely-positioned animated red dot overlay that exercises the
    declarative-animation pipeline.
    """
    from ..api import hid_keys as k
    from ..api import macros as m

    showcase = Layer(layout=grid(cols=4, rows=7, gap=8))
    showcase += cell(
        _button(
            "hello",
            text="Type 'hi'",
            on_click=macro_action([m.key_tap(k.KEY_H, k.MOD_LSHIFT), m.key_tap(k.KEY_I)]),
            style=[
                style(
                    transition=transition(
                        props=[PROP_TRANSFORM_WIDTH, PROP_BG_COLOR],
                        path=ANIM_PATH_LINEAR,
                        duration_ms=200,
                    )
                ),
                style(transform_width=20, bg_color=0xCC2222, for_state=STATE_PRESSED),
            ],
        ),
        col=0,
        row=0,
        grow_x=1,
        grow_y=1,
    )
    showcase += cell(
        _button(
            "ping",
            text="Ping host",
            on_click=host_action(on_event=lambda e: logger.info("[ping]   widget=%r", e.user_data)),
        ),
        col=1,
        row=0,
        col_span=3,
        grow_x=1,
        grow_y=1,
    )
    showcase += cell(
        slider(
            "level",
            min=0,
            max=100,
            value=42,
            on_change=host_action(
                on_event=lambda e: logger.info("[slider] widget=%r value=%s", e.user_data, e.value)
            ),
        ),
        col=0,
        row=1,
        col_span=3,
        grow_x=1,
        grow_y=1,
    )
    showcase += cell(
        checkbox(
            "enable",
            text="Enabled",
            checked=True,
            on_change=host_action(
                on_event=lambda e: logger.info("[check]  widget=%r on=%s", e.user_data, e.checked)
            ),
        ),
        col=0,
        row=2,
        grow_x=1,
        grow_y=1,
    )
    showcase += cell(
        image_button(
            "smile",
            asset="F:host/images/smiley.png",
            on_click=host_action(on_event=lambda e: logger.info("[smile]  widget=%r", e.user_data)),
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
        row=2,
        col_span=2,
        grow_x=1,
        grow_y=1,
    )
    showcase += cell(force_render("force"), col=3, row=1, grow_x=1, grow_y=1)
    showcase += cell(fps("fps"), col=3, row=2, grow_x=1, grow_y=1)
    showcase += cell(log_line("log"), col=0, row=3, col_span=4, row_span=4, grow_x=1, grow_y=1)

    grid_widget = _proto.Widget()
    showcase.copy_into(grid_widget)

    # Stage 59 — wrap the showcase grid in an absolute layer so we can
    # overlay a freely-positioned animated "red dot" that exercises the
    # declarative-animation pipeline end-to-end.
    # Screen is 480 px wide; dot grows to 100 px, so x_end=380 puts the
    # right edge flush with the screen edge.
    red_dot = spacer(
        id="reddot",
        rect=rect(x=10, y=10, w=10, h=10),
        style=[style(bg_color=0xE53935, radius=32767)],
        animations=[
            animation(
                anim_track(PROP_WIDTH, 10, 100),
                anim_track(PROP_HEIGHT, 10, 100),
                anim_track(PROP_X, 10, 380),
                duration_ms=1000,
                path=ANIM_PATH_EASE_IN_OUT,
                repeat_count=0,
                repeat_delay_ms=500,
                reverse=True,
                reverse_delay_ms=100,
                reverse_duration_ms=300,
            ),
        ],
    )
    outer = Layer(layout=absolute(), widgets=[red_dot, grid_widget])
    test_widget = _proto.Widget()
    outer.copy_into(test_widget)

    return "test", test_widget
