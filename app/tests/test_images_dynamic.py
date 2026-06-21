# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for the Stage 87 dynamic image sources + ``T:`` transient drive."""

from __future__ import annotations

import re

import pytest

from touchy_pad import TouchyClient
from touchy_pad.api import touchy_open
from touchy_pad.api.screens import ImageSource, image, image_button
from touchy_pad.sim.transport import make_tempdir_transport

Image = pytest.importorskip("PIL.Image")


def _img(color: str):
    return Image.new("RGB", (8, 8), color)


# -- path allocation --------------------------------------------------------


def test_path_is_stable_and_monotonic() -> None:
    a = ImageSource(_img("red"))
    b = ImageSource(_img("blue"))
    assert re.fullmatch(r"T:dyn/\d+\.bin", a.path)
    assert a.path == a.path  # stable for the life of the source
    na = int(a.path.split("/")[1].split(".")[0])
    nb = int(b.path.split("/")[1].split(".")[0])
    assert nb > na  # process-global monotonic counter


def test_rejects_non_positive_interval() -> None:
    with pytest.raises(ValueError):
        ImageSource(_img("red"), every=0)


# -- simulator T: drive -----------------------------------------------------


def test_simfs_round_trips_temp_drive(tmp_path) -> None:
    from touchy_pad.sim.fs import SimFs

    fs = SimFs(tmp_path, "sim")
    fs.save("T:dyn/1.bin", b"hello")
    assert fs.exists("T:dyn/1.bin")
    assert fs.read("T:dyn/1.bin") == b"hello"


def test_board_info_reports_temp_is_flash_false() -> None:
    with make_tempdir_transport() as t:
        v = TouchyClient(t).sys_board_info_get()
    # The sim advertises PSRAM, so its T: drive is ramdisk-backed.
    assert v.temp_is_flash is False
    assert v.protocol_version >= 10


# -- end-to-end upload + dedup ----------------------------------------------


def test_image_source_uploads_initial_frame_and_dedups() -> None:
    with make_tempdir_transport() as t:
        dev = t._device  # noqa: SLF001 — white-box fs inspection
        with touchy_open(transport=t) as pad:
            src = ImageSource(_img("red"))
            pad.user_screen_save("hud", image("fps", asset=src))
            # Saving the screen pushed the initial frame to the stable path.
            assert dev._fs.exists(src.path)  # noqa: SLF001
            first = dev._fs.read(src.path)  # noqa: SLF001
            # An identical frame is not re-uploaded.
            assert src.update() is False
            # A changed frame is uploaded.
            assert src.update(_img("blue")) is True
            second = dev._fs.read(src.path)  # noqa: SLF001
            assert second != first


def test_temp_rewrite_fires_image_update_callback() -> None:
    with make_tempdir_transport() as t:
        dev = t._device  # noqa: SLF001
        seen: list[str] = []
        dev.set_image_update_callback(seen.append)
        with touchy_open(transport=t) as pad:
            src = ImageSource(_img("red"))
            pad.user_screen_save("hud", image("fps", asset=src))
            seen.clear()  # drop the initial bind upload
            assert src.update(_img("green")) is True
        assert src.path in seen


def test_image_button_slots_take_distinct_sources() -> None:
    released = ImageSource(_img("red"))
    pressed = ImageSource(_img("blue"))
    w = image_button("k", asset=released, pressed_asset=pressed)
    assert w.image_button.released.path == released.path
    assert w.image_button.pressed.path == pressed.path
    assert released.path != pressed.path


def test_bare_pil_image_is_auto_wrapped_and_uploaded() -> None:
    with make_tempdir_transport() as t:
        dev = t._device  # noqa: SLF001
        with touchy_open(transport=t) as pad:
            w = image("logo", asset=_img("red"))
            assert re.fullmatch(r"T:dyn/\d+\.bin", w.image.path)
            pad.user_screen_save("hud", w)
            assert dev._fs.exists(w.image.path)  # noqa: SLF001


def test_unbound_update_is_noop() -> None:
    # Before the screen is saved the source is not bound to a device.
    src = ImageSource(_img("red"))
    assert src.update() is False
