"""USB HID Keyboard usage codes (page 0x07).

A trimmed subset of the HID Usage Tables sufficient for typical
keyboard macros built with :class:`touchy_pad.screens.MacroAction`.

Constants are plain integers \u2014 they go straight into
``touchy.KeyEvent.keycode`` on the wire. Modifier bits live in
``KeyEvent.modifiers`` (see :data:`MOD_LCTRL` and friends below).

Reference: https://usb.org/sites/default/files/hut1_22.pdf section 10.
"""

from __future__ import annotations

# --- Modifier bitmask byte ------------------------------------------------

MOD_LCTRL = 0x01
MOD_LSHIFT = 0x02
MOD_LALT = 0x04
MOD_LGUI = 0x08
MOD_RCTRL = 0x10
MOD_RSHIFT = 0x20
MOD_RALT = 0x40
MOD_RGUI = 0x80

# Aliases (Ctrl / Shift / Alt / Gui usually mean "left"-variant).
CTRL = MOD_LCTRL
SHIFT = MOD_LSHIFT
ALT = MOD_LALT
GUI = MOD_LGUI

# --- Keycodes (usage page 0x07) ------------------------------------------

# Letters (A=4, B=5, ..., Z=29).
KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J = range(4, 14)
KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T = range(14, 24)
KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z = range(24, 30)

# Number row (1=30, 2=31, ..., 9=38, 0=39).
KEY_1, KEY_2, KEY_3, KEY_4, KEY_5 = range(30, 35)
KEY_6, KEY_7, KEY_8, KEY_9, KEY_0 = range(35, 40)

KEY_ENTER = 0x28
KEY_ESC = 0x29
KEY_BACKSPACE = 0x2A
KEY_TAB = 0x2B
KEY_SPACE = 0x2C
KEY_MINUS = 0x2D
KEY_EQUAL = 0x2E
KEY_LBRACKET = 0x2F
KEY_RBRACKET = 0x30
KEY_BACKSLASH = 0x31
KEY_SEMICOLON = 0x33
KEY_QUOTE = 0x34
KEY_GRAVE = 0x35
KEY_COMMA = 0x36
KEY_DOT = 0x37
KEY_SLASH = 0x38
KEY_CAPSLOCK = 0x39

# F-keys.
KEY_F1, KEY_F2, KEY_F3, KEY_F4 = 0x3A, 0x3B, 0x3C, 0x3D
KEY_F5, KEY_F6, KEY_F7, KEY_F8 = 0x3E, 0x3F, 0x40, 0x41
KEY_F9, KEY_F10, KEY_F11, KEY_F12 = 0x42, 0x43, 0x44, 0x45

# Navigation / editing.
KEY_INSERT = 0x49
KEY_HOME = 0x4A
KEY_PAGEUP = 0x4B
KEY_DELETE = 0x4C
KEY_END = 0x4D
KEY_PAGEDOWN = 0x4E
KEY_RIGHT = 0x4F
KEY_LEFT = 0x50
KEY_DOWN = 0x51
KEY_UP = 0x52


# --- Helpers --------------------------------------------------------------

# Plain-ASCII translation table for printable characters reachable without
# modifier keys on a US QWERTY layout. Used by `text_to_macro_steps()` so
# callers can write `macro("Hello\n")` instead of spelling out keycodes.
_ASCII_TO_KEY: dict[str, tuple[int, int]] = {
    " ": (KEY_SPACE, 0),
    "\t": (KEY_TAB, 0),
    "\n": (KEY_ENTER, 0),
    "-": (KEY_MINUS, 0),
    "=": (KEY_EQUAL, 0),
    "[": (KEY_LBRACKET, 0),
    "]": (KEY_RBRACKET, 0),
    "\\": (KEY_BACKSLASH, 0),
    ";": (KEY_SEMICOLON, 0),
    "'": (KEY_QUOTE, 0),
    "`": (KEY_GRAVE, 0),
    ",": (KEY_COMMA, 0),
    ".": (KEY_DOT, 0),
    "/": (KEY_SLASH, 0),
}
for _c, _k in zip("abcdefghijklmnopqrstuvwxyz", range(KEY_A, KEY_A + 26), strict=True):
    _ASCII_TO_KEY[_c] = (_k, 0)
    _ASCII_TO_KEY[_c.upper()] = (_k, MOD_LSHIFT)
for _c, _k in zip(
    "1234567890",
    [KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0],
    strict=True,
):
    _ASCII_TO_KEY[_c] = (_k, 0)
# Shifted-number row (US layout).
for _c, _k in zip(
    "!@#$%^&*()",
    [KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0],
    strict=True,
):
    _ASCII_TO_KEY[_c] = (_k, MOD_LSHIFT)
for _c, _shifted in [
    ("_", "-"),
    ("+", "="),
    ("{", "["),
    ("}", "]"),
    ("|", "\\"),
    (":", ";"),
    ('"', "'"),
    ("~", "`"),
    ("<", ","),
    (">", "."),
    ("?", "/"),
]:
    k, _ = _ASCII_TO_KEY[_shifted]
    _ASCII_TO_KEY[_c] = (k, MOD_LSHIFT)


def char_to_key(c: str) -> tuple[int, int] | None:
    """Return ``(keycode, modifiers)`` for an ASCII character (US layout).

    Returns ``None`` for characters that have no single-keystroke mapping.
    """
    return _ASCII_TO_KEY.get(c)
