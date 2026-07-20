"""Bare-HTTP front end for the simulator's command API (Stage lb8).

Mirrors the device's ``POST /touchy/api/v1/command`` endpoint so the
:class:`~touchy_pad.api._transport_http.HttpTransport` — and any plain
``curl`` — can drive the in-process :class:`~touchy_pad.sim.device.SimDevice`
over HTTP exactly as it would a real networked device.

The simulator only ever speaks **plaintext HTTP** (no TLS); it listens on
:data:`DEFAULT_SIM_HTTP_PORT` (8083) by default. The request/response
bodies are *bare* serialized ``Command`` / ``Response`` protobufs — no
``MAGIC | LEN | CRC8`` framing (HTTP delimits them).
"""

from __future__ import annotations

import logging
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from .. import _proto
from ..api._transport_http import API_PATH, CONTENT_TYPE
from .device import SimDevice

_log = logging.getLogger("touchy_pad.sim.http")

#: Fixed plaintext-HTTP port the simulator serves the command API on.
DEFAULT_SIM_HTTP_PORT: int = 8083


class _NoFqdnHTTPServer(ThreadingHTTPServer):
    """ThreadingHTTPServer that skips the reverse-DNS lookup in server_bind.

    The standard ``HTTPServer.server_bind`` calls ``socket.getfqdn(host)``
    to populate ``server_name``. On macOS (and some CI runners) this
    reverse-DNS lookup for ``127.0.0.1`` blocks for 30+ seconds because
    the loopback address has no PTR record. We don't need the FQDN; just
    store the literal bind address.
    """

    def server_bind(self) -> None:  # type: ignore[override]
        import socket
        import socketserver

        # Call TCPServer.server_bind directly, bypassing HTTPServer's
        # override (which does the blocking socket.getfqdn lookup).
        socketserver.TCPServer.server_bind(self)
        host, port = self.server_address[:2]
        self.server_name = socket.gethostname() if not host else host
        self.server_port = port


#: Fixed plaintext-HTTP port the simulator serves the command API on.
DEFAULT_SIM_HTTP_PORT: int = 8083


class SimHttpServer:
    """Serve a :class:`SimDevice` over ``POST /touchy/api/v1/command``.

    Parameters
    ----------
    device:
        The sim device whose :meth:`SimDevice.handle_command` answers
        each request.
    host, port:
        Bind address; defaults to loopback on
        :data:`DEFAULT_SIM_HTTP_PORT`. Pass ``port=0`` for an ephemeral
        port (read it back from :attr:`port`).
    """

    def __init__(
        self,
        device: SimDevice,
        *,
        host: str = "127.0.0.1",
        port: int = DEFAULT_SIM_HTTP_PORT,
    ) -> None:
        self._device = device

        handler = self._make_handler(device)
        self._httpd = _NoFqdnHTTPServer((host, port), handler)
        self._bound_host, self._bound_port = self._httpd.server_address[:2]
        self._thread = threading.Thread(
            target=self._httpd.serve_forever,
            name=f"sim-http-{self._bound_port}",
            daemon=True,
        )
        self._thread.start()
        _log.info("sim HTTP API on http://%s:%d%s", self._bound_host, self._bound_port, API_PATH)

    @property
    def host(self) -> str:
        return self._bound_host

    @property
    def port(self) -> int:
        return self._bound_port

    @property
    def url(self) -> str:
        return f"http://{self._bound_host}:{self._bound_port}"

    def close(self) -> None:
        self._httpd.shutdown()
        self._httpd.server_close()
        self._thread.join(timeout=2.0)

    def __enter__(self) -> SimHttpServer:
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    @staticmethod
    def _make_handler(device: SimDevice) -> type[BaseHTTPRequestHandler]:
        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, fmt: str, *args: object) -> None:  # noqa: A003
                _log.debug("sim-http %s - " + fmt, self.address_string(), *args)

            def do_POST(self) -> None:  # noqa: N802 (http.server API)
                if self.path != API_PATH:
                    self.send_error(404, "not found")
                    return
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length) if length else b""

                # Stage lb13 — JSON body (Content-Type contains "json"):
                # parse canonical protobuf-JSON → Command, dispatch on the
                # shared core, render the Response back to JSON.
                is_json = "json" in (self.headers.get("Content-Type", "").lower())
                if is_json:
                    from google.protobuf.json_format import (
                        MessageToJson,
                        Parse,
                        ParseError,
                    )

                    try:
                        cmd = Parse(body.decode("utf-8"), _proto.Command())
                    except (ParseError, UnicodeDecodeError, ValueError) as exc:
                        self.send_error(400, f"bad JSON command: {exc}")
                        return
                    try:
                        raw = device.handle_command(cmd.SerializeToString())
                        resp = _proto.Response.FromString(raw)
                        reply = MessageToJson(resp, indent=None).encode("utf-8")
                    except Exception as exc:  # noqa: BLE001 — keep server up
                        _log.exception("sim-http: JSON handler crashed: %s", exc)
                        reply = b"{}"
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(reply)))
                    self.end_headers()
                    self.wfile.write(reply)
                    return

                try:
                    reply = device.handle_command(body)
                except Exception as exc:  # noqa: BLE001 — keep server up
                    _log.exception("sim-http: handler crashed: %s", exc)
                    reply = _proto.Response(code=_proto.RESULT_UNKNOWN_ERROR).SerializeToString()
                self.send_response(200)
                self.send_header("Content-Type", CONTENT_TYPE)
                self.send_header("Content-Length", str(len(reply)))
                self.end_headers()
                self.wfile.write(reply)

        return Handler


__all__ = ["DEFAULT_SIM_HTTP_PORT", "SimHttpServer"]
