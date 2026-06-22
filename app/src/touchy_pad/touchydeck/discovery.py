# SPDX-License-Identifier: GPL-3.0-or-later
"""Hook Touchy-Pad devices into ``StreamDeck.DeviceManager.enumerate``.

:func:`install` monkey-patches ``DeviceManager.enumerate`` so callers
that don't know about Touchy-Pad (i.e. StreamController) still see our
devices show up. :func:`find_touchy_decks` is the explicit, no-side-
effects alternative for host apps that want to opt in manually.
"""

from __future__ import annotations

import logging
from typing import TYPE_CHECKING

from ..api.device import Touchy
from ..client import TouchyClient
from ..transport import DeviceNotFoundError, UsbTransport
from .deck import TouchyDeck

if TYPE_CHECKING:
    from StreamDeck.DeviceManager import register_controllers_factory  # noqa: F401

_LOG = logging.getLogger(__name__)


def _touchy_from_client(client: TouchyClient) -> Touchy:
    """Wrap a connected client in a :class:`Touchy` (no event thread).

    The deck polls ``event_consume`` itself (the StreamDeck base class
    drives a read thread), so we deliberately do **not** start
    :class:`Touchy`'s background event thread — it would race the deck's
    own polling on the same client. ``board_info`` is populated so the
    deck can auto-size its grid without an extra RPC.
    """
    info = None
    try:
        info = client.sys_board_info_get()
    except Exception:  # noqa: BLE001
        _LOG.debug("touchydeck: sys_board_info_get during discovery failed", exc_info=True)
    return Touchy(client, start_event_thread=False, board_info=info)


def find_touchy_decks() -> list[TouchyDeck]:
    """Probe for connected Touchy-Pad devices, return TouchyDecks for each.

    Returns an empty list on platforms without libusb / when no device
    is plugged in. Never raises — discovery failures are logged.

    When :func:`touchy_pad.api.create_sim_device` has been called this
    process, the sim device is included in the returned list (in
    addition to any real USB devices). This is how the StreamController
    integration picks up the in-process sim with no extra wiring on
    the consumer side.

    Each returned ``TouchyDeck`` is constructed but *not* opened; the
    caller (or ``DeviceManager.enumerate`` consumers) calls ``.open()``
    when ready to start the read thread + push the grid.
    """
    decks: list[TouchyDeck] = []

    # Real USB device (if any). Single-device for now; future work
    # extends to enumeration across multiple attached pads.
    try:
        transport = UsbTransport()
    except DeviceNotFoundError:
        transport = None
    except Exception:
        _LOG.debug("touchydeck: UsbTransport probe failed", exc_info=True)
        transport = None

    if transport is not None:
        try:
            client = TouchyClient(transport)
            pad = _touchy_from_client(client)
            decks.append(TouchyDeck(pad))
        except Exception:
            _LOG.exception("touchydeck: failed to instantiate TouchyDeck for USB device")
            try:
                transport.close()
            except Exception:  # pragma: no cover
                pass

    # In-process sim (if `create_sim_device` was called this process).
    from ..api.sim_registry import get_sim_transport

    sim_transport = get_sim_transport()
    if sim_transport is not None:
        try:
            sim_client = TouchyClient(sim_transport)
            sim_pad = _touchy_from_client(sim_client)
            decks.append(TouchyDeck(sim_pad, serial=sim_transport.serial))
        except Exception:
            _LOG.exception("touchydeck: failed to instantiate TouchyDeck for sim device")

    return decks


def install() -> None:
    """Monkey-patch ``DeviceManager.enumerate`` to also yield TouchyDecks.

    Idempotent: calling repeatedly is a no-op after the first
    successful patch. The wrapper calls the original ``enumerate``
    first (so real StreamDecks keep working) and appends any
    TouchyDecks found via :func:`find_touchy_decks`.
    """
    try:
        from StreamDeck.DeviceManager import register_controllers_factory

        register_controllers_factory(find_touchy_decks)
    except ImportError as e:  # pragma: no cover
        raise RuntimeError(
            "streamcontroller-streamdeck is not installed; install "
            "touchy-pad[streamdeck] to enable TouchyDeck enumeration"
        ) from e
