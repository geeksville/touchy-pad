"""Tests for the host-side LVGL bin converter."""

from __future__ import annotations

import io
import struct

import pytest

from touchy_pad.api.lvgl_image import (
    LVGL_BIN_MAGIC,
    is_gif,
    is_lvgl_bin,
    looks_like_supported_image,
    rescale_gif,
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


def _gif_bytes(w: int = 8, h: int = 6, frames: int = 3) -> bytes:
    imgs = []
    for i in range(frames):
        im = Image.new("P", (w, h), color=i % 2)
        # Distinct palette so frames don't collapse to one on save.
        im.putpalette([0, 0, 0, 255, 0, 0] + [0] * (256 * 3 - 6))
        imgs.append(im)
    buf = io.BytesIO()
    imgs[0].save(
        buf,
        format="GIF",
        save_all=True,
        append_images=imgs[1:],
        duration=100,
        loop=0,
    )
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


def test_gif_is_a_supported_image_but_keeps_its_extension():
    gif = _gif_bytes()
    assert is_gif(gif) is True
    assert looks_like_supported_image(gif) is True
    # Stage 80: GIFs are NOT rewritten to .bin — the firmware's lv_gif
    # decoder is selected by the .gif suffix.
    assert rewrite_to_bin_path("F:host/images/bg.gif") == "F:host/images/bg.gif"
    assert rewrite_to_bin_path("F:host/images/bg.GIF") == "F:host/images/bg.GIF"


def test_is_gif_rejects_non_gif():
    assert is_gif(_png_bytes()) is False
    assert is_gif(b"not a gif") is False


def test_rescale_gif_shrinks_frames_preserving_animation():
    gif = _gif_bytes(w=40, h=30, frames=3)
    out = rescale_gif(gif, max_width=10, max_height=10)
    assert is_gif(out) is True
    img = Image.open(io.BytesIO(out))
    assert img.width <= 10 and img.height <= 10
    # Aspect ratio preserved (40x30 -> 10x7/8) and animation intact.
    assert getattr(img, "n_frames", 1) == 3


def test_rescale_gif_noop_when_within_limits():
    gif = _gif_bytes(w=8, h=6)
    assert rescale_gif(gif, max_width=180, max_height=180) is gif
    assert rescale_gif(gif) is gif


def test_to_lvgl_bin_auto_picks_rgb565_for_opaque_png(caplog):
    w, h = 4, 3
    # Fully opaque (alpha=255) — Stage 53 should auto-pick RGB565.
    with caplog.at_level("WARNING", logger="touchy_pad.api.lvgl_image"):
        data = to_lvgl_bin(_png_bytes(w, h, color=(0, 255, 0, 255)))
    assert len(data) == 12 + w * h * 2
    magic, cf, _flags, dw, dh, stride, _resv = struct.unpack("<BBHHHHH", data[:12])
    assert magic == LVGL_BIN_MAGIC
    assert cf == 0x12  # RGB565
    assert (dw, dh) == (w, h)
    assert stride == w * 2
    # Pure green in RGB565 is 0x07E0 (little-endian).
    pixels = data[12:]
    for i in range(w * h):
        word = pixels[i * 2] | (pixels[i * 2 + 1] << 8)
        assert word == 0x07E0
    # No WARN should fire on the opaque fast path.
    assert not [r for r in caplog.records if r.levelname == "WARNING"]


def test_to_lvgl_bin_auto_falls_back_to_rgb565a8_on_real_alpha(caplog):
    # Non-opaque alpha → must use RGB565A8 and emit a WARN.
    with caplog.at_level("WARNING", logger="touchy_pad.api.lvgl_image"):
        data = to_lvgl_bin(_png_bytes(2, 2, color=(255, 0, 0, 128)))
    cf = struct.unpack("<BBHHHHH", data[:12])[1]
    assert cf == 0x14  # RGB565A8
    warns = [r for r in caplog.records if r.levelname == "WARNING"]
    assert len(warns) == 1
    assert "non-opaque alpha" in warns[0].getMessage()


def test_to_lvgl_bin_explicit_cf_overrides_auto():
    raw = _png_bytes(2, 2, color=(255, 0, 0, 255))  # opaque
    forced = to_lvgl_bin(raw, cf="RGB565A8")
    assert struct.unpack("<BBHHHHH", forced[:12])[1] == 0x14
    auto = to_lvgl_bin(raw)
    assert struct.unpack("<BBHHHHH", auto[:12])[1] == 0x12
