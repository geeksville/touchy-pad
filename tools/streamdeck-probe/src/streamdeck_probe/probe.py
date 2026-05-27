"""Probe sequence run against each enumerated StreamDeck."""
from __future__ import annotations

import threading
import time
from typing import Any

from PIL import Image, ImageDraw

from .log import ProbeLogger


def _safe(fn, *a, **kw):
    """Call fn(*a, **kw), returning (value, error_repr)."""
    try:
        return fn(*a, **kw), None
    except Exception as e:  # noqa: BLE001 -- we deliberately swallow + log
        return None, f"{type(e).__name__}: {e}"


def _gather_info(deck, log: ProbeLogger, idx: int) -> dict[str, Any]:
    """Read every introspection method that doesn't require the deck open."""
    info: dict[str, Any] = {}
    for name in (
        "deck_type",
        "id",
        "vendor_id",
        "product_id",
        "key_count",
        "key_layout",
        "is_visual",
        "is_touch",
        "dial_count",
        "touch_key_count",
    ):
        if hasattr(deck, name):
            val, err = _safe(getattr(deck, name))
            info[name] = val if err is None else f"<error: {err}>"
    log.log("info", "device_introspection", deck_index=idx, data=info)
    return info


def _gather_formats(deck, log: ProbeLogger, idx: int) -> dict[str, Any]:
    """Read the image-format introspection methods (require deck open on some models)."""
    fmts: dict[str, Any] = {}
    for name in ("key_image_format", "screen_image_format", "touchscreen_image_format"):
        if hasattr(deck, name):
            val, err = _safe(getattr(deck, name))
            fmts[name] = val if err is None else f"<error: {err}>"
    log.log("info", "image_formats", deck_index=idx, data=fmts)
    return fmts


def _make_tile(width: int, height: int, label: str) -> Image.Image:
    """Create a labeled solid-colored PIL image of the right key size."""
    img = Image.new("RGB", (width, height), (40, 80, 160))
    draw = ImageDraw.Draw(img)
    draw.rectangle((0, 0, width - 1, height - 1), outline=(255, 255, 255), width=2)
    # Use default PIL bitmap font; we don't ship TrueType here.
    draw.text((6, 6), label, fill=(255, 255, 255))
    return img


def _tile_native(deck, label: str) -> bytes | None:
    """Return tile bytes in the deck's native key image format, or None on failure."""
    try:
        fmt = deck.key_image_format()
    except Exception:  # noqa: BLE001
        return None
    size = fmt.get("size") if isinstance(fmt, dict) else None
    if not size:
        return None
    w, h = size
    pil = _make_tile(w, h, label)
    # Use the bundled helper if available (handles rotation/flip/format).
    try:
        from StreamDeck.ImageHelpers import PILHelper  # type: ignore
        return PILHelper.to_native_key_format(deck, pil)
    except Exception:  # noqa: BLE001
        return None


def _tile_flipped(deck, label: str) -> bytes | None:
    """Return a 180°-rotated tile in the deck's native format, or None on failure.

    Used by the press-flip phase: the upside-down version of the normal
    tile is uploaded while a key is held and then swapped back on release.
    """
    try:
        fmt = deck.key_image_format()
    except Exception:  # noqa: BLE001
        return None
    size = fmt.get("size") if isinstance(fmt, dict) else None
    if not size:
        return None
    w, h = size
    pil = _make_tile(w, h, label).transpose(Image.ROTATE_180)
    try:
        from StreamDeck.ImageHelpers import PILHelper  # type: ignore
        return PILHelper.to_native_key_format(deck, pil)
    except Exception:  # noqa: BLE001
        return None


def _probe_brightness(deck, log: ProbeLogger, idx: int, pause_ms: int) -> None:
    for pct in (0, 30, 100):
        with log.timed("brightness", "set_brightness", deck_index=idx, percent=pct):
            deck.set_brightness(pct)
        time.sleep(pause_ms / 1000.0)


def _probe_key_images(deck, log: ProbeLogger, idx: int) -> None:
    try:
        key_count = deck.key_count()
    except Exception as e:  # noqa: BLE001
        log.log("set_key_image", "skip_no_key_count", deck_index=idx, data={"error": str(e)})
        return

    # Phase 1: clear all keys with None.
    for k in range(key_count):
        with log.timed("set_key_image", "clear_none", deck_index=idx, key=k):
            deck.set_key_image(k, None)

    # Phase 2: upload generated tiles.
    sample_tile = _tile_native(deck, "0")
    log.log(
        "set_key_image",
        "tile_prepared",
        deck_index=idx,
        data={
            "have_tile": sample_tile is not None,
            "tile_bytes": len(sample_tile) if sample_tile else None,
        },
    )
    if sample_tile is None:
        log.log("set_key_image", "skip_no_tile", deck_index=idx)
        return

    for k in range(key_count):
        tile = _tile_native(deck, str(k))
        with log.timed(
            "set_key_image",
            "set_tile",
            deck_index=idx,
            key=k,
            tile_bytes=len(tile) if tile else None,
        ):
            deck.set_key_image(k, tile)


def _probe_key_color(deck, log: ProbeLogger, idx: int) -> None:
    touch_keys = getattr(deck, "touch_key_count", lambda: 0)()
    if not touch_keys:
        log.log("set_key_color", "skip_no_touch_keys", deck_index=idx)
        return
    base = deck.key_count()
    for offset in range(touch_keys):
        k = base + offset
        with log.timed("set_key_color", "set", deck_index=idx, key=k, rgb=(255, 0, 0)):
            deck.set_key_color(k, 255, 0, 0)


def _probe_screen_image(deck, log: ProbeLogger, idx: int) -> None:
    if not hasattr(deck, "set_screen_image"):
        return
    fmt = None
    if hasattr(deck, "screen_image_format"):
        fmt, _ = _safe(deck.screen_image_format)
    size = fmt.get("size") if isinstance(fmt, dict) else None
    if not size or size[0] <= 0 or size[1] <= 0:
        log.log("set_screen_image", "skip_no_screen", deck_index=idx, data={"format": fmt})
        return
    w, h = size
    pil = _make_tile(w, h, "screen")
    try:
        from StreamDeck.ImageHelpers import PILHelper  # type: ignore
        img_bytes = PILHelper.to_native_screen_format(deck, pil)  # type: ignore[attr-defined]
    except Exception as e:  # noqa: BLE001
        log.log("set_screen_image", "skip_helper_error", deck_index=idx, data={"error": str(e)})
        return
    with log.timed("set_screen_image", "set", deck_index=idx, size=(w, h), bytes=len(img_bytes)):
        deck.set_screen_image(img_bytes)


def _probe_touchscreen(deck, log: ProbeLogger, idx: int) -> None:
    if not hasattr(deck, "set_touchscreen_image"):
        return
    fmt = None
    if hasattr(deck, "touchscreen_image_format"):
        fmt, _ = _safe(deck.touchscreen_image_format)
    size = fmt.get("size") if isinstance(fmt, dict) else None
    if not size or size[0] <= 0 or size[1] <= 0:
        log.log("set_touchscreen_image", "skip_no_touchscreen", deck_index=idx, data={"format": fmt})
        return
    w, h = size
    pil = _make_tile(w, h, "touch")
    try:
        from StreamDeck.ImageHelpers import PILHelper  # type: ignore
        img_bytes = PILHelper.to_native_touchscreen_format(deck, pil)  # type: ignore[attr-defined]
    except Exception as e:  # noqa: BLE001
        log.log("set_touchscreen_image", "skip_helper_error", deck_index=idx, data={"error": str(e)})
        return
    with log.timed("set_touchscreen_image", "set", deck_index=idx, size=(w, h), bytes=len(img_bytes)):
        deck.set_touchscreen_image(img_bytes)


def _probe_callbacks(
    deck,
    log: ProbeLogger,
    idx: int,
    interactive: bool,
    press_timeout: float,
) -> None:
    """Register press/dial/touch callbacks and either wait for input or skip."""
    stop = threading.Event()
    key_events: list[dict[str, Any]] = []

    # Pre-build press-flip tile caches before defining on_key so the
    # closure captures fully-populated lists. Fetching key_count here (rather
    # than inside the interactive block) is safe — it only reads metadata.
    try:
        key_count = deck.key_count()
    except Exception:  # noqa: BLE001
        key_count = 0
    normal_tiles: list[bytes | None] = [
        _tile_native(deck, str(k)) for k in range(key_count)
    ]
    flipped_tiles: list[bytes | None] = [
        _tile_flipped(deck, str(k)) for k in range(key_count)
    ]
    have_flip = any(t is not None for t in flipped_tiles)

    def on_key(_deck, key: int, state: bool) -> None:
        evt = {"ts": time.time(), "key": key, "state": state}
        key_events.append(evt)
        log.log("callback", "key", deck_index=idx, data=evt)
        # Flip the key image upside-down while held; restore on release.
        if not have_flip or key >= len(flipped_tiles):
            return
        tile = flipped_tiles[key] if state else normal_tiles[key]
        if tile is None:
            return
        try:
            with log.timed(
                "set_key_image",
                "flip_on_press" if state else "restore_on_release",
                deck_index=idx,
                key=key,
            ):
                deck.set_key_image(key, tile)
        except Exception as e:  # noqa: BLE001
            log.log(
                "set_key_image",
                "flip_error",
                deck_index=idx,
                key=key,
                data={"error": str(e)},
            )

    def on_dial(_deck, dial: int, evt_type, value) -> None:
        log.log(
            "callback",
            "dial",
            deck_index=idx,
            data={
                "ts": time.time(),
                "dial": dial,
                "type": getattr(evt_type, "name", repr(evt_type)),
                "value": value,
            },
        )

    def on_touch(_deck, evt_type, value) -> None:
        log.log(
            "callback",
            "touchscreen",
            deck_index=idx,
            data={
                "ts": time.time(),
                "type": getattr(evt_type, "name", repr(evt_type)),
                "value": _scrub_touch_value(value),
            },
        )

    if hasattr(deck, "set_key_callback"):
        deck.set_key_callback(on_key)
    if hasattr(deck, "set_dial_callback") and getattr(deck, "dial_count", lambda: 0)() > 0:
        deck.set_dial_callback(on_dial)
    if hasattr(deck, "set_touchscreen_callback") and getattr(deck, "is_touch", lambda: False)():
        deck.set_touchscreen_callback(on_touch)

    if not interactive:
        log.log("callback", "skip_noninteractive", deck_index=idx)
        return

    if have_flip:
        log.log(
            "callback",
            "press_flip_enabled",
            deck_index=idx,
            data={"key_count": key_count},
        )

    print(
        f"\n[deck {idx}] Press any key to see it flip upside-down while held. "
        f"Keys 0..{key_count - 1}. "
        f"Press Ctrl-C or wait {int(press_timeout)}s to continue.\n",
        flush=True,
    )

    deadline = time.time() + press_timeout
    try:
        while time.time() < deadline and not stop.is_set():
            time.sleep(0.25)
    except KeyboardInterrupt:
        log.log("callback", "user_interrupt", deck_index=idx)

    # Restore all keys to their upright tiles so we leave a clean slate.
    if have_flip:
        for k, tile in enumerate(normal_tiles):
            if tile is None:
                continue
            try:
                with log.timed("set_key_image", "restore_all", deck_index=idx, key=k):
                    deck.set_key_image(k, tile)
            except Exception:  # noqa: BLE001
                pass

    log.log(
        "callback",
        "summary",
        deck_index=idx,
        data={"key_event_count": len(key_events)},
    )


def _scrub_touch_value(value: Any) -> Any:
    """Touchscreen value dicts can contain nested types we want to flatten."""
    if isinstance(value, dict):
        return {k: _scrub_touch_value(v) for k, v in value.items()}
    if hasattr(value, "name") and hasattr(value, "value"):
        return value.name
    return value


def probe_deck(
    deck,
    log: ProbeLogger,
    idx: int,
    *,
    interactive: bool,
    brightness_pause_ms: int,
    press_timeout: float,
) -> None:
    """Run the full probe sequence against a single StreamDeck."""
    log.log("info", "begin", deck_index=idx, data={"deck_type": _safe(deck.deck_type)[0]})

    pre_info = _gather_info(deck, log, idx)

    with log.timed("open", "open", deck_index=idx):
        deck.open()
    try:
        # Identity that may need an open device.
        for name in ("get_firmware_version", "get_serial_number"):
            val, err = _safe(getattr(deck, name))
            log.log(
                "info",
                name,
                deck_index=idx,
                data={"value": val, "error": err},
            )

        _gather_formats(deck, log, idx)

        # Lock-vs-no-lock check: does the docs-recommended `with deck:` matter
        # for a single-threaded call? We compare both paths.
        if pre_info.get("is_visual"):
            with log.timed("set_key_image", "single_no_lock", deck_index=idx, key=0):
                deck.set_key_image(0, None)
            with log.timed("set_key_image", "single_with_lock", deck_index=idx, key=0):
                with deck:
                    deck.set_key_image(0, None)

        with log.timed("open", "reset", deck_index=idx):
            deck.reset()

        if pre_info.get("is_visual"):
            _probe_brightness(deck, log, idx, brightness_pause_ms)
            _probe_key_images(deck, log, idx)
            _probe_screen_image(deck, log, idx)
            _probe_touchscreen(deck, log, idx)
            _probe_key_color(deck, log, idx)

        _probe_callbacks(deck, log, idx, interactive=interactive, press_timeout=press_timeout)
    finally:
        # Deregister callbacks BEFORE close. The StreamDeck library's internal
        # poll thread keeps the libusb handle live while a callback is set;
        # tearing down libusb at interpreter exit while that thread is still
        # running causes the "usbi_mutex_destroy: pthread_mutex_destroy(mutex) == 0"
        # assertion crash we saw in early test runs.
        for unset in ("set_key_callback", "set_dial_callback", "set_touchscreen_callback"):
            if hasattr(deck, unset):
                try:
                    getattr(deck, unset)(None)
                except Exception:  # noqa: BLE001 -- best-effort teardown
                    pass
        with log.timed("close", "close", deck_index=idx):
            try:
                deck.close()
            except Exception as e:  # noqa: BLE001
                log.log("close", "error", deck_index=idx, data={"error": str(e)})
        # Give the poll thread time to exit before we drop the reference and
        # let GC trigger libusb teardown.
        time.sleep(0.1)

    log.log("info", "end", deck_index=idx)
