# SPDX-License-Identifier: GPL-3.0-or-later
"""Hook Touchy-Pad devices into ``StreamDeck.DeviceManager.enumerate``.

:func:`install` monkey-patches ``DeviceManager.enumerate`` so callers
that don't know about Touchy-Pad (i.e. StreamController) still see our
devices show up. :func:`find_touchy_decks` is the explicit, no-side-
effects alternative for host apps that want to opt in manually.
"""

from __future__ import annotations

import logging
from collections.abc import Callable
from typing import TYPE_CHECKING

from ..client import TouchyClient
from ..transport import DeviceNotFoundError, UsbTransport
from .deck import TouchyDeck

if TYPE_CHECKING:
    from StreamDeck.DeviceManager import DeviceManager  # noqa: F401

_LOG = logging.getLogger(__name__)

# Saved reference to the original ``DeviceManager.enumerate`` so
# :func:`uninstall` can restore it. ``None`` when not installed.
_ORIGINAL_ENUMERATE: Callable | None = None


def find_touchy_decks() -> list[TouchyDeck]:
    """Probe for connected Touchy-Pad devices, return TouchyDecks for each.

    Returns an empty list on platforms without libusb / when no device
    is plugged in. Never raises — discovery failures are logged.

    Each returned ``TouchyDeck`` is constructed but *not* opened; the
    caller (or ``DeviceManager.enumerate`` consumers) calls ``.open()``
    when ready to start the read thread + push the grid.
    """
    try:
        transport = UsbTransport()
    except DeviceNotFoundError:
        return []
    except Exception:
        _LOG.debug("touchydeck: UsbTransport probe failed", exc_info=True)
        return []

    client = TouchyClient(transport)
    try:
        return [TouchyDeck(client)]
    except Exception:
        _LOG.exception("touchydeck: failed to instantiate TouchyDeck")
        try:
            client.close()
        except Exception:  # pragma: no cover
            pass
        return []


def install() -> None:
    """Monkey-patch ``DeviceManager.enumerate`` to also yield TouchyDecks.

    Idempotent: calling repeatedly is a no-op after the first
    successful patch. The wrapper calls the original ``enumerate``
    first (so real StreamDecks keep working) and appends any
    TouchyDecks found via :func:`find_touchy_decks`.
    """
    global _ORIGINAL_ENUMERATE
    if _ORIGINAL_ENUMERATE is not None:
        return
    try:
        from StreamDeck.DeviceManager import DeviceManager
    except ImportError as e:  # pragma: no cover
        raise RuntimeError(
            "streamcontroller-streamdeck is not installed; install "
            "touchy-pad[streamdeck] to enable TouchyDeck enumeration"
        ) from e

    original = DeviceManager.enumerate

    def _patched_enumerate(self):  # type: ignore[no-untyped-def]
        # Always return real StreamDecks first so existing apps see
        # them at the same indices they used to.
        try:
            decks = list(original(self))
        except Exception:
            _LOG.debug("touchydeck: original enumerate failed", exc_info=True)
            decks = []
        decks.extend(find_touchy_decks())
        return decks

    _ORIGINAL_ENUMERATE = original
    DeviceManager.enumerate = _patched_enumerate  # type: ignore[method-assign]


def uninstall() -> None:
    """Reverse :func:`install`. No-op if never installed."""
    global _ORIGINAL_ENUMERATE
    if _ORIGINAL_ENUMERATE is None:
        return
    try:
        from StreamDeck.DeviceManager import DeviceManager

        DeviceManager.enumerate = _ORIGINAL_ENUMERATE  # type: ignore[method-assign]
    finally:
        _ORIGINAL_ENUMERATE = None
