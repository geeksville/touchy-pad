# SPDX-License-Identifier: GPL-3.0-or-later
"""Host-side, content-addressed image cache (Stage 100).

Sending image bytes over the USB / UART link is slow, and most
applications (StreamDeck-style key grids in particular) repaint the same
small set of icons over and over. :class:`ImageCache` uploads each
distinct image to the device exactly **once**, keyed by a 128-bit
content hash, and hands back the on-device path so callers can point a
widget at it cheaply (e.g. via :meth:`Touchy.set_image_button_slot`).

The cache is **host-side and volatile**: the in-RAM map is never
serialized, and the device assets live on the ``T:`` transient drive
(a PSRAM ramdisk where available, else a flash scratch area — the device
decides; see ``SysBoardInfoResponse.temp_is_flash``). Build a fresh
:class:`ImageCache` whenever you (re)attach to a device — the first
:meth:`ImageCache.set_cached_image` call clears the cache root on the
device so a crashed prior session leaves no stale files behind.

Eviction is least-recently-used: once :data:`MAX_CACHED_IMAGES` distinct
images are resident, the next miss deletes the least-recently-used asset
before uploading the new one.

This mirrors the Rust crate's ``touchy_pad::image_cache::ImageCache``
(Stage 85/87). The two caches never share files, so they need not use
the same hash function; this module uses stdlib ``hashlib.blake2b`` (no
extra dependency) rather than xxh3.

Typical use::

    from touchy_pad.api import touchy_open, ImageCache

    with touchy_open() as pad:
        cache = ImageCache(pad, max_dim=72)
        path = cache.set_cached_image(icon_bytes)   # uploads once
        pad.set_image_button_slot("key_0", False, path)
"""

from __future__ import annotations

import base64
import hashlib
import logging
from collections import OrderedDict
from typing import TYPE_CHECKING, Union

from ..paths import IMAGE_CACHE_DIR

if TYPE_CHECKING:  # pragma: no cover - typing only
    from PIL.Image import Image as PILImage  # type: ignore[import-not-found]

    from .device import Touchy

_log = logging.getLogger(__name__)

#: On-device directory holding cached image assets. Lives on the ``T:``
#: transient drive (PSRAM ramdisk where available, else a flash scratch
#: area) — wiped on device reboot.
CACHE_ROOT = IMAGE_CACHE_DIR

#: Maximum number of distinct images kept resident on the device before
#: the least-recently-used one is evicted.
MAX_CACHED_IMAGES = 128

#: Accepted input types for :meth:`ImageCache.set_cached_image`.
ImageData = Union[bytes, bytearray, memoryview, "PILImage"]


def _hash_name(data: bytes) -> str:
    """Return the cache filename stem for *data* (128-bit blake2b, urlsafe b64)."""
    digest = hashlib.blake2b(data, digest_size=16).digest()
    return base64.urlsafe_b64encode(digest).rstrip(b"=").decode("ascii")


class ImageCache:
    """Content-addressed image cache for one device.

    Construct one per attached device. ``max_dim`` optionally downscales
    every cached image so neither dimension exceeds it (aspect ratio
    preserved) — use this when the images back a fixed-size widget (e.g.
    a StreamDeck key) so the device stores and blits them at display size
    instead of rescaling an oversized source every frame.

    The cache is **not** thread-safe; serialise calls if you share one
    across threads, or build one per thread.
    """

    def __init__(self, pad: Touchy, *, max_dim: int | None = None) -> None:
        self._pad = pad
        self._max_dim = max_dim
        # hash -> on-device asset path, ordered LRU (front) → MRU (back).
        self._entries: OrderedDict[str, str] = OrderedDict()
        self._initialized = False

    # -- the single primitive ---------------------------------------------

    def set_cached_image(self, data: ImageData) -> str:
        """Ensure *data* is resident in the device cache; return its path.

        Returns the drive-prefixed path to the asset file (an LVGL
        ``.bin``), suitable as the ``released``/``pressed`` image path of
        an :func:`~touchy_pad.api.screens.image_button`. *data* may be PNG
        / JPEG / BMP / GIF / WebP, an already-encoded LVGL ``.bin``, raw
        ``bytes`` of any of those, or a ``PIL.Image``. Bytes are
        normalised exactly as
        :meth:`~touchy_pad.api.Touchy.file_save` would, then hashed;
        identical inputs hit the cache and upload nothing.
        """
        normalized, suffix = self._normalize(data)
        key = _hash_name(normalized)

        # One-time wipe of any stale assets from a prior host session.
        if not self._initialized:
            # Best-effort: the directory may simply not exist yet.
            try:
                self._pad.file_delete(CACHE_ROOT.rstrip("/"))
            except Exception:  # noqa: BLE001
                _log.debug("cache root wipe of %s failed", CACHE_ROOT, exc_info=True)
            self._initialized = True

        # Cache hit — no device I/O.
        if key in self._entries:
            self._entries.move_to_end(key)
            path = self._entries[key]
            _log.debug("image cache hit %s -> %s", key, path)
            return path

        # Miss — evict LRU if at capacity.
        if len(self._entries) >= MAX_CACHED_IMAGES:
            old_key, old_path = self._entries.popitem(last=False)
            _log.debug("image cache evict %s (%s)", old_key, old_path)
            try:
                self._pad.file_delete(old_path)
            except Exception:  # noqa: BLE001
                _log.debug("evict delete of %s failed", old_path, exc_info=True)

        # Upload the (already-normalised) bytes verbatim. Use file_write_raw
        # semantics: the bytes are already a device .bin, so we must bypass
        # file_save's PNG→.bin auto-conversion. We encode to a .bin path.
        path = f"{CACHE_ROOT}{key}{suffix}"
        _log.debug(
            "image cache miss %s -> uploading %s (%d bytes)",
            key,
            path,
            len(normalized),
        )
        self._pad.file_save(path, normalized)
        self._entries[key] = path
        return path

    # -- introspection ----------------------------------------------------

    def __len__(self) -> int:
        """Number of images currently resident in the cache."""
        return len(self._entries)

    def clear(self) -> None:
        """Drop the in-RAM index. Does **not** delete device files.

        To also wipe the device directory, drop this cache and build a
        fresh one (its first :meth:`set_cached_image` wipes the root).
        """
        self._entries.clear()
        # Force the next set_cached_image to re-wipe the device root, so a
        # cleared cache and a fresh cache behave identically.
        self._initialized = False

    # -- internal ---------------------------------------------------------

    def _normalize(self, data: ImageData) -> tuple[bytes, str]:
        """Normalise *data* to device-ready bytes and return ``(bytes, suffix)``.

        Mirrors the conversion logic in
        :meth:`~touchy_pad.client.TouchyClient.file_save` so that a cached
        asset is byte-identical to a direct ``file_save`` of the same
        input (and thus interchangeable as a widget image path):

        * a ``PIL.Image`` is PNG-encoded first, then treated as PNG bytes;
        * a GIF is passed through verbatim with a ``.gif`` suffix (the
          firmware's native ``lv_gif`` decoder renders it);
        * an already-encoded LVGL ``.bin`` is passed through with a
          ``.bin`` suffix;
        * any other supported image (PNG / JPEG / BMP / WebP) is converted
          via :func:`to_lvgl_bin` and gets a ``.bin`` suffix;
        * anything else is stored verbatim with a ``.bin`` suffix.

        ``max_dim`` (from the constructor) is applied as the
        ``max_width`` / ``max_height`` of the conversion so cached icons
        are pre-scaled to the widget size.
        """
        from .lvgl_image import is_gif, is_lvgl_bin, looks_like_supported_image, to_lvgl_bin

        # Coerce a PIL.Image to PNG bytes so the detectors below work.
        if hasattr(data, "save"):  # PIL.Image
            from io import BytesIO

            buf = BytesIO()
            data.save(buf, format="PNG")  # type: ignore[union-attr]
            raw = buf.getvalue()
        else:
            raw = bytes(data)

        if is_gif(raw):
            return raw, ".gif"
        if is_lvgl_bin(raw):
            return raw, ".bin"
        if looks_like_supported_image(raw):
            bin_bytes = to_lvgl_bin(
                raw,
                dest_path=CACHE_ROOT,
                max_width=self._max_dim,
                max_height=self._max_dim,
            )
            return bin_bytes, ".bin"
        return raw, ".bin"
