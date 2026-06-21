"""Stage 16 \u2014 device-side macro authoring helpers.

A *macro* is a list of :class:`touchy._proto.MacroStep` protobufs that the
firmware's macro runner replays as raw USB HID events. Each helper here
returns a single ``MacroStep`` so callers can compose macros declaratively::

    from touchy_pad.macros import key_tap, mouse_click, delay
    from touchy_pad import hid_keys

    steps = [
        key_tap(hid_keys.KEY_H),
        key_tap(hid_keys.KEY_I),
        delay(100),
        mouse_click(),
    ]

Pass the resulting list to :func:`touchy_pad.screens.macro_action` to attach
it to a widget event slot.
"""

from __future__ import annotations

from .. import _proto

# Mirror the firmware-side constants so callers don't need to import
# usb_hid.h. Bit layout matches a standard HID mouse button mask.
HID_MOUSE_BTN_LEFT = 0x01
HID_MOUSE_BTN_RIGHT = 0x02
HID_MOUSE_BTN_MIDDLE = 0x04


def _key_event(keycode: int, modifiers: int) -> _proto.KeyEvent:
    return _proto.KeyEvent(keycode=keycode, modifiers=modifiers)


def key_down(keycode: int, modifiers: int = 0) -> _proto.MacroStep:
    """Press a key (no release). Pair with :func:`key_up` for chords."""
    return _proto.MacroStep(key_down=_key_event(keycode, modifiers))


def key_up(keycode: int = 0, modifiers: int = 0) -> _proto.MacroStep:
    """Release every currently-held key.

    The firmware's runner sends an "all keys released" report on this
    step; the ``keycode`` / ``modifiers`` arguments exist for symmetry but
    are currently ignored on the device.
    """
    return _proto.MacroStep(key_up=_key_event(keycode, modifiers))


def key_tap(keycode: int, modifiers: int = 0) -> _proto.MacroStep:
    """Press then release a single key, optionally with modifiers."""
    return _proto.MacroStep(key_tap=_key_event(keycode, modifiers))


def mouse_button_down(buttons: int = HID_MOUSE_BTN_LEFT) -> _proto.MacroStep:
    """Press one or more mouse buttons (bitmask)."""
    return _proto.MacroStep(mouse_button_down=buttons)


def mouse_button_up(buttons: int = 0) -> _proto.MacroStep:
    """Release every currently-held mouse button.

    The ``buttons`` argument exists for symmetry but the firmware always
    emits an "all released" report.
    """
    return _proto.MacroStep(mouse_button_up=buttons)


def mouse_click(buttons: int = HID_MOUSE_BTN_LEFT) -> _proto.MacroStep:
    """Press then release one or more mouse buttons."""
    return _proto.MacroStep(mouse_click=buttons)


def mouse_move(dx: int | None = None, dy: int | None = None) -> _proto.MacroStep:
    """Send a single relative mouse-cursor move.

    Values are signed; the firmware clamps each component to the int8
    range mandated by the boot-protocol mouse report. ``dx`` / ``dy``
    left as ``None`` are omitted from the wire ``Move``; inside a
    :func:`~touchy_pad.api.screens.trackpad` ``on_move`` action that
    means "use the trackpad's live per-frame delta for that axis".
    """
    move = _proto.Move()
    if dx is not None:
        move.dx = dx
    if dy is not None:
        move.dy = dy
    return _proto.MacroStep(mouse_move=move)


def scroll_move(dx: int | None = None, dy: int | None = None) -> _proto.MacroStep:
    """Send a single relative scroll-wheel move.

    ``dy`` drives the vertical wheel, ``dx`` the horizontal pan. Values
    are signed and int8-clamped on the device. ``dx`` / ``dy`` left as
    ``None`` are omitted from the wire ``Move``; inside a
    :func:`~touchy_pad.api.screens.trackpad` ``on_scroll`` action that
    means "use the trackpad's live per-frame scroll delta for that axis".
    """
    move = _proto.Move()
    if dx is not None:
        move.dx = dx
    if dy is not None:
        move.dy = dy
    return _proto.MacroStep(scroll_move=move)


def zoom_move(dx: int | None = None) -> _proto.MacroStep:
    """Send a single relative zoom step (Stage 92).

    ``dx`` (Relative X) carries the signed zoom magnitude: positive zooms
    in, negative zooms out. Left as ``None`` it is omitted from the wire
    ``Move``; inside a :func:`~touchy_pad.api.screens.trackpad`
    ``on_zoom_in`` / ``on_zoom_out`` action that means "use the trackpad's
    live per-frame span change". The device maps this to the de-facto
    desktop zoom gesture: Ctrl held + a vertical scroll by ``dx``.
    """
    move = _proto.Move()
    if dx is not None:
        move.dx = dx
    return _proto.MacroStep(zoom_move=move)


def set_delay(ms: int) -> _proto.MacroStep:
    """Set the sticky inter-step delay (ms) for every subsequent step."""
    if ms < 0:
        raise ValueError("set_delay ms must be non-negative")
    return _proto.MacroStep(set_delay_ms=ms)


def delay(ms: int) -> _proto.MacroStep:
    """One-shot extra pause (ms) before the next step."""
    if ms < 0:
        raise ValueError("delay ms must be non-negative")
    return _proto.MacroStep(delay_ms=ms)


# --- Convenience: build a macro that types an ASCII string -------------------


def type_text(text: str) -> list[_proto.MacroStep]:
    """Translate ``text`` into a list of `key_tap` steps.

    Only single-keystroke ASCII characters (US-QWERTY layout) are
    supported. Characters with no mapping raise ``ValueError``.
    """
    from . import hid_keys

    out: list[_proto.MacroStep] = []
    for ch in text:
        mapping = hid_keys.char_to_key(ch)
        if mapping is None:
            raise ValueError(f"no HID mapping for character {ch!r}")
        kc, mods = mapping
        out.append(key_tap(kc, mods))
    return out
