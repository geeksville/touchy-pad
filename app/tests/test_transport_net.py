"""Tests for Stage 63 — TCP transport + sim server + env-var fallback."""

from __future__ import annotations

import socket
import threading

import pytest

from touchy_pad.client import TouchyClient
from touchy_pad.sim.server import SimServer, SimServerTransport
from touchy_pad.transport import TransportError
from touchy_pad.transport_net import (
    DEFAULT_SIM_PORT,
    SIM_URL_ENV,
    TcpTransport,
    parse_sim_url,
    sim_url_from_env,
)

# ---------------------------------------------------------------------------
# parse_sim_url
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "raw, expected",
    [
        ("127.0.0.1:8935", ("127.0.0.1", 8935)),
        ("127.0.0.1", ("127.0.0.1", DEFAULT_SIM_PORT)),
        ("tcp://127.0.0.1:1234", ("127.0.0.1", 1234)),
        ("tcp://example.test", ("example.test", DEFAULT_SIM_PORT)),
        ("[::1]:9000", ("::1", 9000)),
        ("[::1]", ("::1", DEFAULT_SIM_PORT)),
        ("tcp://[::1]:9000/", ("::1", 9000)),
    ],
)
def test_parse_sim_url_ok(raw: str, expected: tuple[str, int]) -> None:
    assert parse_sim_url(raw) == expected


@pytest.mark.parametrize("raw", ["", ":8935", "tcp://"])
def test_parse_sim_url_rejects_empty_host(raw: str) -> None:
    with pytest.raises((ValueError, IndexError)):
        parse_sim_url(raw)


# ---------------------------------------------------------------------------
# sim_url_from_env
# ---------------------------------------------------------------------------


def test_sim_url_from_env_unset(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv(SIM_URL_ENV, raising=False)
    assert sim_url_from_env() is None


def test_sim_url_from_env_set(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv(SIM_URL_ENV, "tcp://10.0.0.1:8000")
    assert sim_url_from_env() == ("10.0.0.1", 8000)


def test_sim_url_from_env_malformed_returns_none(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv(SIM_URL_ENV, ":::not-a-url")
    assert sim_url_from_env() is None


# ---------------------------------------------------------------------------
# SimServer + TcpTransport round-trip
# ---------------------------------------------------------------------------


@pytest.fixture
def sim_server(tmp_path):
    srv = SimServerTransport(serial="SIMTEST", fs_root=tmp_path)
    yield srv
    srv.close()


def test_sim_server_board_info_roundtrip(sim_server: SimServerTransport) -> None:
    """Full nanopb framing exercise: client -> TCP -> sim -> response."""
    c = TouchyClient(sim_server)
    info = c.sys_board_info_get()
    assert info.board_name == "sim"


def test_sim_server_separate_tcp_client(tmp_path) -> None:
    """A bare ``SimServer`` (no bundled transport) accepts one TCP client."""
    srv = SimServer(host="127.0.0.1", port=0, serial="SIMTEST2", fs_root=tmp_path)
    try:
        t = TcpTransport(srv.host, srv.port)
        try:
            c = TouchyClient(t)
            info = c.sys_board_info_get()
            assert info.board_name == "sim"
        finally:
            t.close()
    finally:
        srv.close()


def test_sim_server_second_client_is_rejected(tmp_path) -> None:
    """Only one TCP client at a time — second connect hits EOF / RST."""
    srv = SimServer(host="127.0.0.1", port=0, serial="SIMTEST3", fs_root=tmp_path)
    first = TcpTransport(srv.host, srv.port)
    try:
        # First client must still be usable.
        c = TouchyClient(first)
        assert c.sys_board_info_get().board_name == "sim"
        # Second raw connect should be slammed shut by the server.
        extra = socket.create_connection((srv.host, srv.port), timeout=2.0)
        extra.settimeout(2.0)
        extra.sendall(b"\x00\x00\x00\x00")
        try:
            chunk = extra.recv(64)
        except ConnectionResetError:
            chunk = b""
        extra.close()
        assert chunk == b""
    finally:
        first.close()
        srv.close()


# ---------------------------------------------------------------------------
# TOUCHY_SIM_URL fallback in TouchyClient.open()
# ---------------------------------------------------------------------------


def test_touchyclient_open_uses_env_var(
    tmp_path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    srv = SimServer(host="127.0.0.1", port=0, serial="SIMENV", fs_root=tmp_path)
    try:
        monkeypatch.setenv(SIM_URL_ENV, f"tcp://{srv.host}:{srv.port}")
        c = TouchyClient.open()
        try:
            info = c.sys_board_info_get()
            assert info.board_name == "sim"
        finally:
            c.close()
    finally:
        srv.close()


def test_tcp_transport_connect_refused_raises() -> None:
    # Pick an unused loopback port by binding and immediately closing.
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    with pytest.raises(TransportError):
        TcpTransport("127.0.0.1", port, connect_timeout_ms=200)


# ---------------------------------------------------------------------------
# Concurrent commands on one TcpTransport are serialised by the lock.
# ---------------------------------------------------------------------------


def test_tcp_transport_concurrent_rpcs(sim_server: SimServerTransport) -> None:
    c = TouchyClient(sim_server)
    errors: list[BaseException] = []

    def worker() -> None:
        try:
            for _ in range(10):
                info = c.sys_board_info_get()
                assert info.board_name == "sim"
        except BaseException as exc:  # noqa: BLE001
            errors.append(exc)

    threads = [threading.Thread(target=worker) for _ in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert not errors, errors
