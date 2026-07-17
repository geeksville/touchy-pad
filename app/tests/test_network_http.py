"""Stage lb8 — HTTP(S) network transport + NetworkConfig round-trip tests."""

from __future__ import annotations

import importlib.util

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
    blob = prefs.SerializeToString()

    parsed = _proto.PreferencesFile()
    parsed.ParseFromString(blob)
    assert parsed.HasField("network")
    assert parsed.network.wifi_ssid == "my-net"
    assert parsed.network.wifi_psk == "hunter2hunter2"
    assert parsed.network.hostname == "touchypad-lab"
    # file_version is device-owned; the host never sets it.
    assert parsed.file_version == 0


def test_tls_psk_key_is_gone() -> None:
    # Stage lb9 removed the (never-working) lb8 tls_psk_key field.
    net_desc = _proto.PreferencesFile.DESCRIPTOR.fields_by_name["network"].message_type
    assert "tls_psk_key" not in net_desc.fields_by_name


def test_https_without_provisioned_creds_is_rejected(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path,
) -> None:
    # No provisioned mTLS creds → opening an https:// endpoint fails with a
    # clear, provisioning-oriented error.
    from touchy_pad.api import mtls

    monkeypatch.setattr(mtls, "_config_root", lambda: tmp_path)
    with pytest.raises(TransportError, match="provision-mtls"):
        HttpTransport("https://dev.local")


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
    importlib.util.find_spec("cryptography") is None,
    reason="cryptography not installed",
)
def test_mtls_pki_generation_and_client_context(monkeypatch, tmp_path) -> None:
    from touchy_pad.api import mtls

    monkeypatch.setattr(mtls, "_config_root", lambda: tmp_path)
    pki = mtls.generate_pki(device_san="touchypad-lab")
    for blob in (pki.ca_cert, pki.server_cert, pki.server_key, pki.client_cert, pki.client_key):
        assert b"BEGIN" in blob
    d = mtls.save_client_creds(pki, "https://touchypad-lab")
    assert (d / mtls.CLIENT_CERT_FILE).exists()
    # A client context loads cleanly from the saved creds.
    ctx = mtls.load_client_context("https://touchypad-lab")
    assert ctx.check_hostname is False


def test_tls_without_url_still_rejected(monkeypatch: pytest.MonkeyPatch) -> None:
    # A plain https URL with no provisioned creds errors (covered above); a
    # missing TOUCHY_URL + no args still hits normal local discovery.
    monkeypatch.delenv("TOUCHY_URL", raising=False)
    # sanity: parse rejects a bare scheme
    with pytest.raises(ValueError):
        parse_api_url("https://")


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


@pytest.mark.skipif(
    importlib.util.find_spec("cryptography") is None,
    reason="cryptography not installed",
)
def test_mtls_cert_upload_over_transport(monkeypatch, tmp_path) -> None:
    """The provisioning flow: generate PKI, push device certs, save host creds."""
    from touchy_pad.api import mtls
    from touchy_pad.api.client import TouchyClient
    from touchy_pad.paths import (
        TLS_CLIENT_CA_PATH,
        TLS_SERVER_CERT_PATH,
        TLS_SERVER_KEY_PATH,
    )

    monkeypatch.setattr(mtls, "_config_root", lambda: tmp_path)
    pki = mtls.generate_pki(device_san="touchypad-lab")

    with make_tempdir_transport() as t:
        client = TouchyClient(t)
        # PEM bytes must upload verbatim (no image conversion).
        client.file_save(TLS_SERVER_CERT_PATH, pki.server_cert)
        client.file_save(TLS_SERVER_KEY_PATH, pki.server_key)
        client.file_save(TLS_CLIENT_CA_PATH, pki.ca_cert)
        # They land on the device fs, byte-identical.
        assert t.device.fs.read(TLS_SERVER_CERT_PATH) == pki.server_cert
        assert t.device.fs.read(TLS_CLIENT_CA_PATH) == pki.ca_cert

    d = mtls.save_client_creds(pki, "touchypad-lab")
    assert (d / mtls.CA_CERT_FILE).read_bytes() == pki.ca_cert
