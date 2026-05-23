"""Stage-30 step 6 smoke tests for the PySide6 device simulator window.

These tests exercise the renderer and click → action dispatch end-to-end
without opening a real screen: Qt is forced into its ``offscreen``
platform plugin via ``QT_QPA_PLATFORM`` so the suite runs on CI / dev
containers without a display server.

PySide6 is an optional dependency (``pip install 'touchy-pad[sim]'``),
so the whole module is skipped when it isn't available.
"""

from __future__ import annotations

import os
import pathlib
import queue

import pytest

pytest.importorskip("PySide6")

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6 import QtWidgets  # noqa: E402
from PySide6.QtWidgets import QCheckBox, QPushButton, QSlider  # noqa: E402

from touchy_pad.api.images import make_smiley_png  # noqa: E402
from touchy_pad.api.screens import build_demo_screens  # noqa: E402
from touchy_pad.sim.device import SimDevice  # noqa: E402
from touchy_pad.sim.fs import SimFs  # noqa: E402
from touchy_pad.sim.window import SimWindow  # noqa: E402


@pytest.fixture(scope="session")
def qapp() -> QtWidgets.QApplication:
    """Provide a single QApplication for the whole test session.

    Qt forbids creating more than one QApplication per process, so we
    reuse a session-scoped instance across all sim-window tests.
    """
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


@pytest.fixture
def provisioned_fs(tmp_path: pathlib.Path) -> SimFs:
    fs = SimFs(tmp_path, serial="SIMTEST")
    fs.save("F:host/images/smiley.png", make_smiley_png())
    for s in build_demo_screens():
        fs.save(
            f"F:host/screens/{s.name}.pb",
            s.to_proto().SerializeToString(),
        )
    return fs


def _drain(q: queue.Queue) -> list:
    out: list = []
    try:
        while True:
            out.append(q.get_nowait())
    except queue.Empty:
        pass
    return out


def test_renders_initial_screen(qapp, provisioned_fs) -> None:
    """The first registered screen auto-loads and its widgets are rendered."""
    dev = SimDevice(provisioned_fs)
    win = SimWindow(dev, size=(480, 300))
    qapp.processEvents()

    assert dev.active_screen_path == "F:host/screens/home.pb"
    labels = [b.text() for b in win.findChildren(QPushButton)]
    assert "< Prev" in labels and "Next >" in labels


def test_next_button_dispatches_switch_screen(qapp, provisioned_fs) -> None:
    """Clicking ``Next >`` fires an ``ActionSwitchScreen`` (behavior NEXT)."""
    dev = SimDevice(provisioned_fs)
    win = SimWindow(dev, size=(480, 300))
    qapp.processEvents()

    nxt = next(b for b in win.findChildren(QPushButton) if "Next" in b.text())
    nxt.click()
    qapp.processEvents()

    assert dev.active_screen_path == "F:host/screens/test.pb"


def test_host_action_pushes_event(qapp, provisioned_fs) -> None:
    """``ActionHost`` widgets enqueue ``LvEvent`` rows with the right code."""
    dev = SimDevice(provisioned_fs)
    win = SimWindow(dev, size=(480, 300))
    qapp.processEvents()

    # Hop to the test screen which has the host-action widgets.
    nxt = next(b for b in win.findChildren(QPushButton) if "Next" in b.text())
    nxt.click()
    qapp.processEvents()

    ping = next(b for b in win.findChildren(QPushButton) if "Ping" in b.text())
    ping.click()
    # Move the slider; pick the first one (test screen has exactly one).
    sl = win.findChildren(QSlider)[0]
    sl.setValue(sl.value() + 5)
    # Toggle the first checkbox we find (skip the ``Force`` one by id).
    enable_cb = next(cb for cb in win.findChildren(QCheckBox) if cb.text() == "Enabled")
    enable_cb.toggle()
    qapp.processEvents()

    evts = _drain(dev._events)
    by_code = {e.host_code: e for e in evts}
    assert 0x100 in by_code, evts
    assert by_code[0x100].user_data == "ping"
    assert 0x101 in by_code
    assert by_code[0x101].value == sl.value()
    assert 0x102 in by_code
    assert by_code[0x102].user_data == "enable"


def test_button_press_release_edges_dispatch(qapp, tmp_path) -> None:
    """`on_press` / `on_release` produce two LvEvents with distinguishable codes.

    Stage 50.2: the firmware now attaches actions on `LV_EVENT_PRESSED` and
    `LV_EVENT_RELEASED` as well as `LV_EVENT_CLICKED`. The sim mirrors that:
    a single Qt push activation should enqueue three host events when the
    same `host_code` is wired to all three slots, and we should be able to
    tell them apart by `LvEvent.code` (1=PRESSED, 7=CLICKED, 8=RELEASED).
    """
    from touchy_pad.api.screens import Screen, button, host_action

    fs = SimFs(tmp_path, serial="SIMEDGE")
    s = Screen("edges")
    s += button(
        "pad",
        text="Pad",
        on_click=host_action(0xABC),
        on_press=host_action(0xABC),
        on_release=host_action(0xABC),
    )
    fs.save("F:host/screens/edges.pb", s.to_proto().SerializeToString())

    dev = SimDevice(fs)
    win = SimWindow(dev, size=(320, 200))
    qapp.processEvents()
    assert dev.active_screen_path == "F:host/screens/edges.pb"

    pad = next(b for b in win.findChildren(QPushButton) if b.text() == "Pad")
    pad.click()
    qapp.processEvents()

    evts = _drain(dev._events)
    codes = sorted(e.code for e in evts if e.host_code == 0xABC)
    # Qt's QAbstractButton.click() emits pressed → released → clicked.
    assert codes == [1, 7, 8], codes
    for e in evts:
        if e.host_code == 0xABC:
            assert e.user_data == "pad"
