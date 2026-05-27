"""Python implementation of the device-side host-API protocol.

:class:`SimDevice` consumes serialized :class:`touchy.Command` protobufs
and produces serialized :class:`touchy.Response` protobufs, mirroring
what the firmware does in ``firmware/main/host_api.cpp``. Coupled with
:class:`~touchy_pad.sim.transport.SimDeviceTransport` it lets the
existing :class:`touchy_pad.TouchyClient` talk to a Python sim instead
of real hardware.

The sim deliberately implements only the subset of behavior needed for
host-side app development:

* file_open_write / file_write / file_close stream new files into a
  sandboxed pseudo-fs (one in-flight transaction at a time, matching
  firmware constraints).
* file_delete wipes a file or directory subtree.
* screen_load tracks the active screen by drive-prefixed path and
  dispatches its default-load actions in the future (Stage 30 step 5).
* event_consume drains a queue populated by widget activations.
* sys_version_get returns the wire protocol's CURRENT version.
* screen_wake / screen_sleep_timeout / sys_reboot_bootloader are
  acknowledged but otherwise no-ops in the sim.

Out of scope for Stage 30: HID emulation, animation, multitouch,
backlight, preferences persistence, exact pixel rendering.
"""

from __future__ import annotations

import logging
import threading
from collections.abc import Callable
from pathlib import Path
from queue import Empty, Queue

from google.protobuf import json_format

from .. import _proto
from .fs import SimFs

_log = logging.getLogger("touchy_pad.sim")

#: Wire-protocol version this sim implements. Bumped in lockstep with
#: the firmware so existing version checks in the host pass.
_CURRENT_PROTOCOL = _proto.SysBoardInfoResponse.ProtocolVersion.CURRENT

#: Sentinel firmware version reported by the sim. Distinct from any
#: real build number so callers can tell they're talking to a sim.
_SIM_FW_VERSION = 0
_SIM_FW_VERSION_STR = "sim"

#: Path to the JSON-encoded default screen the firmware embeds.
_DEFAULT_SCREEN_JSON = Path(__file__).resolve().parents[4] / "proto" / "default_screen.json"


def _result(code: int = _proto.RESULT_OK, **payload: object) -> _proto.Response:
    """Convenience constructor for a Response with an optional payload."""
    return _proto.Response(code=code, **payload)


def _load_default_screen() -> _proto.Screen | None:
    """Return the firmware's embedded default screen, or ``None``.

    Tolerates a missing JSON file so the sim still works in installs
    that don't ship the ``proto/`` directory (e.g. wheel-only).
    """
    try:
        text = _DEFAULT_SCREEN_JSON.read_text(encoding="utf-8")
    except OSError:
        return None
    try:
        return json_format.Parse(text, _proto.Screen())
    except json_format.ParseError as exc:  # pragma: no cover — invalid ship JSON
        _log.warning("could not parse default_screen.json: %s", exc)
        return None


# ---------------------------------------------------------------------------


class SimDevice:
    """Stateful Python device emulator.

    Thread-safety: :meth:`handle_command` may be called from any
    thread; internal state is guarded by ``self._lock``. The screen-
    change callback fires on the calling thread.
    """

    def __init__(
        self,
        fs: SimFs,
        on_screen_change: Callable[[_proto.Screen | None], None] | None = None,
        *,
        display_size: tuple[int, int] = (480, 300),
    ) -> None:
        self._fs = fs
        self._lock = threading.RLock()
        self._events: Queue[_proto.LvEvent] = Queue()
        self._on_screen_change = on_screen_change
        self._on_image_update: Callable[[str], None] | None = None
        # Reported back via SysBoardInfoResponse.display_{width,height}.
        # Host adapters (e.g. TouchyDeck) size their UI from this.
        self._display_size = (int(display_size[0]), int(display_size[1]))

        #: Active write transactions: handle → (path, accumulated bytes).
        #: Only one in flight at a time, matching firmware constraints,
        #: but we key by handle so a bug-ridden client trying multiple
        #: opens gets a clean ``RESULT_IO_ERROR`` on the second one.
        self._writes: dict[int, tuple[str, bytearray]] = {}
        self._next_handle: int = 1

        #: Current active screen path (drive-prefixed, matching
        #: firmware's `g_current_path`).
        self._active_path: str | None = None
        #: Decoded active screen, or None when nothing has loaded yet.
        self._active_screen: _proto.Screen | None = None

        # Auto-load the lexicographically first uploaded screen on
        # startup, mirroring firmware boot behavior. Falls back to the
        # embedded default screen when the sim-fs is empty.
        paths = self._fs.list_screens()
        if paths:
            try:
                self._do_screen_load(paths[0])
            except Exception as exc:  # noqa: BLE001 — keep sim alive on bad data
                _log.warning("auto-load of %r failed: %s", paths[0], exc)
        if self._active_screen is None:
            default = _load_default_screen()
            if default is not None:
                self._active_path = "<built-in>"
                self._active_screen = default
                self._notify_screen_change()

    # -- public API used by the GUI / tests ------------------------------

    @property
    def active_screen(self) -> _proto.Screen | None:
        with self._lock:
            return self._active_screen

    @property
    def active_screen_path(self) -> str | None:
        with self._lock:
            return self._active_path

    # Back-compat alias for callers that haven't migrated to the
    # post-stage-51 ``active_screen_path`` name yet.
    @property
    def active_screen_name(self) -> str | None:
        with self._lock:
            return self._active_path

    def list_screens(self) -> list[str]:
        """Paths of all uploaded screens, sorted (same order firmware uses)."""
        return self._fs.list_screens()

    def list_widget_files(self, directory: str) -> list[str]:
        """Stage 57 — paths of ``*.pb`` widget files under *directory*.

        Used by the simulator's ``ActionChangeWidgetRef`` NEXT / PREVIOUS
        handler to enumerate paged widget files the same way the firmware
        does.
        """
        return self._fs.list_widget_files(directory)

    @property
    def fs(self) -> SimFs:
        return self._fs

    def set_screen_change_callback(self, cb: Callable[[_proto.Screen | None], None] | None) -> None:
        """Replace the screen-change callback after construction.

        Used by the Qt window, which wires its renderer signal once the
        ``QApplication`` is up rather than at ``SimDevice`` construction.
        """
        self._on_screen_change = cb

    def set_image_update_callback(self, cb: Callable[[str], None] | None) -> None:
        """Register a callback for when image asset files are updated.

        Used by the Qt window to reload pixmaps without destroying
        widgets (which would break mouse tracking during interactions).
        """
        self._on_image_update = cb

    def push_host_event(
        self,
        host_code: int,
        widget_id: str = "",
        *,
        lv_code: int | None = None,
        **state: object,
    ) -> None:
        """Queue a host event as if a widget had fired ``ActionHost``.

        Used by the GUI when the user clicks a button whose ``on_click``
        contains an :class:`ActionHost`, and by tests that want to
        simulate widget activations without rendering. ``lv_code`` lets
        callers pick the LVGL `lv_event_code_t` to forward in
        ``LvEvent.code``; defaults to ``7`` (LV_EVENT_CLICKED) for
        backwards compatibility with pre-50.2 callers.
        """
        evt = _proto.LvEvent(
            code=7 if lv_code is None else int(lv_code),
            user_data=widget_id,
            host_code=host_code,
        )
        if "value" in state:
            evt.value = int(state["value"])  # type: ignore[arg-type]
        if "checked" in state:
            evt.checked = bool(state["checked"])
        self._events.put(evt)

    # -- main command dispatch -------------------------------------------

    def handle_command(self, payload: bytes) -> bytes:
        """Parse a serialized Command, dispatch, return serialized Response.

        Unknown / unsupported commands return ``RESULT_NOT_SUPPORTED``
        so the host sees an explicit error rather than a hang.
        """
        cmd = _proto.Command()
        cmd.ParseFromString(payload)
        which = cmd.WhichOneof("cmd")
        if which is None:
            return _result(_proto.RESULT_INVALID_ARG).SerializeToString()
        try:
            handler = getattr(self, f"_cmd_{which}")
        except AttributeError:
            _log.info("sim: unsupported command %s", which)
            return _result(_proto.RESULT_NOT_SUPPORTED).SerializeToString()
        with self._lock:
            return handler(getattr(cmd, which)).SerializeToString()

    # -- command handlers (one per Command.cmd oneof variant) ------------

    def _cmd_sys_board_info_get(self, _msg: _proto.SysBoardInfoGetCmd) -> _proto.Response:
        return _result(
            sys_board_info=_proto.SysBoardInfoResponse(
                protocol_version=_CURRENT_PROTOCOL,
                firmware_version=_SIM_FW_VERSION,
                firmware_version_str=_SIM_FW_VERSION_STR,
                board_name="sim",
                display_width=self._display_size[0],
                display_height=self._display_size[1],
            ),
        )

    def _cmd_sys_reboot_bootloader(self, _msg: _proto.SysRebootBootloaderCmd) -> _proto.Response:
        _log.info("sim: ignoring sys_reboot_bootloader (sim has no bootloader)")
        return _result()

    def _cmd_screen_wake(self, _msg: _proto.ScreenWakeCmd) -> _proto.Response:
        return _result()

    def _cmd_screen_sleep_timeout(self, _msg: _proto.ScreenSleepTimeoutCmd) -> _proto.Response:
        return _result()

    def _cmd_file_delete(self, msg: _proto.FileDeleteCmd) -> _proto.Response:
        try:
            self._fs.delete(msg.path)
        except ValueError as exc:
            _log.warning("sim: file_delete rejected %r: %s", msg.path, exc)
            return _result(_proto.RESULT_INVALID_ARG)
        except OSError as exc:
            _log.error("sim: file_delete I/O error on %r: %s", msg.path, exc)
            return _result(_proto.RESULT_IO_ERROR)
        _log.info("sim: file_delete (%s)", msg.path)
        return _result()

    def _cmd_file_open_write(self, msg: _proto.FileOpenWriteCmd) -> _proto.Response:
        try:
            # Reject the path early — mirrors the firmware aborting an
            # open() against a malformed drive prefix.
            self._fs._resolve(msg.path)  # noqa: SLF001 — sim is intimate with SimFs
        except ValueError as exc:
            _log.warning("sim: file_open_write rejected %r: %s", msg.path, exc)
            return _result(_proto.RESULT_INVALID_ARG)
        handle = self._next_handle
        self._next_handle += 1
        self._writes[handle] = (msg.path, bytearray())
        return _result(
            file_open_write=_proto.FileOpenWriteResponse(handle=handle),
        )

    def _cmd_file_write(self, msg: _proto.FileWriteCmd) -> _proto.Response:
        txn = self._writes.get(msg.handle)
        if txn is None:
            return _result(_proto.RESULT_IO_ERROR)
        txn[1].extend(msg.data)
        return _result()

    def _cmd_file_close(self, msg: _proto.FileCloseCmd) -> _proto.Response:
        txn = self._writes.pop(msg.handle, None)
        if txn is None:
            return _result(_proto.RESULT_IO_ERROR)
        if not msg.commit:
            return _result()
        path, buf = txn
        try:
            self._fs.save(path, bytes(buf))
        except ValueError as exc:
            _log.warning("sim: file_close rejected %r: %s", path, exc)
            return _result(_proto.RESULT_INVALID_ARG)
        except OSError as exc:
            _log.error("sim: file_close I/O error on %r: %s", path, exc)
            return _result(_proto.RESULT_IO_ERROR)
        # Notify image updates so SimWindow can reload affected pixmaps
        # without destroying widgets (which would break mouse tracking).
        if "host/images/" in path and self._on_image_update is not None:
            _log.debug("sim: image asset updated %r", path)
            try:
                self._on_image_update(path)
            except Exception:  # noqa: BLE001
                _log.exception("sim: on_image_update callback raised")
        return _result()

    def _cmd_screen_load(self, msg: _proto.ScreenLoadCmd) -> _proto.Response:
        try:
            self._do_screen_load(msg.path)
        except FileNotFoundError:
            return _result(_proto.RESULT_NOT_FOUND)
        except Exception as exc:  # noqa: BLE001 — surface protocol error
            _log.warning("sim: screen_load(%r) failed: %s", msg.path, exc)
            return _result(_proto.RESULT_INVALID_ARG)
        return _result()

    def _cmd_event_consume(self, _msg: _proto.EventConsumeCmd) -> _proto.Response:
        try:
            evt = self._events.get_nowait()
        except Empty:
            return _result(_proto.RESULT_NOT_FOUND)
        return _result(event_consume=_proto.EventConsumeResponse(event=evt))

    # -- internal helpers -------------------------------------------------

    def _do_screen_load(self, path: str) -> None:
        """Activate a previously-uploaded screen by full drive-prefixed path.

        Empty path means "load the default screen" — matches firmware
        ``screens_load(NULL)`` semantics: the first registered screen,
        or the built-in fallback when nothing is uploaded.
        """
        if not path:
            paths = self._fs.list_screens()
            if paths:
                path = paths[0]
            else:
                default = _load_default_screen()
                if default is None:
                    raise FileNotFoundError("no screens available")
                self._active_path = "<built-in>"
                self._active_screen = default
                self._notify_screen_change()
                return
        if not self._fs.exists(path):
            raise FileNotFoundError(path)
        screen = _proto.Screen()
        screen.ParseFromString(self._fs.read(path))
        self._active_path = path
        self._active_screen = screen
        _log.info("sim: loaded screen %r", path)
        self._notify_screen_change()

    def _notify_screen_change(self) -> None:
        _log.debug(
            "sim: _notify_screen_change: path=%r callback=%s",
            self._active_path,
            "set" if self._on_screen_change is not None else "NONE",
        )
        if self._on_screen_change is not None:
            try:
                self._on_screen_change(self._active_screen)
            except Exception:  # noqa: BLE001 — never let a GUI bug kill the device
                _log.exception("sim: on_screen_change callback raised")
