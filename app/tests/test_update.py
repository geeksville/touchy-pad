"""Tests for the ``touchy update`` serial-port discovery.

Everything here is mocked: no USB device, no network, and no esptool run.
"""

from __future__ import annotations

import io
import sys
import types

import pytest
from rich.console import Console

from touchy_pad import update


def _glob_mapping(monkeypatch: pytest.MonkeyPatch, mapping: dict[str, list[str]]) -> None:
    def fake_glob(pattern: str) -> list[str]:
        return mapping.get(pattern, [])

    monkeypatch.setattr(update.glob, "glob", fake_glob)


def _allow(monkeypatch: pytest.MonkeyPatch, accessible: set[str]) -> None:
    monkeypatch.setattr(update.os, "access", lambda path, mode: path in accessible)


def test_darwin_families_in_order(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(update.sys, "platform", "darwin")
    _glob_mapping(
        monkeypatch,
        {
            "/dev/cu.usbmodem*": ["/dev/cu.usbmodem2", "/dev/cu.usbmodem1"],
            "/dev/cu.usbserial*": ["/dev/cu.usbserial1"],
            "/dev/cu.wchusbserial*": ["/dev/cu.wchusbserial1"],
            "/dev/cu.SLAB_USBtoUART*": ["/dev/cu.SLAB_USBtoUART1"],
        },
    )
    _allow(monkeypatch, {"/dev/cu.usbmodem2"})

    assert update._serial_port_candidates() == [
        "/dev/cu.usbmodem1",
        "/dev/cu.usbmodem2",
        "/dev/cu.usbserial1",
        "/dev/cu.wchusbserial1",
        "/dev/cu.SLAB_USBtoUART1",
    ]
    # First accessible candidate wins; usbmodem1 is not accessible.
    assert update._autodetect_serial_port() == "/dev/cu.usbmodem2"


def test_darwin_skips_inaccessible(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(update.sys, "platform", "darwin")
    _glob_mapping(
        monkeypatch,
        {"/dev/cu.usbmodem*": ["/dev/cu.usbmodem1", "/dev/cu.usbmodem2"]},
    )
    _allow(monkeypatch, {"/dev/cu.usbmodem2"})

    assert update._autodetect_serial_port() == "/dev/cu.usbmodem2"


def test_linux_host_dev_before_dev_and_acm_before_usb(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(update.sys, "platform", "linux")
    _glob_mapping(
        monkeypatch,
        {
            "/host/dev/ttyACM*": ["/host/dev/ttyACM0"],
            "/host/dev/ttyUSB*": ["/host/dev/ttyUSB0"],
            "/dev/ttyACM*": ["/dev/ttyACM0"],
            "/dev/ttyUSB*": ["/dev/ttyUSB0"],
        },
    )
    _allow(monkeypatch, {"/dev/ttyACM0", "/host/dev/ttyUSB0"})

    assert update._serial_port_candidates() == [
        "/host/dev/ttyACM0",
        "/host/dev/ttyUSB0",
        "/dev/ttyACM0",
        "/dev/ttyUSB0",
    ]
    assert update._autodetect_serial_port() == "/host/dev/ttyUSB0"


def test_linux_aliases_are_deduplicated(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(update.sys, "platform", "linux")
    _glob_mapping(
        monkeypatch,
        {
            "/host/dev/ttyACM*": ["/host/dev/ttyACM0"],
            "/dev/ttyACM*": ["/dev/ttyACM0"],
        },
    )
    monkeypatch.setattr(
        update.os.path, "realpath", lambda path: "/dev/ttyACM0" if "ttyACM0" in path else path
    )

    assert update._serial_port_candidates() == ["/host/dev/ttyACM0"]


def test_no_candidates(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(update.sys, "platform", "darwin")
    _glob_mapping(monkeypatch, {})

    assert update._serial_port_candidates() == []
    assert update._autodetect_serial_port() is None


def test_windows_stays_empty(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(update.sys, "platform", "win32")
    _glob_mapping(
        monkeypatch,
        {"/dev/ttyACM*": ["/dev/ttyACM0"]},
    )
    _allow(monkeypatch, {"/dev/ttyACM0"})

    assert update._serial_port_candidates() == []
    assert update._autodetect_serial_port() is None


def _do_flash_with_fakes(
    monkeypatch: pytest.MonkeyPatch,
    *,
    port: str | None,
    detected: str | None,
) -> list[str | None]:
    seen: list[str | None] = []

    def fake_download(console: Console, url: str, path) -> None:
        path.write_bytes(b"\x00" * 8)

    def fake_invoke(console: Console, *, firmware, port) -> None:
        seen.append(port)

    def fake_autodetect() -> str | None:
        return detected

    monkeypatch.setattr(update, "_download", fake_download)
    monkeypatch.setattr(update, "_invoke_esptool", fake_invoke)
    monkeypatch.setattr(update, "_autodetect_serial_port", fake_autodetect)
    monkeypatch.setattr(update, "_confirm_dangerous_board", lambda console, board: None)

    console = Console(file=io.StringIO())
    update._do_flash(console, board="jc4827w543", version="0.4.0", port=port)
    return seen


def test_explicit_port_wins(monkeypatch: pytest.MonkeyPatch) -> None:
    seen = _do_flash_with_fakes(monkeypatch, port="/dev/cu.usbmodem9", detected="/dev/cu.usbmodem1")
    assert seen == ["/dev/cu.usbmodem9"]


def test_autodetect_used_when_no_explicit_port(monkeypatch: pytest.MonkeyPatch) -> None:
    seen = _do_flash_with_fakes(monkeypatch, port=None, detected="/dev/cu.usbmodem1")
    assert seen == ["/dev/cu.usbmodem1"]


def test_no_port_falls_back_to_esptool_probe(monkeypatch: pytest.MonkeyPatch) -> None:
    seen = _do_flash_with_fakes(monkeypatch, port=None, detected=None)
    assert seen == [None]


def test_invoke_esptool_omits_port_when_none(monkeypatch: pytest.MonkeyPatch) -> None:
    calls: list[list[str]] = []

    fake = types.ModuleType("esptool")
    fake.main = lambda args: calls.append(args)  # type: ignore[attr-defined]
    fake.util = types.SimpleNamespace(FatalError=RuntimeError)  # type: ignore[attr-defined]
    monkeypatch.setitem(sys.modules, "esptool", fake)

    update._invoke_esptool(Console(file=io.StringIO()), firmware="/tmp/fw.bin", port=None)

    assert len(calls) == 1
    args = calls[0]
    assert "-p" not in args
    assert args[:5] == ["-b", "460800", "--before", "default-reset", "--after"]
    assert args[-3:] == ["write-flash", "0x0", "/tmp/fw.bin"]


def test_invoke_esptool_passes_port(monkeypatch: pytest.MonkeyPatch) -> None:
    calls: list[list[str]] = []

    fake = types.ModuleType("esptool")
    fake.main = lambda args: calls.append(args)  # type: ignore[attr-defined]
    fake.util = types.SimpleNamespace(FatalError=RuntimeError)  # type: ignore[attr-defined]
    monkeypatch.setitem(sys.modules, "esptool", fake)

    update._invoke_esptool(
        Console(file=io.StringIO()), firmware="/tmp/fw.bin", port="/dev/cu.usbmodem1"
    )

    assert calls[0][:3] == ["-p", "/dev/cu.usbmodem1", "-b"]
