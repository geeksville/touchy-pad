"""PySide6 main window for the Touchy-Pad device simulator.

:class:`SimWindow` renders the active screen of a :class:`SimDevice`
inside a fixed-size canvas alongside a small log panel. Screen
switches (from on-screen ``Prev``/``Next`` buttons, ``screen load``
RPCs, etc.) trigger a re-render via a cross-thread signal.

Click handling is deferred to :mod:`touchy_pad.sim.window` rather than
sitting inside :mod:`touchy_pad.sim.widgets` so the renderer stays
pure: the window walks each clicked widget's ``on_click`` actions and
either dispatches them to the device (widget_ref rebinds), echoes them to
the log panel (macros), or feeds them back into the host-event queue
(host actions).

Top / sys / bottom LVGL layers are rendered as transparent overlays
above the active screen so the header chrome from
:func:`touchy_pad.api.screens.build_demo` shows up correctly.
"""

from __future__ import annotations

import logging

from PySide6 import QtCore, QtWidgets

from .. import _proto
from .device import SimDevice
from .widgets import _ImageButton, build_widget

_log = logging.getLogger("touchy_pad.sim")


class _Canvas(QtWidgets.QFrame):
    """Fixed-size frame that hosts the active+overlay screen widgets.

    Children are placed at full canvas size via ``setGeometry`` so
    layers stack correctly; the canvas itself paints a dark background
    so the device's "screen area" is visually distinct from window chrome.
    """

    def __init__(self, size: tuple[int, int], parent: QtWidgets.QWidget) -> None:
        super().__init__(parent)
        self._w, self._h = size
        self.setFixedSize(self._w, self._h)
        self.setStyleSheet("QFrame { background: #101010; }")
        self.setFrameShape(QtWidgets.QFrame.Box)

    def add_layer(self, widget: QtWidgets.QWidget, *, transparent_to_mouse: bool = False) -> None:
        widget.setParent(self)
        widget.setGeometry(0, 0, self._w, self._h)
        widget.setAttribute(QtCore.Qt.WA_TranslucentBackground, True)
        # Overlay layers (top / bottom / sys) cover the whole canvas
        # but only their concrete child widgets are interactive — the
        # container itself must let mouse events fall through to the
        # active layer below, otherwise empty cells of e.g. the top
        # navigation grid swallow every click. Children with their
        # own mouse handlers (buttons, sliders, ...) keep receiving
        # events because WA_TransparentForMouseEvents does not
        # propagate to child widgets.
        if transparent_to_mouse:
            widget.setAttribute(QtCore.Qt.WA_TransparentForMouseEvents, True)
        widget.show()
        widget.raise_()

    def clear_layers(self) -> None:
        for child in self.findChildren(QtWidgets.QWidget, options=QtCore.Qt.FindDirectChildrenOnly):
            child.setParent(None)
            child.deleteLater()


class SimWindow(QtWidgets.QMainWindow):
    """Main window: device-screen canvas + side log panel."""

    # Cross-thread relay for SimDevice → window updates. SimDevice's
    # callback fires on whatever thread runs the command worker; this
    # signal hops back to the Qt main thread before re-rendering.
    _screen_changed = QtCore.Signal(object)
    _image_updated = QtCore.Signal(str)  # path of updated image file

    def __init__(
        self,
        device: SimDevice,
        *,
        size: tuple[int, int] = (480, 300),
        parent: QtWidgets.QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self._device = device
        self._size = size
        # Stage 57: in-RAM widget_ref rebindings keyed by outer Widget.id.
        # Cleared on every screen load (matches firmware semantics: the
        # encoded path takes effect again as soon as the screen is reloaded).
        self._widget_ref_overrides: dict[str, str] = {}
        # Track image widgets by their asset path so we can reload pixmaps
        # without rebuilding the widget tree (which would break mouse tracking).
        self._image_widgets: dict[str, list[_ImageButton]] = {}

        self.setWindowTitle("touchy-pad sim")
        self.resize(size[0] + 260, max(size[1] + 40, 360))

        central = QtWidgets.QWidget(self)
        self.setCentralWidget(central)
        outer = QtWidgets.QHBoxLayout(central)
        outer.setContentsMargins(8, 8, 8, 8)
        outer.setSpacing(8)

        # ── screen canvas ───────────────────────────────────────────
        self._canvas = _Canvas(size, central)
        outer.addWidget(self._canvas, 0, QtCore.Qt.AlignTop | QtCore.Qt.AlignLeft)

        # ── log panel ──────────────────────────────────────────────
        right = QtWidgets.QVBoxLayout()
        right.setSpacing(4)
        right.addWidget(QtWidgets.QLabel("Events / log"))
        self._log_view = QtWidgets.QPlainTextEdit()
        self._log_view.setReadOnly(True)
        self._log_view.setMaximumBlockCount(500)
        right.addWidget(self._log_view, 1)
        outer.addLayout(right, 1)

        # Wire device → window. The callbacks fire on the sim worker
        # thread, so we hop through Qt signals (QueuedConnection) to
        # land on the GUI thread before touching widgets.
        self._screen_changed.connect(self._render_screen, QtCore.Qt.QueuedConnection)
        device.set_screen_change_callback(lambda scr: self._screen_changed.emit(scr))
        self._image_updated.connect(self._reload_image, QtCore.Qt.QueuedConnection)
        device.set_image_update_callback(lambda path: self._image_updated.emit(path))

        self._render_screen(device.active_screen)

    # -- log panel ---------------------------------------------------------

    def log(self, msg: str) -> None:
        """Append a single timestamped line to the event/log panel."""
        ts = QtCore.QTime.currentTime().toString("HH:mm:ss.zzz")
        self._log_view.appendPlainText(f"[{ts}] {msg}")

    # -- screen rendering --------------------------------------------------

    def _render_screen(self, screen: _proto.Screen | None) -> None:
        _log.debug(
            "sim: _render_screen called: path=%r screen=%s",
            self._device.active_screen_path,
            type(screen).__name__ if screen is not None else "None",
        )
        self._canvas.clear_layers()
        # Drop any RAM overrides — encoded path wins on screen (re)load.
        self._widget_ref_overrides.clear()
        self._image_widgets.clear()
        if screen is None:
            self.setWindowTitle("touchy-pad sim — (no screen)")
            return
        self.setWindowTitle(f"touchy-pad sim — {self._device.active_screen_path or '(unnamed)'}")
        # Bottom → active → top → sys (LVGL z-order).
        for attr in ("bottom", "active", "top", "sys"):
            if attr == "active":
                w = screen.active
            else:
                if not screen.HasField(attr):
                    continue
                w = getattr(screen, attr)
            if w.WhichOneof("kind") is None:
                continue
            try:
                qw = build_widget(
                    w,
                    self._device.fs,
                    on_event=self._on_widget_event,
                    widget_ref_overrides=self._widget_ref_overrides,
                    image_widget_registry=self._image_widgets,
                )
            except Exception:  # noqa: BLE001 — broken authoring shouldn't kill the window
                _log.exception("sim: failed to render layer %r", attr)
                continue
            # Only the active layer should be the click-target for its
            # whole area; overlay layers (top / bottom / sys) must let
            # gaps fall through so widgets on the active layer remain
            # clickable.
            self._canvas.add_layer(qw, transparent_to_mouse=attr != "active")

    def _reload_image(self, path: str) -> None:
        """Reload pixmaps for all image widgets backed by the given path."""
        widgets = self._image_widgets.get(path, [])
        _log.debug("sim: reloading %d widget(s) for path %r", len(widgets), path)
        for widget in widgets:
            widget.reload_pixmap()

    # -- shutdown ---------------------------------------------------------

    def closeEvent(self, event) -> None:  # noqa: N802 — Qt override
        """Detach the device callback before the window is destroyed.

        The :class:`SimDevice` worker thread calls our screen-change
        callback (which emits ``_screen_changed`` across threads) every
        time a screen is loaded. Without this teardown, the worker can
        emit a queued signal into a half-destroyed ``QObject`` — that
        manifests as a coredump on window close. The crash itself is
        bad enough, but it also tends to leave the host X server with
        an outstanding pointer/keyboard grab (the XCB connection isn't
        closed cleanly), freezing input on the host until suspend/resume
        resets the grab state. Clearing the callback first prevents the
        race entirely.
        """
        try:
            self._device.set_screen_change_callback(None)
        except Exception:  # noqa: BLE001 — never block window close
            _log.exception("sim: failed to detach device callback on close")
        super().closeEvent(event)

    # -- click / change dispatch ------------------------------------------

    def _on_widget_event(self, w: _proto.Widget, kind: str, state: dict) -> None:
        """Walk a widget's action list and dispatch each entry.

        Mirrors what the firmware's event handler does in
        ``firmware/main/widget_actions.cpp``:

        * :class:`ActionChangeWidgetRef` → re-binds the addressed
          widget_ref slot in-RAM and re-renders;
        * :class:`ActionMacro` → echoed to the log panel; the sim does
          not emulate HID;
        * :class:`ActionHost` → pushed onto the device event queue with
          any current widget state so a connected client sees the same
          ``LvEvent`` it would on real hardware.
        """
        actions = self._actions_for(w, kind)
        widget_id = w.id or ""
        if not actions:
            self.log(f"event: {widget_id or '(no id)'} ({kind}) — no actions")
            return
        for action in actions:
            self._dispatch_action(action, widget_id, kind, state)

    def _actions_for(self, w: _proto.Widget, kind: str) -> list:
        wk = w.WhichOneof("kind")
        if kind == "click":
            if wk == "button":
                return list(w.button.on_click)
            if wk == "image_button":
                return list(w.image_button.on_click)
        elif kind == "press":
            if wk == "button":
                return list(w.button.on_press)
            if wk == "image_button":
                return list(w.image_button.on_press)
        elif kind == "release":
            if wk == "button":
                return list(w.button.on_release)
            if wk == "image_button":
                return list(w.image_button.on_release)
        elif kind == "change":
            if wk == "slider":
                return list(w.slider.on_change)
            if wk == "checkbox":
                return list(w.checkbox.on_change)
            if wk == "toggle":
                return list(w.toggle.on_change)
        return []

    # Mapping from our internal kind-strings to the LVGL `lv_event_code_t`
    # value the firmware would forward in `LvEvent.code`. Used when
    # pushing host events from the sim so connected clients see the same
    # codes they'd see on real hardware.
    _LV_CODE_BY_KIND = {
        "press": 1,  # LV_EVENT_PRESSED
        "click": 7,  # LV_EVENT_CLICKED
        "release": 8,  # LV_EVENT_RELEASED
        "change": 28,  # LV_EVENT_VALUE_CHANGED
    }

    def _dispatch_action(
        self, action: _proto.Action, widget_id: str, kind: str, state: dict
    ) -> None:
        a_kind = action.WhichOneof("kind")
        if a_kind == "host":
            host_code = int(action.host.code)
            lv_code = self._LV_CODE_BY_KIND.get(kind)
            self._device.push_host_event(host_code, widget_id, lv_code=lv_code, **state)
            extras = " ".join(f"{k}={v}" for k, v in state.items())
            self.log(
                f"host: code=0x{host_code:x} widget={widget_id!r}"
                + (f" {extras}" if extras else "")
            )
        elif a_kind == "macro":
            self.log(f"macro: widget={widget_id!r} steps={len(action.macro.steps)} (not emulated)")
        elif a_kind == "device":
            self._dispatch_device(action.device, widget_id)
        else:
            self.log(f"action: widget={widget_id!r} (unknown kind {a_kind!r})")

    def _dispatch_device(self, action: _proto.ActionDevice, widget_id: str) -> None:
        sub = action.WhichOneof("kind")
        if sub != "change_widget_ref":
            self.log(f"device: widget={widget_id!r} (unsupported {sub!r})")
            return
        msg = action.change_widget_ref
        target_id = msg.target_id
        if not target_id:
            self.log(f"change_widget_ref: widget={widget_id!r} — missing target_id")
            return
        # Resolve target path.
        if msg.behavior == _proto.ActionChangeWidgetRef.BY_PATH:
            target = msg.path
            if not target:
                self.log(f"change_widget_ref: target_id={target_id!r} — empty path")
                return
        else:
            directory = msg.path or "F:host/w/"
            paths = self._device.list_widget_files(directory)
            if not paths:
                self.log(
                    f"change_widget_ref: target_id={target_id!r} — "
                    f"no *.pb files in {directory!r}"
                )
                return
            current = self._current_widget_ref_path(target_id)
            try:
                idx = paths.index(current) if current in paths else 0
                if current not in paths:
                    _log.warning(
                        "sim: change_widget_ref %r current %r not in %r",
                        target_id,
                        current,
                        directory,
                    )
            except ValueError:
                idx = 0
            step = 1 if msg.behavior == _proto.ActionChangeWidgetRef.NEXT else -1
            target = paths[(idx + step) % len(paths)]
        self._widget_ref_overrides[target_id] = target
        self.log(f"change_widget_ref: id={target_id!r} → {target!r}")
        # Re-render the current screen with the new override applied.
        # Note: we deliberately bypass `_render_screen` here because it
        # clears the overrides map on entry (those are wiped only on a
        # *new* screen load). Inline the rebuild instead.
        screen = self._device.active_screen
        self._canvas.clear_layers()
        if screen is None:
            return
        for attr in ("bottom", "active", "top", "sys"):
            if attr == "active":
                w = screen.active
            else:
                if not screen.HasField(attr):
                    continue
                w = getattr(screen, attr)
            if w.WhichOneof("kind") is None:
                continue
            try:
                qw = build_widget(
                    w,
                    self._device.fs,
                    on_event=self._on_widget_event,
                    widget_ref_overrides=self._widget_ref_overrides,
                )
            except Exception:  # noqa: BLE001
                _log.exception("sim: failed to render layer %r", attr)
                continue
            self._canvas.add_layer(qw, transparent_to_mouse=attr != "active")

    def _current_widget_ref_path(self, target_id: str) -> str | None:
        """Return the path the given widget_ref currently resolves to.

        Honours any in-RAM override; otherwise walks the active screen
        tree for a ``widget_ref`` whose outer ``Widget.id`` matches and
        returns its encoded path.
        """
        if target_id in self._widget_ref_overrides:
            return self._widget_ref_overrides[target_id]
        screen = self._device.active_screen
        if screen is None:
            return None

        def walk(w: _proto.Widget) -> str | None:
            kind = w.WhichOneof("kind")
            if kind == "widget_ref" and w.id == target_id:
                return w.widget_ref.path
            if kind == "layout_grid":
                children = w.layout_grid.layout.children
            elif kind == "layout_flex":
                children = w.layout_flex.layout.children
            elif kind == "layout_absolute":
                children = w.layout_absolute.layout.children
            else:
                return None
            for child in children:
                hit = walk(child)
                if hit is not None:
                    return hit
            return None

        for attr in ("active", "top", "bottom", "sys"):
            if attr == "active":
                root = screen.active
            else:
                if not screen.HasField(attr):
                    continue
                root = getattr(screen, attr)
            hit = walk(root)
            if hit is not None:
                return hit
        return None


# ---------------------------------------------------------------------------
# Convenience launcher
# ---------------------------------------------------------------------------


def run_sim_window(
    *,
    fs_root=None,
    serial: str = "SIM0000",
    size: tuple[int, int] = (480, 300),
) -> int:
    """Open a sim window backed by a fresh :class:`SimDevice` and run Qt.

    Returns the ``QApplication`` exit code. Blocks until the window is
    closed. Intended as the body of the ``touchy sim`` CLI subcommand;
    application code that needs more control should construct
    :class:`SimWindow` directly.
    """
    from .device import SimDevice
    from .fs import SimFs, default_cache_root

    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    fs = SimFs(fs_root or default_cache_root(), serial)
    device = SimDevice(fs)
    window = SimWindow(device, size=size)
    window.show()

    # Make Ctrl+C in the terminal cleanly close the Qt event loop
    # instead of being swallowed (the C handler runs but Qt is parked
    # in its native loop). A no-op periodic timer keeps Python ticking
    # so the SIGINT handler gets a chance to run.
    import signal

    signal.signal(signal.SIGINT, lambda *_: app.quit())
    _tick = QtCore.QTimer()
    _tick.start(200)
    _tick.timeout.connect(lambda: None)

    return app.exec()


__all__ = ["SimWindow", "run_sim_window"]
