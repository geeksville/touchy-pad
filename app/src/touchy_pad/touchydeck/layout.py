# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a StreamDeck-style key grid as a Touchy **user-screen page body**.

Stage 100 port. The grid is no longer a standalone ``Screen`` that
replaces the boot chrome; it is a bare :class:`Widget` (a ``LayoutGrid``)
pushed via :meth:`Touchy.user_screen_save`, so it renders *inside* the
default chrome's ``widget_ref(id="page")`` body. This mirrors the Rust
``touchy-opendeck`` plugin (see ``rust/touchy-opendeck/src/layout.rs``).

Each cell maps 1:1 to a key index in the StreamDeck library: key ``k``
is an ``image_button`` widget with id :func:`key_widget_id` ``(k)`` and
both ``on_press`` and ``on_release`` action slots wired to the same
host-code :func:`host_code_for` ``(k)``. The host distinguishes which
edge fired via ``LvEvent.code`` (1=PRESSED, 8=RELEASED).

Every cell's released image starts at one shared blank path (see
:data:`BLANK_BIN`); per-key artwork arrives via the Stage-86 in-place
image-slot swap (see :meth:`Touchy.set_image_button_slot`), so the grid
never flashes and a key the user is pressing keeps its touch state.
"""

from __future__ import annotations

from .._proto import Widget
from ..api import screens as _s

#: Bare stem of the user-screen page body this module uploads. Pushed to
#: ``F:host/uscr/<PAGE_NAME>.pb`` via
#: :meth:`~touchy_pad.api.Touchy.user_screen_save`; the default chrome's
#: ``widget_ref(id="page")`` displays it.
PAGE_NAME = "touchydeck"

#: Widget-id prefix for the per-key image buttons.
ID_PREFIX = "sdk_key_"

#: First ``ActionHost.code`` allocated to TouchyDeck. The key index is
#: added to this; e.g. key 0 = ``0xA000``, key 5 = ``0xA005``. Picked
#: above the demo screens' user-defined codes (0x100-0x103) and below
#: the OpenDeck plugin's ``0xB000`` base so a single device can host
#: both at once.
HOST_CODE_BASE = 0xA000


def host_code_for(key: int) -> int:
    """Return the ``ActionHost.code`` allocated to ``key``."""
    return HOST_CODE_BASE + int(key)


def key_for_host_code(code: int) -> int | None:
    """Inverse of :func:`host_code_for`.

    Returns ``None`` if ``code`` is outside the TouchyDeck range
    (``HOST_CODE_BASE`` … ``HOST_CODE_BASE + 0xFFF``).
    """
    code = int(code)
    if code < HOST_CODE_BASE:
        return None
    k = code - HOST_CODE_BASE
    if k > 0xFFF:
        return None
    return k


def key_widget_id(key: int) -> str:
    """Stable per-key ``ImageButton`` widget id (``f"{ID_PREFIX}{key}"``).

    Used both when building the grid and when targeting a key for an
    in-place image-slot repaint via
    :meth:`~touchy_pad.api.Touchy.set_image_button_slot`.
    """
    return f"{ID_PREFIX}{int(key)}"


#: A minimal opaque "blank key" LVGL ``.bin``: a 1×1 dark-grey
#: (``0x101010``) RGB565 pixel, stretched to fill the cell. Seeded into
#: the image cache at attach so every key has a valid released image
#: before any artwork arrives, and reused to clear a key. RGB565
#: (native) so it takes the device's zero-copy mmap fast path.
#:
#: Layout: 12-byte v9 header (``magic, cf, flags, w, h, stride, rsvd``)
#: + one little-endian RGB565 word. ``rgb565(0x10,0x10,0x10) == 0x1082``.
#: Identical to the Rust ``layout::BLANK_BIN`` so the two plugins paint
#: the same placeholder.
BLANK_BIN: bytes = bytes(
    [
        0x19,
        0x12,
        0x00,
        0x00,  # magic, cf=RGB565, flags
        0x01,
        0x00,
        0x01,
        0x00,  # w=1, h=1
        0x02,
        0x00,
        0x00,
        0x00,  # stride=2, reserved
        0x82,
        0x10,  # pixel: rgb565(0x10,0x10,0x10) little-endian
    ]
)


def build_page(
    cols: int,
    rows: int,
    *,
    blank_path: str,
) -> Widget:
    """Build the TouchyDeck page body: a ``cols × rows`` key grid.

    Each cell is a per-key ``image_button`` (see :func:`key_widget_id`)
    whose released image starts at ``blank_path`` (a cached blank
    ``.bin``). The button carries this key's host code on both press and
    release, so it stays clickable regardless of its current artwork.
    Repaints swap the ``released`` / ``pressed`` image slot in place via
    :meth:`~touchy_pad.api.Touchy.set_image_button_slot` — never
    rebuilding the widget, so a key the user is pressing keeps its touch
    state and still emits a release event.

    Keys are numbered left-to-right, top-to-bottom from 0 — matching
    StreamDeck convention. Cells are stretched to fill their grid slot
    (``grow_x=1, grow_y=1``, Stage 72). Returned as a bare
    :class:`~touchy_pad.api.protobuf.Widget` (a ``LayoutGrid``) ready for
    :meth:`~touchy_pad.api.Touchy.user_screen_save`.

    Parameters
    ----------
    cols, rows:
        Grid dimensions; both must be ``>= 1``.
    blank_path:
        Cached blank-image path every key starts at (the result of
        seeding :data:`BLANK_BIN` through an
        :class:`~touchy_pad.api.ImageCache`).
    """
    if cols < 1 or rows < 1:
        raise ValueError("cols and rows must be >= 1")
    children: list[Widget] = []
    for r in range(rows):
        for c in range(cols):
            k = r * cols + c
            btn = _s.image_button(
                id=key_widget_id(k),
                asset=blank_path,
                on_press=host_code_for(k),
                on_release=host_code_for(k),
            )
            children.append(_s.cell(btn, col=c, row=r, grow_x=1, grow_y=1))
    page = Widget(id="touchydeck_root")
    page.layout_grid.cols = cols
    page.layout_grid.rows = rows
    page.layout_grid.gap = 4
    page.layout_grid.layout.children.extend(children)
    return page
