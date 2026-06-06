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
from touchy_pad.api.screens import build_demo
from touchy_pad.sim.transport import SimDeviceTransport, make_tempdir_transport


def test_sysversion_reports_sim() -> None:
    with make_tempdir_transport() as t:
        c = TouchyClient(t)
        v = c.sys_board_info_get()
    assert v.firmware_version_str == "sim"
    assert v.protocol_version == _proto.SysBoardInfoResponse.ProtocolVersion.CURRENT
    assert v.board_name == "sim"


def test_board_info_capabilities_default_full() -> None:
    """By default the sim emulates a full-featured device (multitouch + USB)."""
    with make_tempdir_transport() as t:
        c = TouchyClient(t)
        v = c.sys_board_info_get()
    assert v.is_multitouch is True
    assert v.has_usb is True


def test_board_info_reports_sim_serial() -> None:
    """Stage 71 — the sim reports a stable sentinel serial."""
    with make_tempdir_transport() as t:
        c = TouchyClient(t)
        v = c.sys_board_info_get()
    assert v.serial == "tsim001"


def test_board_info_reports_memory_and_storage() -> None:
    """Stage 81 — the sim advertises plausible free RAM / PSRAM / FS values."""
    with make_tempdir_transport() as t:
        c = TouchyClient(t)
        v = c.sys_board_info_get()
    assert v.free_heap_bytes > 0
    assert v.free_psram_bytes > 0
    assert v.fs_total_bytes >= v.fs_used_bytes > 0


def test_set_preferences_partial_update_merges() -> None:
    """Stage 82 — a partial update only changes the fields it carries."""
    with make_tempdir_transport() as t:
        c = TouchyClient(t)
        # Only set the boot delay; the log level keeps its default.
        c.set_boot_delay(7)
        dev = t._device  # noqa: SLF001 — white-box check of merged prefs
        assert dev._prefs.boot_delay_s == 7  # noqa: SLF001
        assert dev._prefs.min_log_level == _proto.LOG_PRIORITY_ERROR  # noqa: SLF001
        # A second partial update leaves the first field intact.
        c.set_min_log_level(_proto.LOG_PRIORITY_DEBUG)
        assert dev._prefs.boot_delay_s == 7  # noqa: SLF001
        assert dev._prefs.min_log_level == _proto.LOG_PRIORITY_DEBUG  # noqa: SLF001


def test_run_actions_runs_host_action_headless(tmp_path: pathlib.Path) -> None:
    """Stage 71 — RunActionsCmd with no GUI dispatcher runs ActionHost inline."""
    from touchy_pad.sim.device import SimDevice
    from touchy_pad.sim.fs import SimFs

    dev = SimDevice(SimFs(tmp_path, "sim"))
    act = _proto.Action(host=_proto.ActionHost(code=0xB001))
    cmd = _proto.Command(run_actions=_proto.RunActionsCmd(actions=[act]))
    reply = _proto.Response()
    reply.ParseFromString(dev.handle_command(cmd.SerializeToString()))
    assert reply.code == _proto.RESULT_OK
    # The host action should have queued an LvEvent the host can drain.
    evt_cmd = _proto.Command(event_consume=_proto.EventConsumeCmd())
    evt_reply = _proto.Response()
    evt_reply.ParseFromString(dev.handle_command(evt_cmd.SerializeToString()))
    assert evt_reply.event_consume.event.host_code == 0xB001

    with make_tempdir_transport() as t:
        c = TouchyClient(t)
        v = c.sys_board_info_get()
    assert v.is_multitouch is True
    assert v.has_usb is True


def test_board_info_capabilities_configurable(tmp_path: pathlib.Path) -> None:
    """Stage 65 — caps are configurable so the sim can emulate the CYD."""
    from touchy_pad.sim.device import SimDevice
    from touchy_pad.sim.fs import SimFs

    dev = SimDevice(SimFs(tmp_path, "cyd"), is_multitouch=False, has_usb=False)
    cmd = _proto.Command(sys_board_info_get=_proto.SysBoardInfoGetCmd())
    reply = _proto.Response()
    reply.ParseFromString(dev.handle_command(cmd.SerializeToString()))
    assert reply.sys_board_info.is_multitouch is False
    assert reply.sys_board_info.has_usb is False


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
            screen, widgets = build_demo()
            c.file_save(
                f"F:host/s/{screen.name}.pb",
                screen.to_proto().SerializeToString(),
            )
            for name, w in widgets:
                c.file_save(f"F:host/uscr/{name}.pb", w.SerializeToString())
            c.screen_load(f"F:host/s/{screen.name}.pb")
        active = t.device.active_screen
        assert active is not None
        assert t.device.list_screens() == [
            "F:host/s/default.pb",
        ]
    finally:
        t.close()


def test_screen_load_missing_returns_not_found() -> None:
    with make_tempdir_transport() as t:
        c = TouchyClient(t)
        # _check raises on non-OK; assert the underlying response code.
        # Stage 82 — screen load goes through SetPreferencesCmd.
        reply = c._rpc(
            _proto.Command(
                set_preferences=_proto.SetPreferencesCmd(
                    prefs=_proto.PreferencesFile(current_screen="F:host/s/nope.pb")
                )
            )
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
