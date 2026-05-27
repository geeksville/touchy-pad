"""TCP transport for the Touchy-Pad out-of-process simulator.

The simulator (``touchy simulator``) exposes the same protobuf
command/response protocol over a TCP socket that the firmware exposes
over USB bulk endpoints. Framing on the wire is *identical*: a 4-byte
little-endian length prefix followed by the nanopb payload (see
:mod:`touchy_pad.transport`).

This module provides:
  * :data:`DEFAULT_SIM_PORT` — the agreed-upon TCP port.
  * :data:`SIM_URL_ENV` — the env-var name used as a fallback for
    discovery (``TOUCHY_SIM_URL``).
  * :class:`TcpTransport` — a :class:`~touchy_pad.transport.Transport`
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
import threading
import time

from .transport import _LEN_STRUCT, _MAX_FRAME, Transport, TransportError, _pack

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


class TcpTransport(Transport):
    """Transport that talks the USB-bulk wire format over a TCP socket.

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
        self._addr = (host, port)
        self._lock = threading.Lock()
        self._closed = False

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

    # -- Transport interface --------------------------------------------

    def send_command(self, payload: bytes) -> None:
        if self._closed:
            raise TransportError("TcpTransport is closed")
        frame = _pack(payload)
        with self._lock:
            try:
                self._sock.sendall(frame)
            except OSError as exc:
                self._closed = True
                raise TransportError(f"sim TCP send failed: {exc}") from exc

    def recv_response(self, timeout_ms: int = 2000) -> bytes:
        if self._closed:
            raise TransportError("TcpTransport is closed")
        self._sock.settimeout(timeout_ms / 1000.0)
        try:
            header = self._recv_exact(_LEN_STRUCT.size)
            (length,) = _LEN_STRUCT.unpack(header)
            if length > _MAX_FRAME:
                raise TransportError(f"sim TCP frame exceeds {_MAX_FRAME}-byte cap: {length} bytes")
            return self._recv_exact(length)
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

    # -- helpers --------------------------------------------------------

    def _recv_exact(self, n: int) -> bytes:
        buf = bytearray()
        while len(buf) < n:
            chunk = self._sock.recv(n - len(buf))
            if not chunk:
                raise TransportError("sim TCP: connection closed by peer")
            buf.extend(chunk)
        return bytes(buf)


__all__ = [
    "DEFAULT_SIM_PORT",
    "SIM_URL_ENV",
    "TcpTransport",
    "parse_sim_url",
    "sim_url_from_env",
]
