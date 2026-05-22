# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a StreamDeck-style key grid as a Touchy ``Screen``.

Each TouchyDeck cell maps 1:1 to a key index in the StreamDeck library:
key ``k`` is an ``image_button`` widget with id ``f"{ID_PREFIX}{k}"`` and
both ``on_press`` and ``on_release`` action slots wired to the same
host_code ``HOST_CODE_BASE + k``. The host distinguishes which edge
fired via ``LvEvent.code`` (1=PRESSED, 8=RELEASED).

The default asset path for cell ``k`` is ``images/sdk_key_<k>.bin``.
:class:`TouchyDeck.set_key_image` overwrites that file with caller-
supplied bytes (auto-converted to LVGL's native ``.bin`` by
:meth:`TouchyClient.file_save`) and triggers a screen reload.
"""

from __future__ import annotations

from ..api import screens as _s

#: Touchy screen name reserved for the active StreamDeck-emulation grid.
SCREEN_NAME = "touchydeck"

#: Widget-id prefix for the per-key image buttons.
ID_PREFIX = "sdk_key_"

#: First ``ActionHost.code`` allocated to TouchyDeck. The key index is
#: added to this; e.g. key 0 = ``0xA000``, key 5 = ``0xA005``. Picked
#: above the demo screens' user-defined codes (0x100-0x103) so a single
#: device can host both at once.
HOST_CODE_BASE = 0xA000


def host_code_for(key: int) -> int:
    """Return the ``ActionHost.code`` allocated to ``key``."""
    return HOST_CODE_BASE + int(key)


def key_for_host_code(code: int) -> int | None:
    """Inverse of :func:`host_code_for`. Returns ``None`` if ``code`` is
    outside the TouchyDeck range."""
    code = int(code)
    if code < HOST_CODE_BASE:
        return None
    k = code - HOST_CODE_BASE
    if k > 0xFFF:
        return None
    return k


def asset_path_for(key: int) -> str:
    """Filesystem path on the device for cell ``key``'s image asset.

    ``.bin`` is the on-device extension that LVGL's built-in decoder
    matches; :meth:`TouchyClient.file_save` rewrites supplied
    PNG/JPEG/etc. paths to this on upload.
    """
    return f"images/{ID_PREFIX}{int(key)}.bin"


def build_screen(
    *,
    cols: int,
    rows: int,
    initial_asset: str | None = None,
) -> _s.Screen:
    """Build the :data:`SCREEN_NAME` screen with ``cols × rows`` keys.

    Iteration order matches StreamDeck convention: keys numbered
    left-to-right, top-to-bottom starting at 0. Each cell is an
    :func:`image_button` with both press and release edges wired to
    :func:`host_code_for` ``(k)``. ``initial_asset`` is the path to a
    placeholder image already uploaded to the device (e.g.
    ``"images/sdk_blank.bin"``); when ``None`` the per-key asset path
    is referenced directly and missing files render as LVGL's
    decode-failure placeholder until the host calls
    :meth:`TouchyDeck.set_key_image`.
    """
    if cols < 1 or rows < 1:
        raise ValueError("cols and rows must be >= 1")
    screen = _s.Screen(SCREEN_NAME, layout=_s.grid(cols=cols, rows=rows, gap=4))
    for r in range(rows):
        for c in range(cols):
            k = r * cols + c
            asset = initial_asset if initial_asset is not None else asset_path_for(k)
            btn = _s.image_button(
                id=f"{ID_PREFIX}{k}",
                asset=asset,
                on_press=host_code_for(k),
                on_release=host_code_for(k),
            )
            screen += _s.cell(btn, col=c, row=r)
    return screen
