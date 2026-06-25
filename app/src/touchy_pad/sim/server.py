"""TCP server that fronts a :class:`SimDevice` with the USB wire format.

Stage 63 — the in-process queue-backed sim still exists for unit tests
(:mod:`touchy_pad.sim.transport`), but the user-facing simulator (CLI
``touchy simulator`` and the embedded ``--sim-gui`` / ``--sim-headless``
modes) goes through this class so the *exact same* length-prefixed
nanopb framing exercised by the USB bulk endpoints is exercised by the
sim too. That makes the host's PNG→LVGL ``.bin`` conversion, the
:class:`~touchy_pad.api._transport_net.TcpTransport`, and the Rust client
all reach the sim by the same code paths they'd reach hardware.

One TCP connection at a time, mirroring USB exclusivity. Concurrent
connect attempts are closed immediately (the client sees EOF).
"""

from __future__ import annotations

import logging
import socket
import threading
from collections.abc import Callable
from pathlib import Path

from .. import _proto
from ..api._transport import _FrameDecoder, _pack
from ..api._transport_net import DEFAULT_SIM_PORT, TcpTransport
from .device import SimDevice
from .fs import SimFs, default_cache_root

_log = logging.getLogger("touchy_pad.sim.server")


class SimServer:
    """Listen for TCP clients and dispatch framed commands to a :class:`SimDevice`.

    Parameters
    ----------
    host, port:
        Bind address. Defaults to loopback on
        :data:`~touchy_pad.api._transport_net.DEFAULT_SIM_PORT`. Pass
        ``port=0`` to let the OS pick (handy for tests); read the
        chosen port back from :attr:`port`.
    serial:
        Pseudo-USB serial; selects the per-serial subdir under
        ``fs_root``.
    fs_root:
        Cache directory root for the sim's pseudo-filesystem.
        ``None`` uses :func:`default_cache_root`.
    display_size:
        Reported back via ``SysBoardInfoResponse``.
    on_screen_change:
        Optional callback invoked on the worker thread whenever the
        sim's active screen changes. Wired by the Qt window.
    """

    def __init__(
        self,
        *,
        host: str = "127.0.0.1",
        port: int = DEFAULT_SIM_PORT,
        serial: str = "SIM0000",
        fs_root: Path | None = None,
        display_size: tuple[int, int] = (480, 300),
        on_screen_change: Callable[[_proto.Screen | None], None] | None = None,
    ) -> None:
        self._serial = serial
        self._fs = SimFs(fs_root or default_cache_root(), serial)
        self._device = SimDevice(
            self._fs,
            on_screen_change=on_screen_change,
            display_size=display_size,
        )

        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind((host, port))
        self._sock.listen(4)
        self._bound_host, self._bound_port = self._sock.getsockname()[:2]

        self._stop = threading.Event()
        self._client_lock = threading.Lock()
        self._active_client: socket.socket | None = None
        self._accept_thread = threading.Thread(
            target=self._accept_loop,
            name=f"sim-server-accept-{self._bound_port}",
            daemon=True,
        )
        self._accept_thread.start()
        _log.info(
            "sim server listening on %s:%d (serial=%s, fs=%s)",
            self._bound_host,
            self._bound_port,
            serial,
            self._fs.root,
        )

    # -- introspection ---------------------------------------------------

    @property
    def device(self) -> SimDevice:
        return self._device

    @property
    def serial(self) -> str:
        return self._serial

    @property
    def host(self) -> str:
        return self._bound_host

    @property
    def port(self) -> int:
        return self._bound_port

    # -- lifecycle -------------------------------------------------------

    def close(self) -> None:
        """Stop accepting new clients and disconnect any active one."""
        if self._stop.is_set():
            return
        self._stop.set()
        try:
            self._sock.close()
        except OSError:
            pass
        with self._client_lock:
            client = self._active_client
            self._active_client = None
        if client is not None:
            try:
                client.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                client.close()
            except OSError:
                pass
        self._accept_thread.join(timeout=2.0)

    def __enter__(self) -> SimServer:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    # -- worker loops ----------------------------------------------------

    def _accept_loop(self) -> None:
        while not self._stop.is_set():
            try:
                client, addr = self._sock.accept()
            except OSError:
                return  # socket closed during shutdown
            with self._client_lock:
                if self._active_client is not None:
                    _log.warning("sim: rejecting %s (server busy)", addr)
                    try:
                        client.close()
                    except OSError:
                        pass
                    continue
                self._active_client = client
            _log.info("sim: client connected from %s", addr)
            t = threading.Thread(
                target=self._serve_client,
                name=f"sim-server-client-{addr[1]}",
                args=(client, addr),
                daemon=True,
            )
            t.start()

    def _serve_client(self, client: socket.socket, addr: tuple) -> None:
        try:
            client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            decoder = _FrameDecoder()
            while not self._stop.is_set():
                payload = decoder.next_frame()
                if payload is None:
                    chunk = self._recv_some(client)
                    if not chunk:
                        return
                    decoder.feed(chunk)
                    continue
                try:
                    reply = self._device.handle_command(payload)
                except Exception as exc:  # noqa: BLE001 — keep server up
                    _log.exception("sim: handler crashed: %s", exc)
                    reply = _proto.Response(code=_proto.RESULT_UNKNOWN_ERROR).SerializeToString()
                try:
                    client.sendall(_pack(reply))
                except OSError as exc:
                    _log.info("sim: send to %s failed (%s); disconnecting", addr, exc)
                    return
        finally:
            with self._client_lock:
                if self._active_client is client:
                    self._active_client = None
            try:
                client.close()
            except OSError:
                pass
            _log.info("sim: client %s disconnected", addr)

    # -- helpers ---------------------------------------------------------

    @staticmethod
    def _recv_some(client: socket.socket) -> bytes:
        """Read a chunk from ``client``; ``b""`` on EOF or error."""
        try:
            return client.recv(65536)
        except OSError:
            return b""


__all__ = ["SimServer", "SimServerTransport", "make_tempdir_server_transport"]


# ---------------------------------------------------------------------------


class SimServerTransport(TcpTransport):
    """A :class:`TcpTransport` that owns the :class:`SimServer` it talks to.

    Convenience for the CLI's ``--sim-headless`` / ``--sim-gui`` modes
    and for tests: a single object that, when closed, tears down both
    the network client *and* the underlying in-process sim server.

    Exposes ``.device`` and ``.serial`` properties so callers that
    previously held a :class:`~touchy_pad.sim.transport.SimDeviceTransport`
    (notably :mod:`touchy_pad.touchydeck.discovery`) keep working
    unchanged when they get one of these instead.
    """

    def __init__(
        self,
        *,
        serial: str = "SIM0000",
        fs_root: Path | None = None,
        display_size: tuple[int, int] = (480, 300),
        on_screen_change: Callable[[_proto.Screen | None], None] | None = None,
    ) -> None:
        self._server = SimServer(
            port=0,
            serial=serial,
            fs_root=fs_root,
            display_size=display_size,
            on_screen_change=on_screen_change,
        )
        try:
            super().__init__(self._server.host, self._server.port)
        except Exception:
            self._server.close()
            raise

    @property
    def device(self) -> SimDevice:
        return self._server.device

    @property
    def serial(self) -> str:
        return self._server.serial

    def close(self) -> None:
        try:
            super().close()
        finally:
            self._server.close()

    def __exit__(self, *exc: object) -> None:
        self.close()
        td = getattr(self, "_tempdir", None)
        if td is not None:
            td.cleanup()


def make_tempdir_server_transport(**kwargs: object) -> SimServerTransport:
    """Convenience: a :class:`SimServerTransport` with a fresh temp fs root."""
    import tempfile

    td = tempfile.TemporaryDirectory(prefix="touchy-pad-sim-")
    kwargs.setdefault("fs_root", Path(td.name))
    t = SimServerTransport(**kwargs)  # type: ignore[arg-type]
    t._tempdir = td  # type: ignore[attr-defined]  # keep alive
    return t
