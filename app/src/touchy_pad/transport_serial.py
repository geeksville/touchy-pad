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

from .transport import TransportError, _StreamFramedTransport

_log = logging.getLogger(__name__)

#: The protocol always runs at this fixed baud rate.
BAUD_RATE = 460800


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


__all__ = ["SerialTransport", "BAUD_RATE"]
