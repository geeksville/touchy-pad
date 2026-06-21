# SPDX-License-Identifier: GPL-3.0-or-later
"""Host-side dynamic image sources (Stage 87).

An :class:`ImageSource` is a host-side handle to a *live* image asset on
the device. Both :func:`touchy_pad.api.screens.image` and
:func:`~touchy_pad.api.screens.image_button` accept one anywhere they
accept a path ``str``. The source owns a **stable** on-device path
(``T:dyn/<n>.bin`` on the transient :data:`~touchy_pad.paths.TEMP_DRIVE`
drive); calling :meth:`ImageSource.update` re-renders, content-hash
dedups, and rewrites that file. Because the path never changes the
firmware repaints the widget *in place* — no screen rebuild, no lost
touch state.

Typical use::

    from touchy_pad.api import touchy_open
    from touchy_pad.api.screens import image, ImageSource

    with touchy_open() as pad:
        fps = ImageSource(render_fps)        # render_fps() -> PIL.Image | bytes
        pad.user_screen_save("hud", image("fps", asset=fps))
        while running:
            fps.update()                     # push only if the bytes changed

Pass ``every=<seconds>`` and call :meth:`ImageSource.start` for a
background refresh thread that just calls :meth:`update` on a schedule —
the periodic path is pure sugar over the on-demand one. The source is
registered with the open :class:`~touchy_pad.api.device.Touchy` so its
timer is stopped automatically when the device connection closes.
"""

from __future__ import annotations

import hashlib
import itertools
import logging
import threading
from collections.abc import Callable
from typing import TYPE_CHECKING, Union

from ..paths import dynamic_image_path

if TYPE_CHECKING:  # pragma: no cover - typing only
    from PIL.Image import Image as PILImage  # type: ignore[import-not-found]

    from .device import Touchy

_log = logging.getLogger(__name__)

# Process-global monotonic counter: each ImageSource owns one stable
# T:dyn/<n>.bin path, starting at 1 on app launch (mirrors the doc's
# "process-global counter" contract and the Rust ImageCache discipline).
_counter = itertools.count(1)
_counter_lock = threading.Lock()

# Pending registry: sources referenced by an image()/image_button() call
# but not yet bound to a device. Keyed by the source's stable path so the
# owning screen_save can harvest exactly the ones it embeds. Mirrors the
# Stage 67 inline-callback registry in ``_events``.
_pending: dict[str, ImageSource] = {}
_pending_lock = threading.Lock()

#: Accepted in-memory frame types (besides a zero-arg callable producing
#: one of these).
FrameValue = Union[bytes, bytearray, "PILImage"]


def _next_index() -> int:
    with _counter_lock:
        return next(_counter)


def _register_pending(source: ImageSource) -> None:
    with _pending_lock:
        _pending[source.path] = source


def harvest(paths: set[str]) -> dict[str, ImageSource]:
    """Pop and return every pending source whose path is in *paths*.

    Called by :class:`~touchy_pad.api.device.Touchy` when it uploads a
    screen / widget so each embedded :class:`ImageSource` gets bound to
    the device exactly once.
    """
    found: dict[str, ImageSource] = {}
    with _pending_lock:
        for path in paths:
            src = _pending.pop(path, None)
            if src is not None:
                found[path] = src
    return found


def coerce_image_source(value: object) -> ImageSource:
    """Return *value* as an :class:`ImageSource`, wrapping bare frames.

    An :class:`ImageSource` is returned unchanged; a ``PIL.Image`` /
    ``bytes`` / callable is wrapped in a fresh, single-use source.
    """
    if isinstance(value, ImageSource):
        return value
    return ImageSource(value)


class ImageSource:
    """A live, host-driven image asset bound to a stable device path.

    *value* is the initial frame: a ``PIL.Image.Image``, raw image
    ``bytes`` (anything Pillow can decode), or a zero-arg callable
    returning one of those (invoked on every :meth:`update`). It may be
    ``None`` if you only ever push frames via ``update(new=...)``.

    *every* — optional refresh interval in seconds. When set, calling
    :meth:`start` spawns a daemon thread that calls :meth:`update` every
    *every* seconds until :meth:`stop` (or device close).

    *cf*, *max_width*, *max_height* are forwarded to
    :func:`touchy_pad.api.lvgl_image.to_lvgl_bin` for the encode.
    """

    def __init__(
        self,
        value: FrameValue | Callable[[], FrameValue] | None = None,
        *,
        every: float | None = None,
        cf: str | None = None,
        max_width: int | None = None,
        max_height: int | None = None,
    ) -> None:
        if every is not None and every <= 0:
            raise ValueError(f"every must be a positive number of seconds, got {every!r}")
        self._value = value
        self._every = every
        self._cf = cf
        self._max_width = max_width
        self._max_height = max_height
        self._path = dynamic_image_path(_next_index())
        self._pad: Touchy | None = None
        self._last_hash: bytes | None = None
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        _register_pending(self)

    @property
    def path(self) -> str:
        """The stable ``T:dyn/<n>.bin`` device path this source rewrites."""
        return self._path

    # -- rendering ---------------------------------------------------------

    def _resolve_frame(self) -> FrameValue:
        v = self._value
        if callable(v):
            v = v()
        if v is None:
            raise ValueError(
                "ImageSource has no frame to render; construct it with a "
                "value/callable or call update(new=...)"
            )
        return v

    def _encode(self, frame: FrameValue) -> bytes:
        from .lvgl_image import to_lvgl_bin

        return to_lvgl_bin(
            frame,
            cf=self._cf,
            dest_path=self._path,
            max_width=self._max_width,
            max_height=self._max_height,
        )

    # -- the single primitive ---------------------------------------------

    def update(self, new: FrameValue | Callable[[], FrameValue] | None = None) -> bool:
        """Re-render and push the current frame; skip if bytes are unchanged.

        With *new* given, it replaces the stored value/callable first.
        Returns ``True`` if a new bitmap was uploaded, ``False`` if the
        content was identical to the last push (or the source is not yet
        bound to a device — that initial upload happens when the screen
        is saved).
        """
        if new is not None:
            with self._lock:
                self._value = new
        with self._lock:
            pad = self._pad
        if pad is None:
            # Not bound yet: the current value is uploaded when the
            # containing screen/widget is saved.
            return False
        encoded = self._encode(self._resolve_frame())
        digest = hashlib.blake2b(encoded, digest_size=16).digest()
        with self._lock:
            if digest == self._last_hash:
                return False
            self._last_hash = digest
        # The bytes are already LVGL .bin, so file_save passes them
        # through without re-conversion.
        pad.file_save(self._path, encoded)
        _log.debug("ImageSource.update: pushed %d bytes to %s", len(encoded), self._path)
        return True

    # -- periodic sugar ----------------------------------------------------

    def start(self) -> None:
        """Spawn the background refresh thread (requires ``every=``)."""
        if self._every is None:
            raise ValueError("start() requires an `every=` interval")
        with self._lock:
            if self._thread is not None and self._thread.is_alive():
                return
            self._stop.clear()
            self._thread = threading.Thread(
                target=self._run,
                name=f"touchy-img-{self._path}",
                daemon=True,
            )
            self._thread.start()

    def stop(self) -> None:
        """Stop the background refresh thread, if running."""
        self._stop.set()
        with self._lock:
            t = self._thread
            self._thread = None
        if t is not None and t.is_alive() and t is not threading.current_thread():
            t.join(timeout=2.0)

    def _run(self) -> None:
        assert self._every is not None
        while not self._stop.wait(self._every):
            try:
                self.update()
            except Exception:  # noqa: BLE001 — a render error must not kill the loop.
                _log.exception("ImageSource refresh failed for %s", self._path)

    # -- binding (called by Touchy on upload) ------------------------------

    def _bind(self, pad: Touchy) -> None:
        """Attach to *pad*, push the initial frame, and start any timer."""
        with self._lock:
            self._pad = pad
            self._last_hash = None
        if self._value is not None:
            self.update()
        if self._every is not None:
            self.start()
