"""Stage lb6 — BoardConfig preference + `pref from-template` CLI tests."""

from __future__ import annotations

from touchy_pad import _proto
from touchy_pad.cli import _apply_prefs_json, _list_templates, _templates_dir


def _load_template(name: str) -> _proto.PreferencesFile:
    data = (_templates_dir() / f"{name}.json").read_text(encoding="utf-8")
    from google.protobuf.json_format import Parse

    return Parse(data, _proto.PreferencesFile())


def test_led_32x8_template_parses_panel():
    prefs = _load_template("led-32x8")
    assert prefs.HasField("board_config")
    assert len(prefs.board_config.displays) == 1
    panels = prefs.board_config.displays[0].panels
    assert len(panels) == 1
    assert (panels[0].width, panels[0].height, panels[0].gpio) == (32, 8, 4)


def test_board_config_survives_serialize_roundtrip():
    prefs = _load_template("led-32x8")
    again = _proto.PreferencesFile()
    again.ParseFromString(prefs.SerializeToString())
    panel = again.board_config.displays[0].panels[0]
    assert (panel.width, panel.height, panel.gpio) == (32, 8, 4)


def test_led_32x8_listed_as_template():
    assert "led-32x8" in _list_templates()


def test_apply_prefs_json_sends_board_config_and_strips_file_version(monkeypatch):
    sent: list[_proto.PreferencesFile] = []

    class _FakeClient:
        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

        def set_preferences(self, prefs):
            sent.append(prefs)

    monkeypatch.setattr("touchy_pad.cli._client", lambda: _FakeClient())

    data = (_templates_dir() / "led-32x8.json").read_text(encoding="utf-8")
    _apply_prefs_json(data)

    assert len(sent) == 1
    prefs = sent[0]
    # Device-owned file_version must be stripped before sending (cleared to 0).
    assert prefs.file_version == _proto.PreferencesFile.Version.UNSPECIFIED
    panel = prefs.board_config.displays[0].panels[0]
    assert (panel.width, panel.height, panel.gpio) == (32, 8, 4)
