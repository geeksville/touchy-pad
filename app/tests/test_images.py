"""Tests for the packaged image assets used by Stage 20."""

from __future__ import annotations

from touchy_pad.images import make_smiley_png


def test_make_smiley_png_is_a_transparent_rgba_png():
    blob = make_smiley_png()

    # PNG magic.
    assert blob[:8] == b"\x89PNG\r\n\x1a\n"

    # IHDR is the first chunk; bytes 8..12 = chunk length, 12..16 = type.
    assert blob[12:16] == b"IHDR"
    width = int.from_bytes(blob[16:20], "big")
    height = int.from_bytes(blob[20:24], "big")
    bit_depth = blob[24]
    color_type = blob[25]

    assert (width, height) == (16, 16)
    assert bit_depth == 8
    # 6 = truecolor + alpha (RGBA), required for the transparent
    # background the demo relies on.
    assert color_type == 6
