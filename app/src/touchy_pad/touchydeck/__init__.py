# SPDX-License-Identifier: GPL-3.0-or-later
"""StreamDeck-compatible facade over a TouchyPad device (Stage 50.2).

This package makes a TouchyPad look like an Elgato StreamDeck to the
``streamcontroller-streamdeck`` library, so apps such as StreamController
(https://github.com/StreamController/StreamController) can drive Touchy-Pad
hardware without modification.

Two entry points:

* :func:`install` — monkey-patches
  ``StreamDeck.DeviceManager.DeviceManager.enumerate`` so connected
  TouchyPad devices appear alongside real StreamDecks in the enumerate
  result. Idempotent; safe to call multiple times. Apps that already
  iterate ``DeviceManager().enumerate()`` get TouchyDeck support for
  free once this runs.
* :func:`find_touchy_decks` — explicit list of currently-attached
  TouchyDecks, no monkey-patching. For host apps that want opt-in
  discovery.

The :class:`TouchyDeck` class itself subclasses the abstract
``StreamDeck.Devices.StreamDeck.StreamDeck`` directly (not one of the
concrete device subclasses), so it carries no Elgato-specific HID
quirks. Wire-level event semantics rely on the dual-edge ``on_press``
/ ``on_release`` widget action slots added in Stage 50.2 — see
``proto/widgets.proto`` and ``firmware/main/widget_builders.cpp``.

Optional dependency: install with ``pip install 'touchy-pad[streamdeck]'``
to pull in ``streamcontroller-streamdeck``.
"""

from __future__ import annotations

from .deck import TouchyDeck
from .discovery import find_touchy_decks, install, uninstall

__all__ = ["TouchyDeck", "find_touchy_decks", "install", "uninstall"]
