"""Stage-30 step 2 smoke test for the headless device simulator.

Drives the sim through :class:`TouchyClient` end-to-end:

* sys_board_info_get returns CURRENT protocol + ``firmware_version_str == 'sim'``.
* file_save persists into the sandboxed pseudo-fs.
* screen_load decodes the uploaded screen and exposes it as the active
  screen.
* On startup with an empty fs the embedded default screen loads.
* event_consume drains injected host events.
"""

from __future__ import annotations

import pathlib

from touchy_pad import TouchyClient, _proto
from touchy_pad.api.screens import build_demo_screens
from touchy_pad.sim.transport import SimDeviceTransport, make_tempdir_transport


def test_sysversion_reports_sim() -> None:
    with make_tempdir_transport() as t:
        c = TouchyClient(t)
        v = c.sys_board_info_get()
    assert v.firmware_version_str == "sim"
    assert v.protocol_version == _proto.SysBoardInfoResponse.ProtocolVersion.CURRENT
    assert v.board_name == "sim"


def test_default_screen_autoloads_on_empty_fs() -> None:
    with make_tempdir_transport() as t:
        # The sim's default screen comes from proto/default_screen.json.
        assert t.device.active_screen_path == "<built-in>"
        active = t.device.active_screen
        assert active is not None


def test_file_save_and_screen_load_roundtrip(tmp_path: pathlib.Path) -> None:
    t = SimDeviceTransport(headless=True, fs_root=tmp_path)
    try:
        with TouchyClient(t) as c:
            home, test = build_demo_screens()
            for s in (home, test):
                c.file_save(
                    f"F:host/screens/{s.name}.pb",
                    s.to_proto().SerializeToString(),
                )
            c.screen_load("F:host/screens/test.pb")
        active = t.device.active_screen
        assert active is not None
        # The two screens should now be listed in lex order on disk.
        assert t.device.list_screens() == [
            "F:host/screens/home.pb",
            "F:host/screens/test.pb",
        ]
    finally:
        t.close()


def test_screen_load_missing_returns_not_found() -> None:
    with make_tempdir_transport() as t:
        c = TouchyClient(t)
        # _check raises on non-OK; assert the underlying response code.
        reply = c._rpc(
            _proto.Command(screen_load=_proto.ScreenLoadCmd(path="F:host/screens/nope.pb"))
        )
        assert reply.code == _proto.RESULT_NOT_FOUND


def test_event_consume_drains_queue() -> None:
    with make_tempdir_transport() as t:
        t.device.push_host_event(host_code=0x100, widget_id="ping")
        with TouchyClient(t) as c:
            evt = c.event_consume()
            assert evt is not None
            assert evt.host_code == 0x100
            assert evt.user_data == "ping"
            # Queue is now empty.
            assert c.event_consume() is None


def test_image_bytes_are_not_converted_for_sim() -> None:
    """``needs_image_conversion=False`` means PNG bytes go through verbatim
    and the path keeps its original extension."""
    with make_tempdir_transport() as t:
        from touchy_pad.api.images import make_smiley_png

        smiley = make_smiley_png()
        with TouchyClient(t) as c:
            c.file_save("F:host/images/smiley.png", smiley)
        # File landed under its original name (no .bin rewrite) and
        # contents match the source PNG exactly.
        assert t._fs.read("F:host/images/smiley.png") == smiley
