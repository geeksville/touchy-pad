"""Trackpad page body — full-bleed multitouch trackpad with layered LVGL draw order."""

from __future__ import annotations

from .. import _proto
from ..api import hid_keys
from ..api.macros import key_tap
from ..api.screens import (
    AnimPath,
    Layer,
    cell,
    grid,
    image,
    label,
    macro_action,
    ripple_animation,
    spacer,
    style,
    trackpad,
)


def build(background_image: str | None = None) -> tuple[str, _proto.Widget]:
    """Return ``("trackpad", widget)`` for the full-bleed multitouch trackpad page.

    A 1×1 grid stacks LVGL children in the same cell so draw order provides
    layering without any custom drawing inside the trackpad widget.

    If *background_image* is a device filepath (e.g. ``"F:host/images/touchy.png"``),
    an :func:`~touchy_pad.api.screens.image` widget is used as the background
    layer.  Otherwise the default two-layer background is used:

    1. A :func:`~touchy_pad.api.screens.spacer` filled with the dark
       background colour (``0x1a1a2e``).
    2. A dim hint :func:`~touchy_pad.api.screens.label` ("Touch here"),
       content-sized and centred by the grid default.

    The final layer is always the :func:`~touchy_pad.api.screens.trackpad`
    with a transparent background — it is drawn last so it receives all touch
    events.
    """
    container = Layer(layout=grid(cols=1, rows=1))

    # Layer 1 — dark background fill.  A spacer is used instead of a label
    # or layout widget because build_spacer() calls lv_obj_remove_style_all(),
    # which lets the user-supplied bg_color style take effect without fighting
    # a local lv_obj_set_style_bg_opa(LV_OPA_TRANSP) that those other widget
    # types set internally.
    container += cell(
        spacer("pad_bg", style=style(bg_color=0x1A1A2E, radius=16)),
        col=0,
        row=0,
        grow_x=1,
        grow_y=1,
    )

    if background_image is not None:
        # Single image layer fills the cell. but we want rounded corners
        container += cell(
            image("pad_bg", asset=background_image, style=style(radius=16)),
            col=0,
            row=0,
            grow_x=1,
            grow_y=1,
        )
    else:
        # Layer 2 — dim hint text, content-sized and centred by the grid default.
        container += cell(
            label("pad_hint", text="Touch here", font_size=30, style=style(text_color=0x334466)),
            col=0,
            row=0,
        )

    # Layer 3 — transparent trackpad on top; snarfs all touch events because
    # LVGL dispatches input to the topmost (last-drawn) object.
    container += cell(
        trackpad(
            "pad",
            left_touch_color=0x00BFFF,
            right_touch_color=0xFFA500,
            middle_touch_color=0xFF44FF,
            scrollbar_color=0xADD8E6,
            touch_ripple=ripple_animation(
                start_opa=180,
                max_radius=45,
                duration_ms=400,
                path=AnimPath.EASE_OUT,
            ),
            tap_ripple=ripple_animation(
                start_opa=255,
                max_radius=70,
                duration_ms=300,
                path=AnimPath.EASE_OUT,
                border_width=4,
            ),
            # Stage 91 — enable single-finger swipe detection so the device
            # logs recognised swipes (no Actions bound yet; we just watch
            # the ESP_LOGI output to validate the gesture engine). A flick
            # of ~60 px within 300 ms triggers; consecutive swipes repeat
            # every ~40 px / 200 ms while the finger keeps moving.
            swipe_initial_distance=60,
            swipe_initial_time=300,
            swipe_consecutive_distance=40,
            swipe_consecutive_time=200,
            swipe_angle=30,
            # Stage 92 — enable two-finger zoom (pinch) detection. A pinch
            # whose span changes by ~50 px within 300 ms fires, then repeats
            # every ~25 px while the fingers keep moving. Bound to the
            # de-facto desktop zoom shortcuts: Ctrl+"=" (zoom in) and
            # Ctrl+"-" (zoom out). The device also logs each recognised
            # zoom via ESP_LOGI for hardware validation.
            zoom_initial_distance=50,
            zoom_initial_time=300,
            zoom_consecutive_distance=25,
            zoom_consecutive_time=200,
            on_zoom_in=macro_action([key_tap(hid_keys.KEY_EQUAL, hid_keys.CTRL)]),
            on_zoom_out=macro_action([key_tap(hid_keys.KEY_MINUS, hid_keys.CTRL)]),
        ),
        col=0,
        row=0,
        grow_x=1,
        grow_y=1,
    )

    result = _proto.Widget(id="pad_container")
    container.copy_into(result)
    return "trackpad", result
