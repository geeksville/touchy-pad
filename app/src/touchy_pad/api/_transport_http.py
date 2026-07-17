"""HTTP(S) transport for the Touchy-Pad network API (Stage lb8).

The device (and the simulator) can expose the same protobuf
``Command``/``Response`` protocol over an HTTP endpoint::

    POST /touchy/api/v1/command HTTP/1.1
    Content-Type: application/protobuf
    <binary serialized Command>

    HTTP/1.1 200 OK
    Content-Type: application/protobuf
    <binary serialized Response>

Unlike the byte-stream transports (USB / TCP-sim / serial) there is **no**
``MAGIC | LEN | payload | CRC8`` framing here: HTTP ``Content-Length`` + TCP
already delimit each message, so the POST body is a *bare* serialized
``Command`` and the reply body a *bare* serialized ``Response``.

Because one POST is a complete command→response exchange, this transport
buffers: :meth:`send_command` performs the POST and stashes the response
body; :meth:`recv_response` returns it. The caller (``TouchyClient``)
always brackets the two under its RPC lock, so the buffering is safe.

Security (Stage lb9): an ``https://`` endpoint uses **mutual TLS**. The
client presents the certificate saved by ``touchy pref provision-mtls``
and verifies the device's certificate against the provisioned CA (see
:mod:`touchy_pad.api.mtls`). ``check_hostname`` is disabled — the device's
IP/mDNS name drifts, but the CA signature is the identity guarantee. This
works on any Python 3.x (unlike the abandoned lb8 TLS-PSK path).
"""

from __future__ import annotations

import http.client
import logging
import ssl
from urllib.parse import urlsplit

from ._transport import Transport, TransportError

#: The URI path the device/sim serves the command API on.
API_PATH: str = "/touchy/api/v1/command"

#: Content type for the bare protobuf body.
CONTENT_TYPE: str = "application/protobuf"

#: Environment variable consulted by :func:`touchy_pad.api.touchy_open`
#: to pick up a network endpoint, mirroring ``TOUCHY_SIM_URL``.
API_URL_ENV: str = "TOUCHY_URL"

_log = logging.getLogger(__name__)


def parse_api_url(value: str) -> tuple[str, str, int]:
    """Split an ``http(s)://host[:port]`` string into ``(scheme, host, port)``.

    ``port`` defaults to 80 for ``http`` and 443 for ``https`` when omitted.
    Raises :class:`ValueError` for anything that isn't an ``http``/``https``
    URL with a host.
    """
    parts = urlsplit(value.strip())
    scheme = parts.scheme.lower()
    if scheme not in ("http", "https"):
        raise ValueError(f"not an http(s) URL: {value!r}")
    host = parts.hostname
    if not host:
        raise ValueError(f"missing host in URL: {value!r}")
    port = parts.port if parts.port is not None else (443 if scheme == "https" else 80)
    return scheme, host, port


def _build_mtls_context(endpoint: str | None) -> ssl.SSLContext:
    """Build a mutual-TLS client context from the stored credentials.

    Loads the client cert/key + CA saved by ``touchy pref provision-mtls``
    for *endpoint* (see :mod:`touchy_pad.api.mtls`). Raises
    :class:`TransportError` when no credentials have been provisioned.
    """
    from .mtls import load_client_context

    try:
        return load_client_context(endpoint)
    except FileNotFoundError as exc:
        raise TransportError(str(exc)) from exc


class HttpTransport(Transport):
    """A :class:`Transport` that speaks the command API over HTTP(S).

    Parameters
    ----------
    url:
        Base endpoint, ``http://host[:port]`` or ``https://host[:port]``.
        An ``https://`` URL uses mutual TLS: the client credentials saved
        by ``touchy pref provision-mtls`` for this endpoint are loaded and
        presented, and the device's certificate is verified against the
        provisioned CA.
    """

    def __init__(self, url: str) -> None:
        scheme, host, port = parse_api_url(url)

        self._scheme = scheme
        self._host = host
        self._port = port
        self._ssl_ctx = _build_mtls_context(url) if scheme == "https" else None
        self._pending: bytes | None = None
        self._conn: http.client.HTTPConnection | None = None
        _log.debug("HttpTransport → %s://%s:%d%s", scheme, host, port, API_PATH)

    def _connection(self) -> http.client.HTTPConnection:
        if self._conn is not None:
            return self._conn
        if self._scheme == "https":
            self._conn = http.client.HTTPSConnection(
                self._host, self._port, context=self._ssl_ctx, timeout=10
            )
        else:
            self._conn = http.client.HTTPConnection(self._host, self._port, timeout=10)
        return self._conn

    def send_command(self, payload: bytes) -> None:
        headers = {"Content-Type": CONTENT_TYPE, "Accept": CONTENT_TYPE}
        # Retry once on a stale keep-alive connection.
        for attempt in (0, 1):
            conn = self._connection()
            try:
                conn.request("POST", API_PATH, body=payload, headers=headers)
                resp = conn.getresponse()
                body = resp.read()
            except (http.client.HTTPException, OSError) as exc:
                self._drop_connection()
                if attempt == 0:
                    continue
                raise TransportError(f"HTTP request failed: {exc}") from exc
            if resp.status != 200:
                self._drop_connection()
                raise TransportError(f"HTTP {resp.status} {resp.reason} from {API_PATH}")
            self._pending = body
            return

    def recv_response(self, timeout_ms: int = 2000) -> bytes:
        if self._pending is None:
            raise TransportError("recv_response called before send_command")
        body = self._pending
        self._pending = None
        return body

    def _drop_connection(self) -> None:
        if self._conn is not None:
            try:
                self._conn.close()
            except Exception:  # noqa: BLE001
                pass
            self._conn = None

    def close(self) -> None:
        self._drop_connection()


__all__ = ["API_PATH", "API_URL_ENV", "CONTENT_TYPE", "HttpTransport", "parse_api_url"]
