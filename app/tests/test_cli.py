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
from click.testing import CliRunner

from touchy_pad import _proto
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


# ---------------------------------------------------------------------------
# `touchy property set` — Stage lb19's --int flag.
# ---------------------------------------------------------------------------


class _FakePropertyClient:
    """Captures the ``SetPropertiesCmd`` batch the CLI sends."""

    def __init__(self, sent: list) -> None:
        self._sent = sent

    def __enter__(self):
        return self

    def __exit__(self, *exc: object) -> bool:
        return False

    def set_properties(self, cmds) -> None:
        self._sent.extend(cmds)


def _run_property_set(monkeypatch: pytest.MonkeyPatch, argv: list[str]) -> tuple[list, object]:
    sent: list = []
    monkeypatch.setattr(cli_module, "_client", lambda: _FakePropertyClient(sent))
    result = CliRunner().invoke(cli_module.cli, ["property", "set", *argv])
    return sent, result


def test_property_set_int_flag_sends_an_integer(monkeypatch: pytest.MonkeyPatch) -> None:
    """`--int` reaches LVGL *style* properties (e.g. a label's scroll speed).

    Without it the CLI can only send strings, which the device would hand to
    LVGL as a pointer — so `anim_duration` (Stage lb19's ms-per-lap scroll
    knob) was unreachable from the CLI before this flag existed.
    """
    sent, result = _run_property_set(monkeypatch, ["ticker", "anim_duration", "4000", "--int"])
    assert result.exit_code == 0, result.output
    assert len(sent) == 1
    cmd = sent[0]
    assert cmd.widget_id == "ticker"
    assert cmd.property_name == "anim_duration"
    assert cmd.WhichOneof("value") == "int_value"
    assert cmd.int_value == 4000


def test_property_set_defaults_to_a_string(monkeypatch: pytest.MonkeyPatch) -> None:
    """The default form is unchanged: multi-word VALUE → one string."""
    sent, result = _run_property_set(monkeypatch, ["welcome", "text", "Hello", "world"])
    assert result.exit_code == 0, result.output
    assert sent[0].WhichOneof("value") == "string_value"
    assert sent[0].string_value == "Hello world"


def test_property_set_int_flag_rejects_a_non_number(monkeypatch: pytest.MonkeyPatch) -> None:
    sent, result = _run_property_set(monkeypatch, ["ticker", "anim_duration", "abc", "--int"])
    assert result.exit_code != 0
    assert "must be an integer" in result.output
    assert sent == []


# ---------------------------------------------------------------------------
# `touchy screen demo` — page switching must respect the board's touch support.
# ---------------------------------------------------------------------------


class _FakePad:
    """Recording stand-in for :class:`touchy_pad.api.Touchy`."""

    def __init__(self, *, touchable: bool) -> None:
        info = _proto.SysBoardInfoResponse()
        # proto3 bools default to False, so `is_touchable` must be set for the
        # touch-capable case; display size is only read on the touch-less path.
        info.is_touchable = touchable
        info.display_width = 32
        info.display_height = 8
        self.board_info = info
        self.calls: list[tuple[str, object]] = []

    def __enter__(self):
        return self

    def __exit__(self, *exc: object) -> bool:
        return False

    def screen_save(self, screen, name=None) -> None:
        self.calls.append(("screen_save", name))

    def screen_load(self, path: str) -> None:
        self.calls.append(("screen_load", path))

    def user_screen_save(self, name: str, widget) -> None:
        self.calls.append(("user_screen_save", name))

    def file_save(self, path: str, data, **kwargs) -> None:
        self.calls.append(("file_save", path))

    def show_user_screen(self, name: str, **kwargs) -> None:
        self.calls.append(("show_user_screen", name))


def _run_screen_demo(monkeypatch: pytest.MonkeyPatch, pad: _FakePad):
    monkeypatch.setattr(cli_module, "_open_pad", lambda: pad)
    return CliRunner().invoke(cli_module.cli, ["screen", "demo"])


def test_screen_demo_skips_page_switch_on_touch_less_boards(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """LED-matrix boards get the animated setup screen — no `page` ref.

    `_do_screen_init` writes the touch-less setup screen there, which has no
    ``widget_ref(id="page")``. Paging it made the device log
    ``change_widget_ref failed`` on every `screen demo`, so the command must
    upload the demo page without trying to show it.
    """
    pad = _FakePad(touchable=False)
    result = _run_screen_demo(monkeypatch, pad)

    assert result.exit_code == 0, result.output
    assert ("user_screen_save", "test") in pad.calls
    assert not [c for c in pad.calls if c[0] == "show_user_screen"]


def test_screen_demo_shows_the_page_on_touch_boards(monkeypatch: pytest.MonkeyPatch) -> None:
    """The chrome screen *does* have a `page` ref, so the demo page is shown."""
    pad = _FakePad(touchable=True)
    result = _run_screen_demo(monkeypatch, pad)

    assert result.exit_code == 0, result.output
    assert ("show_user_screen", "test") in pad.calls
    # …and the chrome + trackpad baseline was written first.
    assert ("user_screen_save", "trackpad") in pad.calls
