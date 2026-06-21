"""Public Python API for the Touchy-Pad device.

This package is the supported entry point for application code.
Everything in here is part of the stable API; everything outside
this package (``touchy_pad.client``, ``touchy_pad._proto``, …) is
considered internal and may change without notice.

Typical usage::

    from touchy_pad.api import touchy_open, Screen, button, row

    with touchy_open() as pad:
        s = Screen("home", layout=row(gap=8))
        s += button("hi")
        pad.screen_save(s)
        pad.screen_load("home")

Submodules:

* :mod:`touchy_pad.api.screens` — host-side DSL for authoring
  ``Screen`` / ``Layer`` / widget protobufs.
* :mod:`touchy_pad.api.macros` — DSL for keyboard / mouse macros.
* :mod:`touchy_pad.api.hid_keys` — HID keycode constants.
* :mod:`touchy_pad.api.images` — image helpers used by the demos.
* :mod:`touchy_pad.api.lvgl_image` — LVGL binary image conversion.
* :mod:`touchy_pad.api.protobuf` — the raw generated protobuf
  classes (``Screen``, ``Widget``, ``Layout*``, …) for users who
  want to build messages without the higher-level DSL.
"""

from __future__ import annotations

from ..paths import (
    DEFAULT_SCREEN_PATH,
    SCREENS_DIR,
    USER_SCREENS_DIR,
)
from . import hid_keys, images, lvgl_image, protobuf
from .device import (
    MINIMUM_FIRMWARE_VERSION,
    IncompatibleFirmwareError,
    Touchy,
    touchy_get_pad_ids,
    touchy_open,
)
from .macros import (
    HID_MOUSE_BTN_LEFT,
    HID_MOUSE_BTN_MIDDLE,
    HID_MOUSE_BTN_RIGHT,
    delay,
    key_down,
    key_tap,
    key_up,
    mouse_button_down,
    mouse_button_up,
    mouse_click,
    mouse_move,
    scroll_move,
    set_delay,
    type_text,
)
from .screens import (
    AnimPath,
    ImageSource,
    Layer,
    LvState,
    Screen,
    StyleProp,
    TextAlign,
    absolute,
    action,
    arc,
    build_default_screen,
    build_demo,
    build_demo_screen,
    build_setup_screen,
    build_user_pages,
    button,
    cell,
    change_widget_ref_action,
    checkbox,
    col,
    device_action,
    flex,
    force_render,
    fps,
    grid,
    grow,
    host_action,
    image,
    image_button,
    label,
    log_line,
    macro_action,
    next_widget_action,
    prev_widget_action,
    rect,
    ripple_animation,
    row,
    slider,
    spacer,
    style,
    toggle,
    trackpad,
    transition,
    widget_ref,
)
from .sim_registry import (
    create_sim_device,
    destroy_sim_device,
    get_sim_serial,
    get_sim_transport,
)

__all__ = [
    # Device lifecycle.
    "Touchy",
    "touchy_open",
    "touchy_get_pad_ids",
    "IncompatibleFirmwareError",
    "MINIMUM_FIRMWARE_VERSION",
    # Sim device registry (for testing / host apps without USB hardware).
    "create_sim_device",
    "destroy_sim_device",
    "get_sim_serial",
    "get_sim_transport",
    # Submodules.
    "hid_keys",
    "images",
    "lvgl_image",
    "protobuf",
    # Screen / widget DSL.
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
    "ImageSource",
    "arc",
    "spacer",
    "widget_ref",
    "trackpad",
    "ripple_animation",
    "log_line",
    "fps",
    "force_render",
    "build_demo",
    "build_demo_screen",
    "build_default_screen",
    "build_setup_screen",
    "build_user_pages",
    # On-device filesystem paths (Stage 68).
    "SCREENS_DIR",
    "USER_SCREENS_DIR",
    "DEFAULT_SCREEN_PATH",
    # Macros.
    "key_down",
    "key_up",
    "key_tap",
    "mouse_button_down",
    "mouse_button_up",
    "mouse_click",
    "mouse_move",
    "scroll_move",
    "set_delay",
    "delay",
    "type_text",
    "HID_MOUSE_BTN_LEFT",
    "HID_MOUSE_BTN_RIGHT",
    "HID_MOUSE_BTN_MIDDLE",
    # Proto enum namespaces.
    "AnimPath",
    "LvState",
    "StyleProp",
    "TextAlign",
]
