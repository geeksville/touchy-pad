"""USB enumeration helpers shared by the client, the updater and the CLI.

**Why this exists.** pyusb keeps exactly *one* libusb context per process
(``usb.backend.libusb1.get_backend()`` caches a module-level singleton), and
libusb >= 1.0.27 caches the device list *in that context*. So a long-lived
process never sees a device that appeared after its first enumeration: the
kernel's sysfs shows it, and a fresh process sees it, but the already-running
one does not. Measured here on libusb 1.0.27 with a Touchy-Pad plugged in after
``just run`` had already enumerated:

    this process:  total=21  touchy=NOT FOUND      # stale context
    fresh process: total=22  touchy=['005/036']    # new context

That is exactly the case this project cares about — start the app (googly-vr,
StreamController, the OpenDeck plugin) first and plug the pad in later, or
unplug and re-plug it while it runs — and it is also why
``touchy_pad.update._bootloader_visible()`` scans sysfs before calling pyusb.

**What we do about it.** Enumerate through the process-wide context first (the
common path, unchanged), and only when that comes up empty retry once with a
*new* libusb context, which is guaranteed to be current. The fresh context is
pinned to the returned devices so it outlives the handle opened from them.
"""

from __future__ import annotations

from typing import Any


def _as_list(found: Any, find_all: bool) -> list[Any]:
    """Normalise pyusb's ``find()`` result.

    ``find_all=False`` returns a single ``Device`` — which is *iterable* (over
    its configurations), so it must not be ``list()``-ed.
    """
    if found is None:
        return []
    return list(found) if find_all else [found]


def _fresh_backend() -> Any | None:
    """A brand-new libusb context, or ``None`` when libusb isn't usable here."""
    try:
        from usb.backend import libusb1
    except ImportError:  # pragma: no cover — pyusb is a hard dependency
        return None
    if libusb1._lib is None:
        libusb1.get_backend()  # load the shared library so `_lib` is populated
    if libusb1._lib is None:  # e.g. Windows/macOS without libusb installed
        return None
    try:
        return libusb1._LibUSB(libusb1._lib)  # libusb_init() → fresh context
    except Exception:  # noqa: BLE001 — no libusb context, so no devices
        return None


def find_usb_devices(
    usb_core: Any,
    vid: int,
    pid: int,
    *,
    find_all: bool = False,
) -> list[Any]:
    """Devices matching *vid*/*pid*, immune to libusb's cached device list.

    Always returns a list (empty when nothing matches or libusb is
    unavailable), so callers never have to special-case ``None`` or the
    iterable-``Device`` trap. Never raises.
    """
    try:
        cached = _as_list(usb_core.find(idVendor=vid, idProduct=pid, find_all=find_all), find_all)
    except Exception:  # noqa: BLE001 — e.g. NoBackendError on Windows CI
        cached = []
    if cached:
        return cached

    backend = _fresh_backend()
    if backend is None:
        return []
    try:
        found = _as_list(
            usb_core.find(idVendor=vid, idProduct=pid, find_all=find_all, backend=backend),
            find_all,
        )
    except Exception:  # noqa: BLE001
        return []

    for dev in found:
        # Own the context explicitly: pyusb calls libusb_exit() when the
        # backend is garbage-collected, which must not happen while a handle
        # opened from `dev` is still in use.
        try:
            dev._touchy_usb_context = backend
        except Exception:  # noqa: BLE001 — fakes/custom backends may not allow it
            pass
    return found
