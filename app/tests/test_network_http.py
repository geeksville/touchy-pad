"""Stage lb8 — HTTP(S) network transport + NetworkConfig round-trip tests."""

from __future__ import annotations

import pytest

from touchy_pad import _proto
from touchy_pad.api import touchy_open
from touchy_pad.api._transport import TransportError
from touchy_pad.api._transport_http import HttpTransport, parse_api_url
from touchy_pad.sim.http_server import SimHttpServer
from touchy_pad.sim.transport import make_tempdir_transport


def test_parse_api_url_defaults() -> None:
    assert parse_api_url("http://dev.local") == ("http", "dev.local", 80)
    assert parse_api_url("https://dev.local") == ("https", "dev.local", 443)
    assert parse_api_url("http://dev.local:8083") == ("http", "dev.local", 8083)
    with pytest.raises(ValueError):
        parse_api_url("ftp://nope")
    with pytest.raises(ValueError):
        parse_api_url("http://")


def test_network_config_round_trips_through_prefs() -> None:
    prefs = _proto.PreferencesFile()
    prefs.network.wifi_ssid = "my-net"
    prefs.network.wifi_psk = "hunter2hunter2"
    prefs.network.hostname = "touchypad-lab"
    prefs.network.tls_psk_key = "deadbeef"
    blob = prefs.SerializeToString()

    parsed = _proto.PreferencesFile()
    parsed.ParseFromString(blob)
    assert parsed.HasField("network")
    assert parsed.network.wifi_ssid == "my-net"
    assert parsed.network.wifi_psk == "hunter2hunter2"
    assert parsed.network.hostname == "touchypad-lab"
    assert parsed.network.tls_psk_key == "deadbeef"
    # file_version is device-owned; the host never sets it.
    assert parsed.file_version == 0


def test_https_without_psk_is_rejected() -> None:
    with pytest.raises(TransportError):
        HttpTransport("https://dev.local")


def test_http_with_psk_is_rejected() -> None:
    with pytest.raises(TransportError):
        HttpTransport("http://dev.local", tls_psk="deadbeef")


def test_board_info_round_trips_over_http() -> None:
    with make_tempdir_transport() as t:
        with SimHttpServer(t.device, port=0) as http:
            with touchy_open(url=http.url) as pad:
                assert pad.board_info is not None
                assert pad.board_info.protocol_version >= 7


def test_set_network_prefs_over_http_reaches_device() -> None:
    with make_tempdir_transport() as t:
        with SimHttpServer(t.device, port=0) as http:
            with touchy_open(url=http.url) as pad:
                prefs = _proto.PreferencesFile()
                prefs.network.wifi_ssid = "office"
                prefs.network.wifi_psk = "s3cr3t-passphrase"
                pad.client.set_preferences(prefs)

                got = pad.client.get_preferences()
    assert got.network.wifi_ssid == "office"
    assert got.network.wifi_psk == "s3cr3t-passphrase"


@pytest.mark.skipif(
    hasattr(__import__("ssl").SSLContext, "set_psk_client_callback"),
    reason="Python 3.13+ supports TLS-PSK; this asserts the <3.13 error path",
)
def test_https_psk_requires_py313() -> None:
    with pytest.raises(TransportError, match="Python 3.13"):
        HttpTransport("https://dev.local", tls_psk="deadbeef")


def test_tls_psk_without_url_is_rejected(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("TOUCHY_URL", raising=False)
    with pytest.raises(ValueError, match="url"):
        touchy_open(tls_psk="deadbeef")


def test_wifi_ssid_and_psk_merge_independently_over_http() -> None:
    with make_tempdir_transport() as t:
        with SimHttpServer(t.device, port=0) as http:
            with touchy_open(url=http.url) as pad:
                p = _proto.PreferencesFile()
                p.network.wifi_ssid = "home"
                pad.client.set_preferences(p)
                p2 = _proto.PreferencesFile()
                p2.network.wifi_psk = "correct-horse"
                pad.client.set_preferences(p2)
                got = pad.client.get_preferences()
    # Setting the PSK must not have wiped the previously-set SSID.
    assert got.network.wifi_ssid == "home"
    assert got.network.wifi_psk == "correct-horse"
