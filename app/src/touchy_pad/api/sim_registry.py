# SPDX-License-Identifier: GPL-3.0-or-later
"""Process-wide registry for an optional in-process sim device.

API consumers (CLI tools, tests, host apps without USB hardware) call
:func:`create_sim_device` once at startup to spin up an in-process
:class:`~touchy_pad.sim.transport.SimDeviceTransport`. After that:

* :func:`touchy_get_pad_ids` includes the sim's pseudo-serial in its
  return value alongside any real USB devices.
* :func:`touchy_open` (and :func:`touchy_pad.touchydeck.find_touchy_decks`)
  open the sim transparently when ``serial`` matches the sim's serial
  (or when there's no real device and the sim is the only candidate).

The registry holds a single sim instance per process. The sim's worker
thread is daemonised, so callers don't strictly have to call
:func:`destroy_sim_device` at shutdown — but doing so frees the
pseudo-filesystem and Qt window cleanly during long-lived host
processes (e.g. StreamController).
"""

from __future__ import annotations

import logging
import threading
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from ..sim.transport import SimDeviceTransport

_LOG = logging.getLogger(__name__)
_LOCK = threading.Lock()
_SIM_TRANSPORT: SimDeviceTransport | None = None
_SIM_SERIAL: str | None = None


def create_sim_device(
    headless: bool = True,
    *,
    serial: str = "SIM0000",
    fs_root: Path | None = None,
) -> SimDeviceTransport:
    """Spin up (or return the existing) in-process sim device.

    ``headless=False`` is currently advisory: the GUI window is owned
    by the CLI / host app, not the transport, so passing ``False`` does
    not by itself open a Qt window — it just signals to UI-aware
    callers that they should attach a :class:`SimWindow` to the
    returned transport's ``.device``.

    Idempotent: subsequent calls return the already-running sim
    transport without re-checking arguments. Use
    :func:`destroy_sim_device` if you need to reconfigure.
    """
    global _SIM_TRANSPORT, _SIM_SERIAL
    with _LOCK:
        if _SIM_TRANSPORT is not None:
            return _SIM_TRANSPORT
        # Defer the heavy import (touches sim.fs, SimDevice, etc.) so
        # users that never call create_sim_device pay nothing for it.
        from ..sim.transport import SimDeviceTransport

        _SIM_TRANSPORT = SimDeviceTransport(
            serial=serial,
            fs_root=fs_root,
            headless=headless,
        )
        _SIM_SERIAL = serial
        _LOG.info("create_sim_device: sim transport up (serial=%s)", serial)
        return _SIM_TRANSPORT


def destroy_sim_device() -> None:
    """Close the registered sim device, if any. No-op otherwise."""
    global _SIM_TRANSPORT, _SIM_SERIAL
    with _LOCK:
        t = _SIM_TRANSPORT
        _SIM_TRANSPORT = None
        _SIM_SERIAL = None
    if t is not None:
        try:
            t.close()
        except Exception:  # pragma: no cover
            _LOG.exception("destroy_sim_device: close() raised")


def get_sim_transport() -> SimDeviceTransport | None:
    """Return the currently-registered sim transport, or ``None``."""
    return _SIM_TRANSPORT


def get_sim_serial() -> str | None:
    """Return the registered sim serial, or ``None`` when no sim is active."""
    return _SIM_SERIAL
