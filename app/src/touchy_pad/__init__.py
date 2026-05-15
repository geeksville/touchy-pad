"""Host library and CLI for the Touchy-Pad USB multitouch device.

Public API:
    TouchyClient   — typed wrapper around the device's command/response/event
                     protocol; the normal entry point for application code.
    UsbTransport   — pyusb-based transport implementation.
    Transport      — abstract base, useful for tests.

See ``proto/touchy.proto`` in the repository root for the wire format.
"""

from __future__ import annotations

from .client import TouchyClient
from .transport import Transport, UsbTransport

__all__ = ["TouchyClient", "Transport", "UsbTransport"]
__version__ = "0.1.0"
