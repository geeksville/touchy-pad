"""Touchy-Pad packaged image assets.

The host package ships small demo images as binary resources. The
firmware-side LVGL build accepts only its own native ``.bin`` format,
so any source image (PNG, JPEG, BMP, GIF, WebP) is converted on the host
by :func:`touchy_pad.lvgl_image.to_lvgl_bin` before upload — see
``client.Client.file_save``.
"""

from __future__ import annotations

from importlib import resources

__all__ = ["make_smiley_png", "make_touchy_png"]


def make_smiley_png() -> bytes:
    """Return the packaged 16×16 yellow smiley as a transparent-background PNG.

    The bytes are read from ``touchy_pad/assets/smiley.png`` (RGBA, with
    fully-transparent pixels outside the face). Used by ``touchy screens
    demo`` as the source for ``/from_host/images/smiley.png``; the host
    converts the PNG to LVGL's native ``.bin`` format on upload.
    """
    return resources.files("touchy_pad.assets").joinpath("smiley.png").read_bytes()


def make_touchy_png() -> bytes:
    """Return the packaged 256×256 Touchy-Pad logo as a PNG.

    The bytes are read from ``touchy_pad/assets/touchy-256.png``. Used by
    ``touchy init`` as the source for ``F:host/images/touchy.png``; the host
    converts it to LVGL's native ``.bin`` format on upload.
    """
    return resources.files("touchy_pad.assets").joinpath("touchy-256.png").read_bytes()
