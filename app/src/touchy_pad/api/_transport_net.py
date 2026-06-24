"""TCP transport for the Touchy-Pad out-of-process simulator.

The simulator (``touchy simulator``) exposes the same protobuf
command/response protocol over a TCP socket that the firmware exposes
over USB bulk endpoints. Framing on the wire is *identical*: a 4-byte
little-endian length prefix followed by the nanopb payload (see
:mod:`touchy_pad.api._transport`).

This module provides:
  * :data:`DEFAULT_SIM_PORT` — the agreed-upon TCP port.
  * :data:`SIM_URL_ENV` — the env-var name used as a fallback for
    discovery (``TOUCHY_SIM_URL``).
  * :class:`TcpTransport` — a :class:`~touchy_pad.api._transport.Transport`
    that speaks the wire format above to a remote / in-process
    :class:`~touchy_pad.sim.server.SimServer`.
  * :func:`parse_sim_url` — split a ``host[:port]`` /
    ``tcp://host[:port]`` string into ``(host, port)``.

One TCP connection serves one command/response stream. Events are
still polled via ``EventConsumeCmd`` exactly as on the USB transport.
"""

from __future__ import annotations

import logging
import os
import socket
import time

from ._transport import (
    TransportError,
    _StreamFramedTransport,
)

#: The default TCP port the out-of-process simulator listens on.
#: Exposed as a symbolic constant so the firmware/host code never
#: hardcodes the raw number twice.
DEFAULT_SIM_PORT: int = 8935

#: Environment variable consulted by :func:`touchy_pad.api.touchy_open`
#: (and the Rust client) before falling back to USB enumeration. Value
#: is parsed by :func:`parse_sim_url`.
SIM_URL_ENV: str = "TOUCHY_SIM_URL"

_log = logging.getLogger(__name__)


def parse_sim_url(value: str) -> tuple[str, int]:
    """Parse a ``host[:port]`` / ``tcp://host[:port]`` string.

    Returns ``(host, port)``; port defaults to :data:`DEFAULT_SIM_PORT`
    when omitted. IPv6 literals in square brackets are supported:
    ``[::1]:8935``.
    """
    s = value.strip()
    if s.startswith("tcp://"):
        s = s[len("tcp://") :]
    # Strip any trailing path (we don't use one, but be forgiving).
    s = s.split("/", 1)[0]

    host: str
    port = DEFAULT_SIM_PORT
    if s.startswith("["):
        end = s.index("]")
        host = s[1:end]
        rest = s[end + 1 :]
        if rest.startswith(":"):
            port = int(rest[1:])
    elif ":" in s:
        host, _, p = s.rpartition(":")
        port = int(p)
    else:
        host = s
    if not host:
        raise ValueError(f"empty host in sim URL: {value!r}")
    return host, port


def sim_url_from_env() -> tuple[str, int] | None:
    """Return ``(host, port)`` from :data:`SIM_URL_ENV`, or ``None``."""
    raw = os.environ.get(SIM_URL_ENV)
    if not raw:
        return None
    try:
        return parse_sim_url(raw)
    except (ValueError, IndexError) as exc:
        _log.warning("ignoring malformed %s=%r: %s", SIM_URL_ENV, raw, exc)
        return None


class TcpTransport(_StreamFramedTransport):
    """Transport that talks the wire framing over a TCP socket.

    Parameters
    ----------
    host:
        Hostname or IP address of the simulator server.
    port:
        TCP port (defaults to :data:`DEFAULT_SIM_PORT`).
    connect_timeout_ms:
        How long the constructor will retry ``socket.connect()`` before
        giving up. A short retry covers the "client races server" case
        common in tests.
    """

    # The TCP sim now consumes raw LVGL ``.bin`` exactly like the
    # firmware does (Stage 63), so the host-side image pipeline must
    # convert PNG/JPG/etc. to ``.bin`` before upload.
    needs_image_conversion = True

    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = DEFAULT_SIM_PORT,
        *,
        connect_timeout_ms: int = 5000,
    ) -> None:
        super().__init__()
        self._addr = (host, port)

        deadline = time.monotonic() + connect_timeout_ms / 1000.0
        last_exc: Exception | None = None
        while True:
            try:
                self._sock = socket.create_connection(self._addr, timeout=2.0)
                break
            except OSError as exc:
                last_exc = exc
                if time.monotonic() >= deadline:
                    raise TransportError(
                        f"could not connect to sim at {host}:{port}: {exc}"
                    ) from exc
                time.sleep(0.05)
        # Disable Nagle: command/response messages are small and
        # latency matters more than coalescing.
        try:
            self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass
        _log.info("sim: TCP transport connected to %s:%d", host, port)
        del last_exc  # silence linter

    # -- stream primitives ----------------------------------------------

    def _write_all(self, data: bytes) -> None:
        try:
            self._sock.sendall(data)
        except OSError as exc:
            self._closed = True
            raise TransportError(f"sim TCP send failed: {exc}") from exc

    def _read_some(self, timeout_ms: int) -> bytes:
        self._sock.settimeout(timeout_ms / 1000.0)
        try:
            return self._sock.recv(65536)
        except TimeoutError as exc:
            raise TransportError(f"sim TCP: no response within {timeout_ms} ms") from exc
        except OSError as exc:
            self._closed = True
            raise TransportError(f"sim TCP recv failed: {exc}") from exc

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            self._sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self._sock.close()
        except OSError:
            pass


__all__ = [
    "DEFAULT_SIM_PORT",
    "SIM_URL_ENV",
    "TcpTransport",
    "parse_sim_url",
    "sim_url_from_env",
]
