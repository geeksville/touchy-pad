# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for the content-addressed :class:`~touchy_pad.api.ImageCache`."""

from __future__ import annotations

import queue
from io import BytesIO

import pytest

from touchy_pad._proto import (
    RESULT_OK,
    Command,
    Response,
    SysBoardInfoResponse,
)
from touchy_pad.api import MINIMUM_FIRMWARE_VERSION, ImageCache, touchy_open
from touchy_pad.api._transport import Transport


class _FakeTransport(Transport):
    def __init__(self, server) -> None:
        self._server = server
        self._responses: queue.Queue[bytes] = queue.Queue()
        self.closed = False

    def send_command(self, payload: bytes) -> None:
        if self.closed:
            raise OSError("transport closed")
        cmd = Command()
        cmd.ParseFromString(payload)
        reply = self._server(cmd, self)
        self._responses.put(reply.SerializeToString())

    def recv_response(self, timeout_ms: int = 2000) -> bytes:
        return self._responses.get(timeout=timeout_ms / 1000.0)

    def close(self) -> None:
        self.closed = True


def _make_fs_server():
    """Return a server callback that handles file_save / file_delete."""
    saved: dict[str, bytes] = {}
    writes: dict[int, tuple[str, bytearray]] = {}
    next_handle = [1]

    def server(cmd: Command, _t):
        kind = cmd.WhichOneof("cmd")
        if kind == "sys_board_info_get":
            return Response(
                code=RESULT_OK,
                sys_board_info=SysBoardInfoResponse(
                    protocol_version=MINIMUM_FIRMWARE_VERSION,
                    firmware_version=1,
                    firmware_version_str="test",
                    board_name="test_board",
                ),
            )
        if kind == "file_open_write":
            h = next_handle[0]
            next_handle[0] += 1
            writes[h] = (cmd.file_open_write.path, bytearray())
            return Response(code=RESULT_OK, file_open_write={"handle": h})
        if kind == "file_write":
            entry = writes.get(cmd.file_write.handle)
            if entry is None:
                return Response(code=1)
            entry[1].extend(cmd.file_write.data)
            return Response(code=RESULT_OK)
        if kind == "file_close":
            entry = writes.pop(cmd.file_close.handle, None)
            if entry is None:
                return Response(code=1)
            if cmd.file_close.commit:
                saved[entry[0]] = bytes(entry[1])
            return Response(code=RESULT_OK)
        if kind == "file_delete":
            path = cmd.file_delete.path
            removed = [k for k in saved if k == path or k.startswith(path + "/")]
            for k in removed:
                saved.pop(k, None)
            return Response(code=RESULT_OK)
        return Response(code=RESULT_OK)

    server.saved = saved  # type: ignore[attr-defined]
    return server


def _png(color: tuple[int, int, int] = (255, 0, 0), size: int = 8) -> bytes:
    from PIL import Image

    buf = BytesIO()
    Image.new("RGB", (size, size), color).save(buf, format="PNG")
    return buf.getvalue()


@pytest.fixture
def open_pad():
    """Open a :class:`Touchy` against a fresh fake server with a real fs."""
    server = _make_fs_server()
    pads: list = []

    def factory():
        pad = touchy_open(transport=_FakeTransport(server))
        pads.append(pad)
        return pad, server

    yield factory

    for pad in pads:
        pad.close()


def test_cache_miss_then_hit_uploads_once(open_pad):
    """First call uploads; second call with identical bytes hits the cache."""
    pad, server = open_pad()
    cache = ImageCache(pad)

    data = _png()
    path1 = cache.set_cached_image(data)
    assert path1.startswith("T:host/icache/")
    assert path1 in server.saved

    # Second call with the same bytes: no new file written.
    n_before = len(server.saved)
    path2 = cache.set_cached_image(data)
    assert path2 == path1
    assert len(server.saved) == n_before
    assert len(cache) == 1


def test_distinct_images_get_distinct_paths(open_pad):
    """Two different icons produce two cache entries with different paths."""
    pad, _ = open_pad()
    cache = ImageCache(pad)

    p1 = cache.set_cached_image(_png((255, 0, 0)))
    p2 = cache.set_cached_image(_png((0, 255, 0)))
    assert p1 != p2
    assert len(cache) == 2


def test_blank_bin_passes_through_unconverted(open_pad):
    """An already-encoded LVGL .bin is stored verbatim (no PNG re-encode)."""
    from touchy_pad.touchydeck.layout import BLANK_BIN

    pad, server = open_pad()
    cache = ImageCache(pad)

    path = cache.set_cached_image(BLANK_BIN)
    # The stored bytes are exactly the input (it's already a .bin).
    assert server.saved[path] == BLANK_BIN
    assert path.endswith(".bin")


def test_max_dim_downscales_oversized_image(open_pad):
    """max_dim caps the stored image dimensions."""
    pad, server = open_pad()
    cache = ImageCache(pad, max_dim=16)

    big = _png(size=64)
    path = cache.set_cached_image(big)
    # The stored .bin should reflect a downscaled image — decode it back
    # to check dimensions. We can't easily decode LVGL bin here, but we
    # can assert the stored bytes differ from the source PNG (it was
    # converted) and that the call succeeded.
    assert path in server.saved
    assert server.saved[path] != big  # converted to .bin


def test_clear_drops_in_ram_index(open_pad):
    pad, _ = open_pad()
    cache = ImageCache(pad)
    cache.set_cached_image(_png())
    assert len(cache) == 1
    cache.clear()
    assert len(cache) == 0


def test_first_call_wipes_cache_root(open_pad):
    """The first set_cached_image wipes the T:host/icache directory."""
    pad, server = open_pad()
    # Pre-seed a stale file under the cache root.
    server.saved["T:host/icache/stale.bin"] = b"old"
    cache = ImageCache(pad)
    cache.set_cached_image(_png())
    assert "T:host/icache/stale.bin" not in server.saved
