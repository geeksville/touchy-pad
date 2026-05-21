"""Host library and CLI for the Touchy-Pad USB multitouch device.

Public API lives under :mod:`touchy_pad.api` — use that for application
code::

    from touchy_pad.api import touchy_open, Screen, button

The top-level :class:`TouchyClient` / :class:`UsbTransport` exports
below remain available but are considered internal building blocks; new
code should prefer the :mod:`touchy_pad.api` facade.

See ``proto/touchy.proto`` in the repository root for the wire format.
"""

from __future__ import annotations

from .client import TouchyClient
from .transport import Transport, UsbTransport

__all__ = ["TouchyClient", "Transport", "UsbTransport"]
__version__ = "0.1.0"
