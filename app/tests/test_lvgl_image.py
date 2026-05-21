"""Tests for the host-side LVGL bin converter."""

from __future__ import annotations

import io
import struct

import pytest

from touchy_pad.api.lvgl_image import (
    LVGL_BIN_MAGIC,
    is_lvgl_bin,
    looks_like_supported_image,
    rewrite_to_bin_path,
    to_lvgl_bin,
)

PIL = pytest.importorskip("PIL")
from PIL import Image  # noqa: E402  pillow only required for these tests


def _png_bytes(
    w: int = 4, h: int = 3, color: tuple[int, int, int, int] = (255, 0, 0, 128)
) -> bytes:
    img = Image.new("RGBA", (w, h), color)
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def test_looks_like_supported_image_recognises_png():
    assert looks_like_supported_image(_png_bytes()) is True


def test_looks_like_supported_image_rejects_random_bytes():
    assert looks_like_supported_image(b"\x00\x01\x02\x03 not an image") is False


def test_looks_like_supported_image_rejects_already_lvgl_bin():
    # A valid-looking 12-byte LVGL header should not be re-converted.
    header = struct.pack("<BBHHHHH", LVGL_BIN_MAGIC, 0x14, 0, 1, 1, 2, 0)
    assert is_lvgl_bin(header) is True
    assert looks_like_supported_image(header) is False


def test_to_lvgl_bin_png_to_rgb565a8_layout():
    w, h = 4, 3
    data = to_lvgl_bin(_png_bytes(w, h, color=(255, 0, 0, 128)))
    # 12-byte header + RGB565 (2 bpp) + A8 (1 bpp) = 12 + w*h*3.
    assert len(data) == 12 + w * h * 3
    magic, cf, flags, dw, dh, stride, _resv = struct.unpack("<BBHHHHH", data[:12])
    assert magic == LVGL_BIN_MAGIC
    assert cf == 0x14  # RGB565A8
    assert flags == 0
    assert (dw, dh) == (w, h)
    assert stride == w * 2
    # Every RGB565 word should encode pure red (0xF800, little-endian).
    rgb = data[12 : 12 + w * h * 2]
    for px in range(w * h):
        word = rgb[px * 2] | (rgb[px * 2 + 1] << 8)
        assert word == 0xF800
    # Every alpha byte should match the input.
    alpha = data[12 + w * h * 2 :]
    assert alpha == bytes([128] * (w * h))


def test_to_lvgl_bin_accepts_bytes_and_path(tmp_path):
    raw = _png_bytes(2, 2)
    from_bytes = to_lvgl_bin(raw)
    p = tmp_path / "smile.png"
    p.write_bytes(raw)
    from_path = to_lvgl_bin(p)
    assert from_bytes == from_path


def test_rewrite_to_bin_path_replaces_image_extensions():
    assert rewrite_to_bin_path("images/foo.png") == "images/foo.bin"
    assert rewrite_to_bin_path("images/foo.BMP") == "images/foo.bin"
    assert rewrite_to_bin_path("images/foo.jpeg") == "images/foo.bin"


def test_rewrite_to_bin_path_passes_through_unknown_extensions():
    assert rewrite_to_bin_path("screens/home.pb") == "screens/home.pb"
    assert rewrite_to_bin_path("images/foo.bin") == "images/foo.bin"
    assert rewrite_to_bin_path("notes.txt") == "notes.txt"
