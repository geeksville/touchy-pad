"""Tests for ``touchy_pad._usb.find_usb_devices`` — the fresh-context retry.

Pure host-side and hardware-free: pyusb and its libusb backend are faked, so
these also run on Windows/macOS CI where libusb doesn't exist.
"""

from __future__ import annotations

import pytest

from touchy_pad import _usb


class FakeDevice:
    def __init__(self, name: str = "dev") -> None:
        self.name = name

    def __iter__(self):  # pyusb's Device iterates its configurations!
        yield from ("config-a", "config-b")


class FakeCore:
    """Records ``find()`` kwargs and returns scripted results."""

    def __init__(self, results: list[object], *, raise_on_first: bool = False) -> None:
        self._results = list(results)
        self._raise_on_first = raise_on_first
        self.calls: list[dict] = []

    def find(self, **kwargs):
        self.calls.append(kwargs)
        if self._raise_on_first and len(self.calls) == 1:
            raise RuntimeError("NoBackendError-ish")
        return self._results.pop(0) if self._results else None


@pytest.fixture
def fake_backend(monkeypatch: pytest.MonkeyPatch) -> list[object]:
    """Make ``_fresh_backend()`` hand back a fresh fake backend instance."""
    from usb.backend import libusb1

    created: list[object] = []

    class FakeBackend:
        pass

    def _make(lib: object) -> FakeBackend:
        backend = FakeBackend()
        backend.lib = lib  # type: ignore[attr-defined]
        created.append(backend)
        return backend

    monkeypatch.setattr(libusb1, "_lib", object(), raising=False)
    monkeypatch.setattr(libusb1, "_LibUSB", _make, raising=False)
    return created


def test_cached_enumeration_wins_and_no_new_context_is_made(
    fake_backend: list[object],
) -> None:
    dev = FakeDevice()
    core = FakeCore([dev])
    assert _usb.find_usb_devices(core, 0x1234, 0x5678) == [dev]
    assert len(core.calls) == 1  # only the cached pass
    assert "backend" not in core.calls[0]
    assert fake_backend == []  # …so no libusb context was created


def test_single_device_is_not_expanded_by_find_all_logic(fake_backend: list[object]) -> None:
    """pyusb's Device is iterable over configs — a single hit must stay single."""
    dev = FakeDevice()
    assert _usb.find_usb_devices(FakeCore([dev]), 0x1234, 0x5678) == [dev]


def test_retries_with_a_fresh_context_when_the_cache_is_stale(
    fake_backend: list[object],
) -> None:
    dev = FakeDevice("found-later")
    core = FakeCore([None, dev])  # stale context first, real device second
    assert _usb.find_usb_devices(core, 0x303A, 0x8369) == [dev]
    assert len(core.calls) == 2
    assert core.calls[1]["backend"] is fake_backend[0]
    # The fresh context is pinned to the device so it can't be collected.
    assert dev._touchy_usb_context is fake_backend[0]  # type: ignore[attr-defined]


def test_find_all_returns_every_fresh_match(fake_backend: list[object]) -> None:
    d1, d2 = FakeDevice("a"), FakeDevice("b")
    core = FakeCore([[], [d1, d2]])
    assert _usb.find_usb_devices(core, 0x303A, 0x8369, find_all=True) == [d1, d2]


def test_no_libusb_returns_empty(monkeypatch: pytest.MonkeyPatch) -> None:
    from usb.backend import libusb1

    monkeypatch.setattr(libusb1, "_lib", None, raising=False)
    monkeypatch.setattr(libusb1, "get_backend", lambda *a, **k: None, raising=False)
    core = FakeCore([None])
    assert _usb.find_usb_devices(core, 0x303A, 0x8369) == []
    assert len(core.calls) == 1  # nothing to retry with


def test_backend_construction_failure_returns_empty(monkeypatch: pytest.MonkeyPatch) -> None:
    from usb.backend import libusb1

    def _boom(lib: object):  # noqa: ANN202
        raise OSError("libusb_init failed")

    monkeypatch.setattr(libusb1, "_lib", object(), raising=False)
    monkeypatch.setattr(libusb1, "_LibUSB", _boom, raising=False)
    assert _usb.find_usb_devices(FakeCore([None]), 0x303A, 0x8369) == []


def test_enumeration_errors_are_swallowed(fake_backend: list[object]) -> None:
    """A raising pyusb (Windows without libusb) is just "no devices"."""
    assert _usb.find_usb_devices(FakeCore([], raise_on_first=True), 0x303A, 0x8369) == []


def test_fresh_backend_never_raises() -> None:
    """Whatever the host has, asking for a backend is safe (None is fine)."""
    _usb._fresh_backend()
