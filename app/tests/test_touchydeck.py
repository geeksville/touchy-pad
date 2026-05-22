# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for the TouchyDeck StreamDeck-compatibility facade."""

from __future__ import annotations

import time

import pytest

from touchy_pad import TouchyClient
from touchy_pad.sim.transport import make_tempdir_transport
from touchy_pad.touchydeck import TouchyDeck
from touchy_pad.touchydeck.layout import (
    HOST_CODE_BASE,
    SCREEN_NAME,
    asset_path_for,
    build_screen,
    host_code_for,
    key_for_host_code,
)

# We use the real upstream library; skip the whole module on machines that
# don't have it installed (CI without the [streamdeck] extra).
pytest.importorskip("StreamDeck.Devices.StreamDeck")

# Bytes for `LvEvent.code`, matching `_LV_EVENT_PRESSED` / `_LV_EVENT_RELEASED`
# in deck.py. Duplicated locally so a wire-protocol change forces a test fix.
_LV_PRESSED = 1
_LV_RELEASED = 8


def test_host_code_roundtrip() -> None:
    for k in (0, 1, 14, 0x123):
        assert key_for_host_code(host_code_for(k)) == k
    # Out-of-range codes return None.
    assert key_for_host_code(0) is None
    assert key_for_host_code(HOST_CODE_BASE - 1) is None
    assert key_for_host_code(HOST_CODE_BASE + 0x10000) is None


def test_build_screen_emits_image_buttons_with_dual_edges() -> None:
    screen = build_screen(cols=3, rows=2)
    assert screen.name == SCREEN_NAME
    widgets = list(screen.widgets)
    assert len(widgets) == 6
    for idx, w in enumerate(widgets):
        assert w.id == f"sdk_key_{idx}"
        assert w.WhichOneof("kind") == "image_button"
        ib = w.image_button
        # Both edges wired to the same host_code.
        assert len(ib.on_press) == 1
        assert len(ib.on_release) == 1
        code = host_code_for(idx)
        assert ib.on_press[0].host.code == code
        assert ib.on_release[0].host.code == code
        # Asset path matches the convention TouchyDeck.set_key_image uses.
        # Note _fill_image rewrites the .png suffix; here we already pass .bin.
        assert ib.released.asset == asset_path_for(idx)


def test_touchydeck_constructs_with_default_geometry() -> None:
    with make_tempdir_transport() as t, TouchyClient(t) as c:
        deck = TouchyDeck(c, cols=5, rows=3)
        assert deck.KEY_COUNT == 15
        assert deck.KEY_COLS == 5
        assert deck.KEY_ROWS == 3
        assert deck.deck_type().startswith("Touchy")
        assert deck.id() == deck.get_serial_number()
        assert deck.connected()


def test_touchydeck_reset_pushes_screen() -> None:
    with make_tempdir_transport() as t, TouchyClient(t) as c:
        deck = TouchyDeck(c, cols=2, rows=2)
        deck.reset()
        # The sim transport's fs has the screen blob.
        path = f"screens/{SCREEN_NAME}.pb"
        assert t._fs.read(path) is not None


def test_touchydeck_read_loop_fires_press_release_callbacks() -> None:
    """End-to-end: SimDevice → TouchyClient → TouchyDeck → key_callback."""
    with make_tempdir_transport() as t, TouchyClient(t) as c:
        deck = TouchyDeck(c, cols=3, rows=1)

        events: list[tuple[int, bool]] = []

        def on_key(_deck, key_idx, pressed):
            events.append((int(key_idx), bool(pressed)))

        deck.set_key_callback(on_key)

        # Push events into the SimDevice queue as if firmware emitted them.
        # Press key 1, then release key 1, then press key 0.
        t.device.push_host_event(host_code_for(1), widget_id="sdk_key_1", lv_code=_LV_PRESSED)
        t.device.push_host_event(host_code_for(1), widget_id="sdk_key_1", lv_code=_LV_RELEASED)
        t.device.push_host_event(host_code_for(0), widget_id="sdk_key_0", lv_code=_LV_PRESSED)

        # Start the base-class read thread. Polls at ~20Hz; wait up to 2s
        # for it to drain the 3-event queue.
        deck.open(resume_from_suspend=False)
        try:
            deadline = time.monotonic() + 2.0
            while len(events) < 3 and time.monotonic() < deadline:
                time.sleep(0.02)
        finally:
            deck.run_read_thread = False
            deck.close()

        assert events == [(1, True), (1, False), (0, True)]


def test_touchydeck_ignores_unrelated_host_codes() -> None:
    """Events outside the TouchyDeck host-code range mustn't perturb keys."""
    with make_tempdir_transport() as t, TouchyClient(t) as c:
        deck = TouchyDeck(c, cols=2, rows=1)

        events: list[tuple[int, bool]] = []
        deck.set_key_callback(lambda _d, k, p: events.append((int(k), bool(p))))

        # 0x100 is the demo-screen host_code range; well outside HOST_CODE_BASE.
        t.device.push_host_event(0x100, widget_id="other", lv_code=_LV_PRESSED)
        # Plus one legitimate edge so we know the loop is alive.
        t.device.push_host_event(host_code_for(0), widget_id="sdk_key_0", lv_code=_LV_PRESSED)

        deck.open(resume_from_suspend=False)
        try:
            deadline = time.monotonic() + 2.0
            while not events and time.monotonic() < deadline:
                time.sleep(0.02)
        finally:
            deck.run_read_thread = False
            deck.close()

        assert events == [(0, True)]


def test_install_uninstall_idempotent() -> None:
    from StreamDeck.DeviceManager import DeviceManager

    from touchy_pad.touchydeck import install, uninstall

    original = DeviceManager.enumerate
    try:
        install()
        install()  # no-op
        assert DeviceManager.enumerate is not original
    finally:
        uninstall()
        uninstall()  # no-op
    assert DeviceManager.enumerate is original


def test_set_key_image_rejects_out_of_range() -> None:
    with make_tempdir_transport() as t, TouchyClient(t) as c:
        deck = TouchyDeck(c, cols=2, rows=1)
        with pytest.raises(IndexError):
            deck.set_key_image(5, b"x")


def test_set_key_color_not_implemented() -> None:
    with make_tempdir_transport() as t, TouchyClient(t) as c:
        deck = TouchyDeck(c, cols=2, rows=1)
        with pytest.raises(NotImplementedError):
            deck.set_key_color(0, 255, 0, 0)
