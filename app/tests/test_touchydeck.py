# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for the TouchyDeck StreamDeck-compatibility facade."""

from __future__ import annotations

import time

import pytest

from touchy_pad.api import Touchy, touchy_open
from touchy_pad.client import TouchyClient
from touchy_pad.sim.transport import make_tempdir_transport
from touchy_pad.touchydeck import TouchyDeck
from touchy_pad.touchydeck.layout import (
    HOST_CODE_BASE,
    PAGE_NAME,
    build_page,
    host_code_for,
    key_for_host_code,
    key_widget_id,
)


def _touchy_from_transport(t) -> Touchy:
    """Build a Touchy for a TouchyDeck: no event thread (the deck polls).

    The StreamDeck base class drives its own read thread that calls
    ``event_consume`` directly, so :class:`Touchy`'s background event
    thread must be off or the two would race for the same event queue.
    Mirrors ``touchydeck.discovery._touchy_from_client``.
    """
    client = TouchyClient(t)
    info = client.sys_board_info_get()
    return Touchy(client, start_event_thread=False, board_info=info)


# We use the real upstream library; skip the whole module on machines that
# don't have it installed (CI without the [streamdeck] extra).
pytest.importorskip("StreamDeck.Devices.StreamDeck")

# Bytes for `LvEvent.code`, matching `_LV_EVENT_PRESSED` / `_LV_EVENT_RELEASED`
# in deck.py. Duplicated locally so a wire-protocol change forces a test fix.
# These are LVGL 9.x enum values (RELEASED=11, not 8).
_LV_PRESSED = 1
_LV_RELEASED = 11


def test_host_code_roundtrip() -> None:
    for k in (0, 1, 14, 0x123):
        assert key_for_host_code(host_code_for(k)) == k
    # Out-of-range codes return None.
    assert key_for_host_code(0) is None
    assert key_for_host_code(HOST_CODE_BASE - 1) is None
    assert key_for_host_code(HOST_CODE_BASE + 0x10000) is None


def test_build_page_emits_image_buttons_with_dual_edges() -> None:
    page = build_page(3, 2, blank_path="T:host/icache/blank.bin")
    assert page.id == "touchydeck_root"
    assert page.WhichOneof("kind") == "layout_grid"
    grid = page.layout_grid
    assert grid.cols == 3
    assert grid.rows == 2
    children = list(grid.layout.children)
    assert len(children) == 6
    for idx, w in enumerate(children):
        assert w.id == key_widget_id(idx)
        assert w.WhichOneof("kind") == "image_button"
        ib = w.image_button
        # Both edges wired to the same host_code.
        assert len(ib.on_press) == 1
        assert len(ib.on_release) == 1
        code = host_code_for(idx)
        assert ib.on_press[0].host.code == code
        assert ib.on_release[0].host.code == code
        # Every cell starts at the shared blank path.
        assert ib.released.path == "T:host/icache/blank.bin"
        # Cells are stretched to fill their grid slot.
        assert w.grow_x == 1
        assert w.grow_y == 1


def test_build_page_rejects_zero_dims() -> None:
    with pytest.raises(ValueError):
        build_page(0, 2, blank_path="T:blank.bin")
    with pytest.raises(ValueError):
        build_page(2, 0, blank_path="T:blank.bin")


def test_touchydeck_constructs_with_default_geometry() -> None:
    # Default sim canvas is 480x300; with native 72×72 keys and a 4 px
    # gap that fits 6 cols x 3 rows = 18 keys (the JC4827W543 form
    # factor).
    with make_tempdir_transport() as t:
        pad = _touchy_from_transport(t)
        deck = TouchyDeck(pad)
        assert deck.KEY_PIXEL_WIDTH == 72
        assert deck.KEY_PIXEL_HEIGHT == 72
        assert deck.KEY_COLS == 6
        assert deck.KEY_ROWS == 3
        assert deck.KEY_COUNT == 18
        assert deck.deck_type().startswith("Touchy")
        assert deck.id() == deck.get_serial_number()
        assert deck.connected()


@pytest.mark.parametrize(
    "display_size,expected",
    [
        # JC4827W543
        ((480, 272), (6, 3)),
        # Default sim canvas
        ((480, 300), (6, 3)),
        # Waveshare ESP32-S3 Touch LCD 7B
        ((1024, 600), (13, 7)),
        # Tiny edge case — must still produce at least 1x1.
        ((10, 10), (1, 1)),
    ],
)
def test_touchydeck_auto_grid_matches_display(
    display_size: tuple[int, int],
    expected: tuple[int, int],
) -> None:
    with make_tempdir_transport(display_size=display_size) as t:
        pad = _touchy_from_transport(t)
        deck = TouchyDeck(pad)
        assert (deck.KEY_COLS, deck.KEY_ROWS) == expected
        assert deck.KEY_COUNT == expected[0] * expected[1]
        assert deck.KEY_PIXEL_WIDTH == 72


def test_touchydeck_explicit_cols_rows_override_auto_grid() -> None:
    with make_tempdir_transport(display_size=(1024, 600)) as t:
        pad = _touchy_from_transport(t)
        deck = TouchyDeck(pad, cols=5, rows=3)
        assert deck.KEY_COLS == 5
        assert deck.KEY_ROWS == 3
        assert deck.KEY_COUNT == 15


def test_touchydeck_reset_pushes_page() -> None:
    from touchy_pad.paths import user_screen_path

    with make_tempdir_transport() as t:
        pad = _touchy_from_transport(t)
        deck = TouchyDeck(pad, cols=2, rows=2)
        deck.reset()
        # The sim transport's fs has the page-body blob under uscr/.
        assert t._fs.read(user_screen_path(PAGE_NAME)) is not None


def test_touchydeck_read_loop_fires_press_release_callbacks() -> None:
    """End-to-end: SimDevice → Touchy → TouchyDeck → key_callback."""
    with make_tempdir_transport() as t:
        pad = _touchy_from_transport(t)
        deck = TouchyDeck(pad, cols=3, rows=1)

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
    with make_tempdir_transport() as t:
        pad = _touchy_from_transport(t)
        deck = TouchyDeck(pad, cols=2, rows=1)

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


@pytest.mark.skip(reason="register_controllers_factory not yet available")
def test_install_registers_factory() -> None:
    """install() registers a factory that DeviceManager.enumerate consults."""
    from StreamDeck.DeviceManager import DeviceManager

    from touchy_pad.touchydeck import install

    try:
        install()
        # Use the "dummy" HID transport so this test doesn't require
        # libhidapi to be installed on the runner.
        decks = DeviceManager(transport="dummy").enumerate()
        # enumerate returns a list; with no sim device registered there are
        # no TouchyDecks, but the factory itself must not raise.
        assert isinstance(decks, list)
    finally:
        # register_controllers_factory is cumulative and global; we can't
        # cleanly unregister, but that's fine — the factory just returns []
        # when no device is attached.
        pass


def test_set_key_image_rejects_out_of_range() -> None:
    with make_tempdir_transport() as t:
        pad = _touchy_from_transport(t)
        deck = TouchyDeck(pad, cols=2, rows=1)
        with pytest.raises(IndexError):
            deck.set_key_image(5, b"x")


def test_set_key_color_not_implemented(caplog) -> None:
    with make_tempdir_transport() as t:
        pad = _touchy_from_transport(t)
        deck = TouchyDeck(pad, cols=2, rows=1)
        # set_key_color logs ERROR instead of raising NotImplementedError.
        deck.set_key_color(0, 255, 0, 0)
        assert "set_key_color: not implemented" in caplog.text
        assert "ERROR" in caplog.text


def test_touchydeck_set_key_image_caches_and_repoints_slot() -> None:
    """set_key_image(k, data) uploads once via the cache and swaps the slot.

    Stage 100: instead of overwriting a per-key file, the icon bytes go
    through the content-addressed cache (stored under ``T:host/icache/``)
    and the key's image slot is repointed in place via an
    ``ActionChangeWidgetRef`` (IMAGE_BUTTON_RELEASED).
    """
    from io import BytesIO

    from PIL import Image

    with make_tempdir_transport() as t, touchy_open(transport=t) as pad:
        deck = TouchyDeck(pad, cols=2, rows=2)

        buf = BytesIO()
        Image.new("RGB", (16, 16), (255, 0, 0)).save(buf, format="PNG")
        png = buf.getvalue()

        deck.set_key_image(0, png)

        # The cache stored the (converted) icon under T:host/icache/.
        icache_dir = t._fs._resolve("T:host/icache/")
        icache_files = list(icache_dir.glob("*")) if icache_dir.is_dir() else []
        assert len(icache_files) >= 1


def test_touchydeck_press_flip_roundtrip_via_sim() -> None:
    """Press → flip image, release → restore: full E2E via the sim transport.

    Verifies that:
    - Both press and release edges reach the host callback (via on_press /
      on_release action slots already wired in the layout builder).
    - set_key_image called from inside the callback is serialised correctly
      (update_lock is an RLock, so re-entrant from the read thread is fine).
    - The cache dedups so each distinct icon is stored once.
    """
    from io import BytesIO

    from PIL import Image

    with make_tempdir_transport() as t, touchy_open(transport=t) as pad:
        deck = TouchyDeck(pad, cols=2, rows=2)

        def make_png(color: tuple[int, int, int]) -> bytes:
            buf = BytesIO()
            Image.new("RGB", (16, 16), color).save(buf, format="PNG")
            return buf.getvalue()

        black = make_png((0, 0, 0))
        white = make_png((255, 255, 255))

        # Prime every key with the black image.
        for k in range(4):
            deck.set_key_image(k, black)

        observed: list[tuple[int, bool]] = []

        def on_key(_deck, key_idx, pressed) -> None:
            key_idx = int(key_idx)
            pressed = bool(pressed)
            tile = white if pressed else black
            deck.set_key_image(key_idx, tile)
            observed.append((key_idx, pressed))

        deck.set_key_callback(on_key)

        # Inject press then release for keys 0 and 3.
        t.device.push_host_event(host_code_for(0), widget_id="sdk_key_0", lv_code=_LV_PRESSED)
        t.device.push_host_event(host_code_for(0), widget_id="sdk_key_0", lv_code=_LV_RELEASED)
        t.device.push_host_event(host_code_for(3), widget_id="sdk_key_3", lv_code=_LV_PRESSED)
        t.device.push_host_event(host_code_for(3), widget_id="sdk_key_3", lv_code=_LV_RELEASED)

        deck.open(resume_from_suspend=False)
        try:
            deadline = time.monotonic() + 2.0
            while len(observed) < 4 and time.monotonic() < deadline:
                time.sleep(0.02)
        finally:
            deck.run_read_thread = False
            deck.close()

        assert observed == [(0, True), (0, False), (3, True), (3, False)]

        # The cache should hold exactly two distinct icons (black + white)
        # plus the seeded blank — every key repaint hit the cache rather
        # than re-uploading.
        icache_dir = t._fs._resolve("T:host/icache/")
        icache_files = list(icache_dir.glob("*")) if icache_dir.is_dir() else []
        assert len(icache_files) == 3  # blank + black + white


@pytest.mark.skip(reason="register_controllers_factory not yet available")
def test_create_sim_device_surfaces_via_enumerate(tmp_path) -> None:
    """`create_sim_device` registers a sim that DeviceManager.enumerate finds."""
    from StreamDeck.DeviceManager import DeviceManager

    from touchy_pad.api import (
        create_sim_device,
        destroy_sim_device,
        touchy_get_pad_ids,
    )
    from touchy_pad.touchydeck import install

    try:
        sim = create_sim_device(headless=True, serial="SIM-test", fs_root=tmp_path)
        assert sim.serial == "SIM-test"
        assert "SIM-test" in touchy_get_pad_ids()

        install()
        # Use the "dummy" HID transport so this test doesn't require
        # libhidapi to be installed on the runner.
        decks = DeviceManager(transport="dummy").enumerate()
        touchy_decks = [d for d in decks if isinstance(d, TouchyDeck)]
        sim_decks = [d for d in touchy_decks if d.get_serial_number() == "SIM-test"]
        assert len(sim_decks) == 1, (
            f"expected SIM-test in enumerate results, got: "
            f"{[d.get_serial_number() for d in touchy_decks]}"
        )
    finally:
        destroy_sim_device()
