"""Host library and CLI for the Touchy-Pad USB multitouch device.

Public API lives under :mod:`touchy_pad.api` — use that for application
code::

    from touchy_pad.api import touchy_open, Screen, button, TouchyClient

This top-level package only exposes :data:`__version__`; the CLI,
device simulator, StreamDeck shim and generated protobuf bindings
(``touchy_pad._proto``) are considered internal building blocks.

See ``proto/touchy.proto`` in the repository root for the wire format.
"""

from __future__ import annotations

__all__ = ["__version__"]
__version__ = "0.1.0"
