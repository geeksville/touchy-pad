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

Security: when a TLS-PSK key is supplied the endpoint must be ``https://``
and the client negotiates a PSK-secured session. Python's
``ssl.SSLContext.set_psk_client_callback`` is only available on
**Python 3.13+**; on older interpreters the HTTPS-PSK path raises a clear
:class:`TransportError`.
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


def _build_psk_context(psk_hex: str) -> ssl.SSLContext:
    """Build a client SSLContext that authenticates with a TLS-PSK key.

    ``psk_hex`` is the shared key as a hex string. Requires Python 3.13+
    (``set_psk_client_callback``); raises :class:`TransportError` otherwise.
    """
    if not hasattr(ssl.SSLContext, "set_psk_client_callback"):
        raise TransportError(
            "HTTPS-PSK requires Python 3.13+ (ssl.SSLContext.set_psk_client_callback)"
        )
    try:
        psk = bytes.fromhex(psk_hex)
    except ValueError as exc:
        raise TransportError(f"--tls-psk must be a hex string: {exc}") from exc

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    # PSK provides mutual authentication on its own; there is no server
    # certificate to verify.
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.set_psk_client_callback(lambda _hint: (None, psk))
    return ctx


class HttpTransport(Transport):
    """A :class:`Transport` that speaks the command API over HTTP(S).

    Parameters
    ----------
    url:
        Base endpoint, ``http://host[:port]`` or ``https://host[:port]``.
    tls_psk:
        TLS-PSK key as a hex string. Required for (and only valid with) an
        ``https://`` URL; forbidden with ``http://``.
    """

    def __init__(self, url: str, *, tls_psk: str | None = None) -> None:
        scheme, host, port = parse_api_url(url)
        if scheme == "https" and not tls_psk:
            raise TransportError("https:// endpoint requires a --tls-psk key")
        if scheme == "http" and tls_psk:
            raise TransportError("--tls-psk is only valid with an https:// endpoint")

        self._scheme = scheme
        self._host = host
        self._port = port
        self._ssl_ctx = _build_psk_context(tls_psk) if scheme == "https" else None
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
