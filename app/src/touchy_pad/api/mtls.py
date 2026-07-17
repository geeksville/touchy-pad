"""Mutual-TLS (mTLS) key/cert generation and client credential storage.

Stage lb9 secures the network API (the HTTP command endpoint from lb8)
with mutual TLS instead of the abandoned TLS-PSK idea. We run our own
throwaway one-shot Certificate Authority (CA): it signs exactly two
certificates — one for the **device** (server) and one for the **host**
(client). Both sides verify against the CA, so only a client holding a
CA-signed cert can connect, and the client only talks to a device holding
a CA-signed cert.

This module:

* generates the PKI (:func:`generate_pki`) with the ``cryptography``
  library — an EC P-256 CA plus a server and a client leaf cert;
* stores the *host* half (client cert + key + CA cert) under a
  per-endpoint directory in the user config dir
  (:func:`save_client_creds` / :func:`client_creds_dir`);
* builds a ready-to-use client :class:`ssl.SSLContext`
  (:func:`load_client_context`) that presents the client cert and
  verifies the device cert against the CA.

The device half (server cert + key + CA cert) is pushed to the device
over USB by ``touchy pref provision-mtls`` (see
:mod:`touchy_pad.cli`), which writes them as files under
:data:`touchy_pad.paths.TLS_DIR` via the normal FileWrite API.

Design notes / decisions (see docs/hardware/lightbar/design.md lb9):

* **Single client cert.** No multi-client / ``--add-client`` support; one
  host credential per provisioning. Re-provision to rotate/revoke.
* **Long-lived certs** (10 years). "Revocation" = re-provision (a fresh CA
  invalidates the old cert set).
* **``check_hostname=False``** on the client: a gadget's IP/mDNS name
  drifts, so we verify the CA signature (the identity guarantee) but not
  the hostname. The server cert still carries a SAN for future pinning.
* **Losing the host creds = lockout**; recover by re-provisioning over USB.
"""

from __future__ import annotations

import datetime
import ssl
from dataclasses import dataclass
from pathlib import Path

# Filenames used both in the host creds dir and (client_ca aside) on-device.
CLIENT_CERT_FILE = "client.crt"
CLIENT_KEY_FILE = "client.key"
CA_CERT_FILE = "ca.crt"

#: Cert validity window (10 years). Long-lived by design; re-provision to
#: rotate. A little backdating avoids clock-skew "not yet valid" failures.
_VALIDITY_DAYS = 3650


@dataclass
class Pki:
    """A freshly-generated mTLS certificate set (all PEM ``bytes``)."""

    ca_cert: bytes
    server_cert: bytes
    server_key: bytes
    client_cert: bytes
    client_key: bytes


def generate_pki(
    *,
    device_cn: str = "touchy-pad",
    device_san: str | None = None,
    client_cn: str = "touchy-host",
) -> Pki:
    """Generate a one-shot CA plus a device (server) and host (client) cert.

    Uses EC P-256 keys (small + fast, well supported by mbedtls on the
    device). *device_san* — if given — is added as a DNS SAN on the server
    cert (e.g. the mDNS hostname) for optional future hostname pinning; it
    does not affect today's ``check_hostname=False`` client.
    """
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID

    now = datetime.datetime.now(datetime.timezone.utc)
    not_before = now - datetime.timedelta(minutes=5)
    not_after = now + datetime.timedelta(days=_VALIDITY_DAYS)

    def _key_pem(key) -> bytes:
        return key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.PKCS8,
            encryption_algorithm=serialization.NoEncryption(),
        )

    def _cert_pem(cert) -> bytes:
        return cert.public_bytes(serialization.Encoding.PEM)

    # --- CA (self-signed, can sign other certs) --------------------------
    ca_key = ec.generate_private_key(ec.SECP256R1())
    ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "touchy-pad mTLS CA")])
    ca_cert = (
        x509.CertificateBuilder()
        .subject_name(ca_name)
        .issuer_name(ca_name)
        .public_key(ca_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(not_before)
        .not_valid_after(not_after)
        .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
        .add_extension(
            x509.KeyUsage(
                digital_signature=False,
                content_commitment=False,
                key_encipherment=False,
                data_encipherment=False,
                key_agreement=False,
                key_cert_sign=True,
                crl_sign=True,
                encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        )
        .sign(ca_key, hashes.SHA256())
    )

    def _leaf(cn: str, *, server: bool, san: str | None) -> tuple[bytes, bytes]:
        key = ec.generate_private_key(ec.SECP256R1())
        eku = x509.ExtendedKeyUsage(
            [x509.oid.ExtendedKeyUsageOID.SERVER_AUTH]
            if server
            else [x509.oid.ExtendedKeyUsageOID.CLIENT_AUTH]
        )
        builder = (
            x509.CertificateBuilder()
            .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)]))
            .issuer_name(ca_name)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(not_before)
            .not_valid_after(not_after)
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(eku, critical=False)
        )
        if san:
            builder = builder.add_extension(
                x509.SubjectAlternativeName([x509.DNSName(san)]), critical=False
            )
        cert = builder.sign(ca_key, hashes.SHA256())
        return _cert_pem(cert), _key_pem(key)

    server_cert, server_key = _leaf(device_cn, server=True, san=device_san)
    client_cert, client_key = _leaf(client_cn, server=False, san=None)

    return Pki(
        ca_cert=_cert_pem(ca_cert),
        server_cert=server_cert,
        server_key=server_key,
        client_cert=client_cert,
        client_key=client_key,
    )


# --- host-side credential storage ---------------------------------------


def _config_root() -> Path:
    """Root of the per-user touchy config dir (``~/.config/touchy-pad`` etc.)."""
    try:
        import platformdirs

        return Path(platformdirs.user_config_dir("touchy-pad"))
    except Exception:  # noqa: BLE001 — platformdirs should be present, but be safe.
        return Path.home() / ".config" / "touchy-pad"


def _sanitize_key(endpoint: str | None) -> str:
    """Turn a URL/host into a filesystem-safe creds subdir name.

    ``None`` / empty maps to ``"default"``. A full ``https://host:port``
    URL is reduced to ``host`` so the same creds are found whether the
    caller passes the URL or just the host.
    """
    if not endpoint:
        return "default"
    from urllib.parse import urlsplit

    host = endpoint
    if "://" in endpoint:
        host = urlsplit(endpoint).hostname or endpoint
    else:
        host = endpoint.split("/", 1)[0].split(":", 1)[0]
    safe = "".join(c if (c.isalnum() or c in "._-") else "_" for c in host)
    return safe or "default"


def client_creds_dir(endpoint: str | None = None) -> Path:
    """Directory holding the host's client cert/key + CA for *endpoint*."""
    return _config_root() / "mtls" / _sanitize_key(endpoint)


def save_client_creds(pki: Pki, endpoint: str | None = None) -> Path:
    """Persist the host (client) credentials for later connections.

    Writes ``client.crt`` / ``client.key`` / ``ca.crt`` (key mode 0600)
    into :func:`client_creds_dir`. Returns that directory.
    """
    d = client_creds_dir(endpoint)
    d.mkdir(parents=True, exist_ok=True)
    (d / CLIENT_CERT_FILE).write_bytes(pki.client_cert)
    (d / CA_CERT_FILE).write_bytes(pki.ca_cert)
    key_path = d / CLIENT_KEY_FILE
    key_path.write_bytes(pki.client_key)
    try:
        key_path.chmod(0o600)
    except OSError:
        pass  # best-effort on platforms without POSIX modes
    return d


def load_client_context(endpoint: str | None = None) -> ssl.SSLContext:
    """Build a client :class:`ssl.SSLContext` for mTLS to *endpoint*.

    Presents the stored client cert and verifies the device's server cert
    against the stored CA. ``check_hostname`` is disabled (the device's
    IP/mDNS name drifts; the CA signature is the identity guarantee).

    Raises :class:`FileNotFoundError` (with a provisioning hint) when no
    credentials have been saved for *endpoint* (falling back to the
    ``default`` creds dir if the endpoint-specific one is absent).
    """
    d = client_creds_dir(endpoint)
    if not (d / CLIENT_CERT_FILE).exists():
        fallback = client_creds_dir(None)
        if fallback != d and (fallback / CLIENT_CERT_FILE).exists():
            d = fallback
        else:
            raise FileNotFoundError(
                f"no mTLS client credentials for {endpoint or '(default)'} in {d}. "
                f"Run 'touchy pref provision-mtls' over USB first."
            )
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.load_cert_chain(certfile=str(d / CLIENT_CERT_FILE), keyfile=str(d / CLIENT_KEY_FILE))
    ctx.load_verify_locations(cafile=str(d / CA_CERT_FILE))
    ctx.verify_mode = ssl.CERT_REQUIRED
    # The device cert is CA-verified, but its CN/SAN won't match the
    # volatile IP/mDNS name we dial, so don't enforce hostname matching.
    ctx.check_hostname = False
    return ctx


__all__ = [
    "CA_CERT_FILE",
    "CLIENT_CERT_FILE",
    "CLIENT_KEY_FILE",
    "Pki",
    "client_creds_dir",
    "generate_pki",
    "load_client_context",
    "save_client_creds",
]
