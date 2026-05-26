"""Host-side conversion of standard image formats to LVGL's native binary.

The firmware always supports LVGL's built-in image decoder, which reads
the 12-byte ``lv_image_header_t`` followed by raw pixel data from any
``F:`` filesystem path. To avoid depending on the optional BMP / PNG /
JPEG decoders on the device, the host package transparently converts
any uploaded image (BMP / PNG / JPEG / GIF / WEBP — anything Pillow can
decode) into the LVGL native ``.bin`` format before sending it.

Output color format is **RGB565** by default (since Stage 53): the
firmware is built with ``CONFIG_LV_COLOR_DEPTH_16`` and RGB565 matches
the display's native format exactly, which lets the device take the
Stage 52 zero-copy mmap fast path on ``R:`` uploads. The converter
auto-falls back to **RGB565A8** only when the source image actually
contains non-opaque alpha — in which case it emits a single WARN log
line so callers can tell why the slow path was taken.

References
----------

* LVGL image format spec: https://docs.lvgl.io/9.3/details/main-modules/image.html#image-format-on-the-binary-files
* `LVGLImage.py` in the LVGL source tree
  (``firmware/managed_components/lvgl__lvgl/scripts/LVGLImage.py``)
  was used as a reference for the binary header layout and the
  RGB565A8 pixel-block layout (RGB565 word per pixel, then a separate
  A8 plane, both row-major with the same stride).
"""

from __future__ import annotations

import io
import logging
import struct
from pathlib import Path

__all__ = [
    "LVGL_BIN_MAGIC",
    "looks_like_supported_image",
    "is_lvgl_bin",
    "rewrite_to_bin_path",
    "to_lvgl_bin",
]


_log = logging.getLogger(__name__)


# LVGL 9 binary magic (first byte of the header). See
# `LVGLImageHeader.binary` in LVGLImage.py.
LVGL_BIN_MAGIC = 0x19

# `enum lv_color_format_t` values from LVGL 9. Only the ones we need.
_CF_RGB565 = 0x12
_CF_RGB565A8 = 0x14
_CF_ARGB8888 = 0x10

# Magic bytes for the formats we recognise as "an image we should
# convert" rather than "a file the user wants to upload verbatim".
_IMAGE_MAGICS: tuple[tuple[bytes, str], ...] = (
    (b"\x89PNG\r\n\x1a\n", "PNG"),
    (b"\xff\xd8\xff", "JPEG"),
    (b"BM", "BMP"),
    (b"GIF87a", "GIF"),
    (b"GIF89a", "GIF"),
    (b"RIFF", "WEBP"),  # WebP starts with RIFF....WEBP
)


def is_lvgl_bin(data: bytes) -> bool:
    """Return True if *data* already looks like an LVGL native ``.bin``."""
    return len(data) >= 12 and data[0] == LVGL_BIN_MAGIC


def looks_like_supported_image(data: bytes) -> bool:
    """Return True if *data* looks like a BMP / PNG / JPEG / GIF / WebP file.

    Used by :meth:`touchy_pad.TouchyClient.file_save` to decide whether
    to transparently route the bytes through :func:`to_lvgl_bin` before
    uploading them to the device. Already-converted LVGL ``.bin`` files
    are passed through unchanged.
    """
    if is_lvgl_bin(data):
        return False
    for magic, _name in _IMAGE_MAGICS:
        if data.startswith(magic):
            return True
    return False


# File extensions we know how to convert to LVGL ``.bin``. Kept lowercase;
# matched case-insensitively via :func:`rewrite_to_bin_path`.
_CONVERTIBLE_EXTS: tuple[str, ...] = (
    ".bmp",
    ".png",
    ".jpg",
    ".jpeg",
    ".gif",
    ".webp",
)


def rewrite_to_bin_path(path: str) -> str:
    """Replace a recognised image extension with ``.bin``.

    LVGL 9's built-in bin decoder selects itself by file extension
    (``lv_fs_get_ext(src) == "bin"``) before sniffing the magic byte,
    so a file converted to LVGL bin format on the host must also be
    *named* ``*.bin`` on the device — otherwise LVGL hands the bytes
    to the BMP / PNG / JPEG decoders, which reject them.

    Both :meth:`touchy_pad.TouchyClient.file_save` and the screen DSL's
    asset-path handling call this helper so an ``asset="images/foo.png"``
    on the host transparently lines up with ``images/foo.bin`` on flash.
    Paths whose extension we don't recognise are returned unchanged.
    """
    lower = path.lower()
    for ext in _CONVERTIBLE_EXTS:
        if lower.endswith(ext):
            return path[: -len(ext)] + ".bin"
    return path


def _build_header(cf: int, w: int, h: int, stride: int) -> bytes:
    """Pack the 12-byte LVGL v9 image header."""
    if w > 0xFFFF or h > 0xFFFF:
        raise ValueError(f"image too large for LVGL bin format: {w}x{h}")
    return struct.pack(
        "<BBHHHHH",
        LVGL_BIN_MAGIC,  # magic
        cf & 0x1F,  # color format (lower 5 bits)
        0,  # flags
        w,
        h,
        stride,
        0,  # reserved
    )


def _to_rgb565a8(image) -> tuple[int, int, bytes]:
    """Encode a PIL image as an RGB565A8 pixel block.

    Returns ``(width, height, pixel_bytes)`` where ``pixel_bytes`` is
    the RGB565 plane (2 bytes/pixel) followed by the A8 plane
    (1 byte/pixel), both in row-major order with no padding between
    rows or planes.
    """
    rgba = image.convert("RGBA")
    w, h = rgba.size
    pixels = rgba.tobytes()  # RGBA8888, row-major, no padding
    rgb_plane = bytearray(w * h * 2)
    a_plane = bytearray(w * h)
    for i in range(w * h):
        r = pixels[i * 4 + 0]
        g = pixels[i * 4 + 1]
        b = pixels[i * 4 + 2]
        a = pixels[i * 4 + 3]
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        rgb_plane[i * 2 + 0] = rgb565 & 0xFF
        rgb_plane[i * 2 + 1] = (rgb565 >> 8) & 0xFF
        a_plane[i] = a
    return w, h, bytes(rgb_plane) + bytes(a_plane)


def _to_rgb565(image) -> tuple[int, int, bytes]:
    """Encode a PIL image as a bare RGB565 pixel block (no alpha).

    Returns ``(width, height, pixel_bytes)`` where ``pixel_bytes`` is
    one RGB565 little-endian word per pixel, row-major.
    """
    rgb = image.convert("RGB")
    w, h = rgb.size
    pixels = rgb.tobytes()  # RGB888, row-major
    out = bytearray(w * h * 2)
    for i in range(w * h):
        r = pixels[i * 3 + 0]
        g = pixels[i * 3 + 1]
        b = pixels[i * 3 + 2]
        word = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[i * 2 + 0] = word & 0xFF
        out[i * 2 + 1] = (word >> 8) & 0xFF
    return w, h, bytes(out)


def _has_non_opaque_alpha(image) -> bool:
    """Return True if *image* actually carries per-pixel transparency.

    A PIL image can be in mode ``RGBA``/``LA``/``PA`` without ever
    using its alpha channel (e.g. a PNG saved by an editor that
    always emits RGBA even for opaque artwork). We treat such images
    as opaque so they can take the RGB565 fast path on-device. Only
    images with at least one pixel where ``alpha < 255`` count as
    needing the RGB565A8 fallback.

    Images without any alpha channel (``RGB``, ``L``, palette-without-
    transparency, …) trivially return False.
    """
    if image.mode in {"RGBA", "LA", "PA"} or (image.mode == "P" and "transparency" in image.info):
        alpha = image.convert("RGBA").split()[-1]
        return alpha.getextrema()[0] < 255
    return False


def to_lvgl_bin(
    source: bytes | str | Path,
    *,
    cf: str | None = None,
    dest_path: str | None = None,
) -> bytes:
    """Convert *source* (image bytes or a path) to an LVGL ``.bin`` blob.

    *cf* selects the output color format. Supported values:

    * ``None`` (default) — auto-pick: emit ``RGB565`` for opaque
      images (fastest on-device, mmap-eligible) and fall back to
      ``RGB565A8`` only when the source actually contains non-opaque
      alpha. A ``logging.WARNING`` is emitted when *dest_path* starts
      with ``R:`` (PSRAM ramdisk), because that is the only filesystem
      that supports the Stage 52 zero-copy mmap fast path; ``F:``
      (flash) never mmaps, so the fallback is silent there.
    * ``"RGB565"`` — force opaque RGB565 (alpha is dropped).
    * ``"RGB565A8"`` — force RGB565 + A8 plane.

    Requires Pillow at runtime; raises :class:`ImportError` with a
    helpful hint otherwise.
    """
    try:
        from PIL import Image  # type: ignore[import-untyped]
    except ImportError as exc:  # pragma: no cover - exercised manually
        raise ImportError(
            "Pillow is required to convert images to LVGL native format. "
            "Install with `pip install pillow` (or `poetry add pillow`)."
        ) from exc

    if isinstance(source, str | Path):
        img = Image.open(source)
    elif isinstance(source, bytes | bytearray | memoryview):
        img = Image.open(io.BytesIO(bytes(source)))
    else:
        raise TypeError(f"unsupported source type: {type(source).__name__}")

    img.load()  # force decoding now so the BytesIO can be GC'd

    chosen = (cf or "").upper()
    if chosen == "":
        # Auto-pick: prefer RGB565; fall back to RGB565A8 on real alpha.
        if _has_non_opaque_alpha(img):
            # Only warn when the destination is R: (PSRAM ramdisk) — that is
            # the only filesystem that supports the zero-copy mmap fast path.
            # F: (flash) never mmaps, so RGB565A8 there is fine silently.
            if dest_path is None or dest_path.upper().startswith("R:"):
                _log.warning(
                    "image has non-opaque alpha (%dx%d, mode=%s); falling back "
                    "to RGB565A8 — this asset will miss the on-device mmap "
                    "fast path",
                    img.width,
                    img.height,
                    img.mode,
                )
            chosen = "RGB565A8"
        else:
            chosen = "RGB565"

    if chosen == "RGB565":
        w, h, pixels = _to_rgb565(img)
        return _build_header(_CF_RGB565, w, h, w * 2) + pixels
    if chosen == "RGB565A8":
        w, h, pixels = _to_rgb565a8(img)
        # RGB565A8 stride convention in LVGL: stride covers the RGB565
        # plane only (2 bytes/pixel). The A8 plane is implicitly the
        # same width.
        return _build_header(_CF_RGB565A8, w, h, w * 2) + pixels
    raise NotImplementedError(f"output color format not supported yet: {cf}")
