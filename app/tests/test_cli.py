"""CLI transport-selection tests (Stage 83 / Stage lb8).

Regression coverage for the CLI's ``_make_transport`` factory:

* ``touchy --port PATH`` must build a :class:`SerialTransport`. This used to
  import a misspelled module (``api._transponrt_serial``) and raise
  ``ImportError`` on every use.
* ``--url`` must build an :class:`HttpTransport`.
* With neither set, the CLI falls back to USB auto-discovery (``None``).
"""

from __future__ import annotations

import pytest

from touchy_pad import cli as cli_module
from touchy_pad.api import _transport_serial


class _FakeSerialTransport:
    """Stand-in for :class:`SerialTransport` so no real port is opened."""

    def __init__(self, port: str) -> None:
        self.port = port


def _make_transport(monkeypatch: pytest.MonkeyPatch, **obj: object):
    """Run the CLI's transport factory against a synthetic context object."""
    monkeypatch.setattr(_transport_serial, "SerialTransport", _FakeSerialTransport)
    ctx_obj = {"sim": False, "url": None, "port": None, **obj}
    with cli_module.cli.make_context("touchy", []) as ctx:
        ctx.obj = ctx_obj
        with ctx:
            return cli_module._make_transport()


def test_port_flag_builds_serial_transport(monkeypatch: pytest.MonkeyPatch) -> None:
    transport = _make_transport(monkeypatch, port="/dev/ttyUSB9")
    assert isinstance(transport, _FakeSerialTransport)
    assert transport.port == "/dev/ttyUSB9"


def test_url_flag_builds_http_transport(monkeypatch: pytest.MonkeyPatch) -> None:
    # HttpTransport connects lazily, so construction opens no socket.
    transport = _make_transport(monkeypatch, url="http://127.0.0.1:8083")
    assert transport is not None
    assert type(transport).__name__ == "HttpTransport"


def test_no_selection_falls_back_to_usb_autodiscovery(monkeypatch: pytest.MonkeyPatch) -> None:
    assert _make_transport(monkeypatch) is None
