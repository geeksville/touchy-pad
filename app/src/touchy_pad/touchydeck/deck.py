# SPDX-License-Identifier: GPL-3.0-or-later
"""``TouchyDeck`` — a Touchy-Pad masquerading as an Elgato StreamDeck.

See :mod:`touchy_pad.touchydeck` for the package-level intro.

Wire-level mapping
------------------
Each cell ``k`` (``0 <= k < KEY_COUNT``, indexed left-to-right then
top-to-bottom) is a Touchy ``image_button`` widget with id
``f"{layout.ID_PREFIX}{k}"`` whose ``on_press`` and ``on_release``
action slots both carry the host-code ``layout.HOST_CODE_BASE + k``.
The two edges are distinguished on the host by ``LvEvent.code``:

* ``1`` (``LV_EVENT_PRESSED``)  → key pressed,  ``state = True``
* ``8`` (``LV_EVENT_RELEASED``) → key released, ``state = False``

The base ``StreamDeck`` class polls :meth:`_read_control_states` from
a daemon thread (~20 Hz by default) and diffs the returned per-key
booleans against its own ``last_key_states`` cache, firing
``self.key_callback`` on each edge — so we just have to keep our
snapshot honest.

Thread safety
-------------
``TouchyClient`` is single-shot per RPC: there's no internal lock, so
concurrent calls from the read thread and the caller would corrupt the
request/response stream. We serialise every RPC through the base class'
``self.update_lock`` (an ``RLock`` it already holds for its own
state-update paths).
"""

from __future__ import annotations

import logging
import threading
from collections.abc import Callable
from typing import TYPE_CHECKING, Any

# `streamcontroller-streamdeck` is an *optional* dep. Importing the
# package lazily lets the rest of `touchy_pad` import cleanly even when
# StreamController support isn't installed; users only hit the import
# error when they actually construct a `TouchyDeck`.
try:
    from StreamDeck.Devices.StreamDeck import ControlType, StreamDeck
except ImportError as _e:  # pragma: no cover - environmental
    _IMPORT_ERROR: Exception | None = _e
    StreamDeck = object  # type: ignore[assignment,misc]
    ControlType = None  # type: ignore[assignment]
else:
    _IMPORT_ERROR = None

if TYPE_CHECKING:
    from ..client import TouchyClient

from ..usb_ids import PID as _TOUCHY_PID
from ..usb_ids import VID as _TOUCHY_VID
from . import layout as _layout

_LOG = logging.getLogger(__name__)

# LVGL event codes that flag press / release edges. Must stay in sync
# with the firmware's `widget_attach_actions(..., LV_EVENT_PRESSED ...)`
# / `LV_EVENT_RELEASED` / `LV_EVENT_PRESS_LOST` registrations in
# `firmware/main/widget_builders.cpp`. Values come from the LVGL 9.x
# `lv_event_code_t` enum in `lvgl/src/misc/lv_event.h` — keep aligned
# with the firmware's actual emission, not with older LVGL 8 docs.
_LV_EVENT_PRESSED = 1
_LV_EVENT_PRESS_LOST = 3
_LV_EVENT_RELEASED = 11


class _FakeTransportDevice:
    """Minimal ``StreamDeck.Transport.Transport.Device`` stand-in.

    The base ``StreamDeck`` class stores its USB ``Device`` on
    ``self.device`` and only ever pokes a handful of methods on it:
    ``is_open()``, ``connected()``, ``open()``, ``close()``, plus the
    HID I/O helpers (which we never reach because we override every
    abstract method that would call them). This shim is just enough to
    keep ``StreamDeck.open()`` / ``.connected()`` from blowing up.
    """

    def __init__(self, client: TouchyClient, serial: str) -> None:
        self._client = client
        self._serial = serial
        self._open = True

    # -- presence ---------------------------------------------------------
    def is_open(self) -> bool:
        return self._open

    def connected(self) -> bool:
        return self._open

    # -- lifecycle --------------------------------------------------------
    def open(self) -> None:
        self._open = True

    def close(self) -> None:
        self._open = False

    # -- identity ---------------------------------------------------------
    def path(self) -> bytes:
        # StreamDeck base uses this as a unique id string in some logs.
        return f"touchy:{self._serial}".encode()

    # Pseudo-USB descriptor methods. Real StreamDeck transports expose
    # these as callables on the device object (the StreamDeck base
    # class and probe tools call them like ``deck.device.vendor_id()``).
    # Values match the Touchy-Pad USB descriptor (see
    # ``touchy_pad.usb_ids``).
    def vendor_id(self) -> int:
        return _TOUCHY_VID

    def product_id(self) -> int:
        return _TOUCHY_PID


class TouchyDeck(StreamDeck):  # type: ignore[misc,valid-type]
    """``StreamDeck`` subclass backed by a Touchy-Pad device.

    Construct with an already-connected :class:`TouchyClient`. Call
    :meth:`open` (inherited from ``StreamDeck``) to start the read
    thread and push the initial empty key grid, then drive it like any
    other StreamDeck (``set_key_callback``, ``set_key_image``, etc.).

    Grid geometry is derived from the device's reported display size
    (``SysBoardInfoResponse.display_{width,height}``): we lay out as
    many StreamDeck-classic-sized 72×72 px keys as physically fit,
    with a 4 px gap between them. Callers may override with explicit
    ``cols`` / ``rows`` ctor kwargs (used by tests and by users who
    want a smaller-than-maximal grid).
    """

    # StreamDeck-classic native key pixel size. Real Elgato hardware
    # also draws 72×72; matching that means the same icon assets and
    # font metrics work on both, and PIL’s default 6 px bitmap font
    # stays legible (it doesn’t survive smooth-downscaling).
    STREAMDECK_KEY_PIXELS = 72
    STREAMDECK_KEY_GAP = 4

    # -- StreamDeck class constants (overridden per-instance in __init__) --
    KEY_COUNT = 18
    KEY_COLS = 6
    KEY_ROWS = 3
    TOUCH_KEY_COUNT = 0
    KEY_PIXEL_WIDTH = 72
    KEY_PIXEL_HEIGHT = 72
    KEY_IMAGE_FORMAT = "PNG"
    KEY_FLIP = (False, False)
    KEY_ROTATION = 0

    DECK_TYPE = "Touchy-Pad (StreamDeck-compatible)"
    DECK_VISUAL = True

    def __init__(
        self,
        client: TouchyClient,
        *,
        cols: int | None = None,
        rows: int | None = None,
        serial: str | None = None,
    ) -> None:
        if _IMPORT_ERROR is not None:  # pragma: no cover - environmental
            raise RuntimeError(
                "streamcontroller-streamdeck is not installed; "
                "install touchy-pad[streamdeck] to use TouchyDeck"
            ) from _IMPORT_ERROR

        # When cols/rows weren't pinned by the caller, query the device
        # for its panel resolution and lay out as many native-size keys
        # as fit. Doing this *before* super().__init__() means the base
        # class sees the right KEY_COUNT when it builds its caches.
        if cols is None or rows is None:
            info = client.sys_board_info_get()
            auto_cols, auto_rows = self._auto_grid(info.display_width, info.display_height)
            if cols is None:
                cols = auto_cols
            if rows is None:
                rows = auto_rows

        if cols < 1 or rows < 1:
            raise ValueError("TouchyDeck cols and rows must be >= 1")

        # Instance-level overrides for the StreamDeck class constants.
        # The base class reads them via `self.KEY_COUNT`, etc., so
        # instance attributes shadow the class defaults cleanly.
        self.KEY_COLS = cols
        self.KEY_ROWS = rows
        self.KEY_COUNT = cols * rows
        self.KEY_PIXEL_WIDTH = self.STREAMDECK_KEY_PIXELS
        self.KEY_PIXEL_HEIGHT = self.STREAMDECK_KEY_PIXELS

        self._client = client
        self._serial = serial or f"touchy-{id(client):x}"
        self._screen_pushed = False
        self._brightness_pct = 100

        # Per-key pressed state. Mutated by `_read_control_states` as it
        # drains the event queue; the base read loop diffs against its
        # own copy to emit edges.
        self._key_state: list[bool] = [False] * self.KEY_COUNT
        # Set True whenever an edge mutates _key_state so the next
        # `_read_control_states` returns a snapshot instead of None
        # (None makes the base sleep ~1/poll_rate).
        self._state_dirty = threading.Event()

        super().__init__(_FakeTransportDevice(client, self._serial))

    # -- helpers ----------------------------------------------------------

    @classmethod
    def _auto_grid(cls, display_w: int, display_h: int) -> tuple[int, int]:
        """How many native-72px keys fit in ``display_w x display_h`` px.

        Layout assumes a uniform ``STREAMDECK_KEY_GAP`` between cells
        and at the panel edges, so the usable budget is
        ``display - gap`` and the pitch per cell is ``key + gap``. We
        always return at least 1x1 so callers never see a degenerate
        zero-key deck (the firmware always reports a real panel size).
        """
        pitch = cls.STREAMDECK_KEY_PIXELS + cls.STREAMDECK_KEY_GAP
        cols = max(1, (max(0, display_w) - cls.STREAMDECK_KEY_GAP) // pitch)
        rows = max(1, (max(0, display_h) - cls.STREAMDECK_KEY_GAP) // pitch)
        return int(cols), int(rows)

    def _rpc(self, fn: Callable[..., Any], *args: Any, **kw: Any) -> Any:
        """Serialise an RPC against the base class' update_lock.

        Both the read thread (via :meth:`_read_control_states`) and the
        caller's main thread (``set_key_image`` etc.) reach
        :class:`TouchyClient`, which itself isn't internally locked.
        """
        with self.update_lock:
            return fn(*args, **kw)

    # -- StreamDeck abstract methods --------------------------------------

    def _read_control_states(self) -> dict | None:  # noqa: D401 - base-class API
        """Poll one event from the device, update key state, return snapshot.

        Returns a ``{ControlType.KEY: [bool] * KEY_COUNT}`` mapping when
        anything changed since the last call; ``None`` otherwise (which
        makes the base read loop sleep instead of busy-polling).
        """
        try:
            evt = self._rpc(self._client.event_consume)
        except Exception:  # pragma: no cover - device-side I/O failures
            _LOG.exception("touchydeck: event_consume failed")
            return None

        if evt is None:
            # Nothing pending; tell the base loop to sleep ~1/poll_rate.
            return None

        _LOG.debug(
            "touchydeck: event received: host_code=0x%x lv_code=%d widget=%r",
            evt.host_code,
            evt.code,
            evt.user_data,
        )

        # Filter to events from our own key widgets, identified by their
        # host_code range (NOT user_data, so a host-side rename of the
        # widget id can't break this).
        key = _layout.key_for_host_code(evt.host_code)
        if key is None or key >= self.KEY_COUNT:
            return None

        code = evt.code
        if code == _LV_EVENT_PRESSED:
            pressed = True
        elif code in (_LV_EVENT_RELEASED, _LV_EVENT_PRESS_LOST):
            pressed = False
        else:
            # CLICKED / VALUE_CHANGED / etc. — ignored (we only emit
            # press+release for these widgets, but defend against future
            # firmware adding extra edges).
            return None

        if self._key_state[key] == pressed:
            return None
        self._key_state[key] = pressed
        # Snapshot — the base class diffs vs its own last_key_states and
        # fires key_callback on edges, so returning the full list is the
        # contract even when only one cell changed.
        return {ControlType.KEY: list(self._key_state)}

    def _reset_key_stream(self) -> None:  # noqa: D401 - base-class API
        """No-op: Touchy has no streaming-image HID protocol to abort."""
        _LOG.debug("_reset_key_stream: no-op")

    def reset(self) -> None:  # noqa: D401 - base-class API
        """Push the grid screen and clear cached key state."""
        _LOG.debug("reset: pushing %dx%d grid screen", self.KEY_COLS, self.KEY_ROWS)
        screen = _layout.build_screen(cols=self.KEY_COLS, rows=self.KEY_ROWS)
        self._rpc(
            self._client.file_save,
            _layout.SCREEN_PATH,
            screen.to_bytes(),
        )
        self._rpc(self._client.screen_load, _layout.SCREEN_PATH)
        self._key_state = [False] * self.KEY_COUNT
        self._screen_pushed = True

    def set_brightness(self, percent: int) -> None:  # noqa: D401 - base-class API
        """Stash the requested brightness; real backlight control is TBD.

        The Touchy backlight RPC exposes wake/sleep but not a 0-100% PWM
        target yet, so for now we just record the value and log it.
        StreamController-side code that introspects `set_brightness`
        before calling it sees the method as available (matching real
        StreamDecks) without hitting an exception.
        """
        _LOG.debug("set_brightness: %d%%", percent)
        percent = max(0, min(100, int(percent)))
        self._brightness_pct = percent
        if percent == 0:
            # Best-effort screen sleep; ignored if RPC fails.
            try:
                self._rpc(self._client.screen_sleep_timeout, 0)
            except Exception:  # pragma: no cover
                _LOG.debug("touchydeck: screen_sleep_timeout(0) failed", exc_info=True)
        else:
            try:
                self._rpc(self._client.screen_wake)
            except Exception:  # pragma: no cover
                _LOG.debug("touchydeck: screen_wake failed", exc_info=True)

    def set_key_image(self, key: int, image: bytes | None) -> None:  # noqa: D401 - base-class API
        """Upload ``image`` (PNG bytes) to cell ``key`` and refresh the screen.

        ``image=None`` is treated as "clear" — there's no on-device
        delete in the public API yet, so we just skip the upload; the
        existing asset (if any) stays until the next call.
        """
        _LOG.debug("set_key_image: key=%d, image=%s", key, "present" if image else "None")
        if key < 0 or key >= self.KEY_COUNT:
            raise IndexError(f"key {key} out of range (0..{self.KEY_COUNT - 1})")
        if image is None:
            return
        path = _layout.asset_path_for(key)
        # `file_save` auto-converts PNG/JPEG/etc. → LVGL .bin and rewrites
        # the path's extension to .bin, matching the asset path the
        # layout already references.
        self._rpc(self._client.file_save, path, bytes(image))
        # Re-load the screen so LVGL picks up the freshly-written asset.
        # NOTE: this flashes the whole grid. A widget-targeted refresh
        # RPC would be friendlier; tracked separately.
        if not self._screen_pushed:
            self.reset()  # Push the screen layout if we haven't already
        # No longer needed, the device now auto redraws image files that are updated
        # self._rpc(self._client.screen_load, _layout.SCREEN_PATH)

    def set_key_color(self, key: int, r: int, g: int, b: int) -> None:  # noqa: D401 - base-class API
        """Solid-colour key fill — not implemented for Touchy yet."""
        _LOG.error(
            "set_key_color: not implemented (key=%d, rgb=(%d,%d,%d)); "
            "use set_key_image() with a PNG instead",
            key,
            r,
            g,
            b,
        )

    def set_screen_image(self, image: bytes | None) -> None:  # noqa: D401 - base-class API
        _LOG.error(
            "set_screen_image: not implemented; TouchyDeck does not expose a "
            "single full-screen image slot"
        )

    def set_touchscreen_image(  # noqa: D401 - base-class API
        self, image: bytes | None, x_pos: int = 0, y_pos: int = 0, width: int = 0, height: int = 0
    ) -> None:
        _LOG.error(
            "set_touchscreen_image: not implemented (x=%d, y=%d, w=%d, h=%d); "
            "TouchyDeck routes touchscreen pixels through Screen widgets, not the "
            "StreamDeck Plus touchscreen-strip API",
            x_pos,
            y_pos,
            width,
            height,
        )

    def get_firmware_version(self) -> str:  # noqa: D401 - base-class API
        _LOG.debug("get_firmware_version: querying device")
        try:
            v = self._rpc(self._client.sys_board_info_get)
        except Exception:  # pragma: no cover
            return "unknown"
        return getattr(v, "firmware_version_str", "") or str(getattr(v, "firmware_version", 0))

    def get_serial_number(self) -> str:  # noqa: D401 - base-class API
        _LOG.debug("get_serial_number: %s", self._serial)
        return self._serial

    # -- identity --------------------------------------------------------

    def deck_type(self) -> str:
        return self.DECK_TYPE

    def id(self) -> str:
        return self._serial

    # -- friendly repr ---------------------------------------------------

    def __repr__(self) -> str:  # pragma: no cover - cosmetic
        return (
            f"<TouchyDeck serial={self._serial!r} "
            f"keys={self.KEY_COLS}x{self.KEY_ROWS} "
            f"open={self.connected()}>"
        )
