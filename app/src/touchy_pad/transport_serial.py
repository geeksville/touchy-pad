"""Serial-port transport for the touchy-pad wire protocol.

This lets the host talk to a device that exposes the protocol over a
serial port (a USB-CDC ACM port, or a real UART on boards without native
USB such as the future CYD2USB). It uses the same self-synchronising
frame as every other transport (see :mod:`touchy_pad.transport`):

    MAGIC(2) | LEN(u16 LE) | payload | CRC8(1)

The port must carry *only* protocol frames — device log output is
tunnelled in-band via the Stage 64.1 ``LogRecord`` mechanism, never
written to this port as raw text.

``pyserial`` is imported lazily so that importing this module (and the
package as a whole) never hard-fails on platforms where pyserial is not
installed.
"""

from __future__ import annotations

import logging
import os
from pathlib import Path

from .transport import TransportError, _StreamFramedTransport
from .usb_ids import UART_BRIDGE_VID_PIDS

_log = logging.getLogger(__name__)

#: The protocol always runs at this fixed baud rate.
BAUD_RATE = 460800

#: Devcontainer mount point: when the host's ``/dev`` is bind-mounted at
#: ``/host/dev``, ``serial.tools.list_ports`` (which reads sysfs) sees nothing
#: because sysfs isn't bind-mounted. We fall back to a path-shape glob there.
_HOST_DEV_DIR = Path("/host/dev")


class SerialTransport(_StreamFramedTransport):
    """Transport that talks the wire framing over a serial port.

    Parameters
    ----------
    port:
        Serial device path (e.g. ``/dev/ttyACM0`` or ``COM3``).
    timeout_ms:
        Default per-read timeout used when polling the port.
    """

    needs_image_conversion = True

    def __init__(self, port: str, *, timeout_ms: int = 2000) -> None:
        super().__init__()
        try:
            import serial  # type: ignore[import-untyped]
        except ImportError as exc:  # pragma: no cover - env dependent
            raise TransportError(
                "pyserial is required for serial transport; install it with "
                "`pip install pyserial`"
            ) from exc

        self._port_name = port
        self._read_timeout_s = timeout_ms / 1000.0
        try:
            self._ser = serial.Serial(
                port=port,
                baudrate=BAUD_RATE,
                timeout=self._read_timeout_s,
                write_timeout=2.0,
            )
        except serial.SerialException as exc:
            raise TransportError(f"could not open serial port {port}: {exc}") from exc
        _log.info("serial: transport connected to %s @ %d baud", port, BAUD_RATE)

    # -- stream primitives ----------------------------------------------

    def _write_all(self, data: bytes) -> None:
        try:
            self._ser.write(data)
            self._ser.flush()
        except Exception as exc:  # noqa: BLE001 - pyserial raises various types
            self._closed = True
            raise TransportError(f"serial write to {self._port_name} failed: {exc}") from exc

    def _read_some(self, timeout_ms: int) -> bytes:
        want_s = timeout_ms / 1000.0
        if want_s != self._read_timeout_s:
            self._ser.timeout = want_s
            self._read_timeout_s = want_s
        try:
            # ``read(1)`` blocks up to the timeout for the first byte, then
            # drain whatever else is already buffered so we hand the decoder
            # large chunks instead of one byte at a time.
            chunk = self._ser.read(1)
            if not chunk:
                raise TransportError(
                    f"serial {self._port_name}: no response within {timeout_ms} ms"
                )
            pending = self._ser.in_waiting
            if pending:
                chunk += self._ser.read(pending)
            return chunk
        except TransportError:
            raise
        except Exception as exc:  # noqa: BLE001 - pyserial raises various types
            self._closed = True
            raise TransportError(f"serial read from {self._port_name} failed: {exc}") from exc

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            self._ser.close()
        except Exception:  # noqa: BLE001 - best-effort
            pass


def _is_rw_accessible(path: str) -> bool:
    """Return True iff *path* exists and is read+write accessible.

    Used to drop device nodes the user has no permission to open (no
    ``dialout``/``uucp`` group membership). Trying to open them would
    just fail at :class:`SerialTransport` construction time anyway, so
    silently skip them during discovery.
    """
    try:
        return os.access(path, os.R_OK | os.W_OK)
    except OSError:
        return False


def discover_serial_ports() -> list[str]:
    """Stage 83 — return device paths of UART-bridge Touchys on the host.

    Walks :func:`serial.tools.list_ports.comports` and keeps every port
    whose ``(vid, pid)`` is in :data:`touchy_pad.usb_ids.UART_BRIDGE_VID_PIDS`.
    Inaccessible nodes (no ``dialout`` / ``uucp`` group membership) are
    dropped silently — they would just fail at open time.

    **Devcontainer fallback** (Linux only): if ``comports()`` returns
    nothing *and* ``/host/dev`` exists, additionally glob
    ``/host/dev/ttyUSB*`` and ``/host/dev/ttyACM*``. Sysfs isn't
    bind-mounted into the container so the VID/PID filter can't run
    there; we fall back to the path-shape check, which is consistent
    with the spec's "trust the device if accessible" rule.

    Returns
    -------
    list[str]
        Absolute device-node paths (e.g. ``/dev/ttyUSB0``, ``COM3``,
        ``/host/dev/ttyUSB0``). Empty when pyserial is unavailable or no
        candidates pass the filters.
    """
    paths: list[str] = []
    try:
        from serial.tools import list_ports  # type: ignore[import-untyped]
    except Exception:  # noqa: BLE001 - pyserial may be absent or broken
        ports = []
    else:
        try:
            ports = list(list_ports.comports())
        except Exception:  # noqa: BLE001 - some platforms fail here
            ports = []

    for p in ports:
        vid = getattr(p, "vid", None)
        pid = getattr(p, "pid", None)
        if vid is None or pid is None:
            continue
        if (int(vid), int(pid)) not in UART_BRIDGE_VID_PIDS:
            continue
        device = getattr(p, "device", None)
        if not device:
            continue
        if not _is_rw_accessible(device):
            continue
        paths.append(device)

    # Devcontainer fallback: bind-mounted /host/dev with no sysfs.
    if not paths and _HOST_DEV_DIR.is_dir():
        for pattern in ("ttyUSB*", "ttyACM*"):
            for entry in sorted(_HOST_DEV_DIR.glob(pattern)):
                s = str(entry)
                if _is_rw_accessible(s):
                    paths.append(s)

    return paths


__all__ = ["BAUD_RATE", "SerialTransport", "discover_serial_ports"]
