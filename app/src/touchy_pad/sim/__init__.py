"""Stage-30 device simulator.

A 100%-Python stand-in for the ESP32 firmware that speaks the same
host-API protocol (see ``docs/host-api.md``) over an in-process
:class:`~touchy_pad.api._transport.Transport`. Enables app development
without device hardware.

Two modes:

* **Headless** (default for tests / CI):
  ``SimDeviceTransport(headless=True)`` — no GUI imported, screen
  switches and dispatched actions log to stderr.
* **Windowed** (``pip install touchy-pad[sim]`` required):
  ``SimDeviceTransport()`` — opens a PySide6 window that renders the
  current screen and accepts user clicks. (Stage 30, step 4+.)

Typical use::

    from touchy_pad.api import TouchyClient
    from touchy_pad.sim import SimDeviceTransport

    with TouchyClient(SimDeviceTransport(headless=True)) as pad:
        pad.file_save("screens/home.pb", home.to_proto().SerializeToString())
        pad.screen_load("home")
"""

from __future__ import annotations

from .transport import SimDeviceTransport

__all__ = ["SimDeviceTransport"]


def run_sim_window(**kwargs):
    """Lazy passthrough to :func:`touchy_pad.sim.window.run_sim_window`.

    Importing :mod:`PySide6` at module import time would defeat the
    "headless costs nothing" promise, so the GUI entry point is
    resolved on demand.
    """
    from .window import run_sim_window as _impl

    return _impl(**kwargs)
