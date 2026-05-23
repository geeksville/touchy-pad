"""Lifecycle-managed entry point for application code.

The :class:`Touchy` class wraps the internal :class:`TouchyClient`
with:

* a background thread that polls the device for ``LvEvent`` messages
  and dispatches them to user-registered callbacks;
* a firmware-version compatibility check at open time;
* a higher-level :meth:`Touchy.screen_save` that accepts host-DSL
  ``Screen`` objects, raw ``protobuf.Screen`` messages, parsed JSON
  dicts, or paths to ``.json`` files.

Most users should construct a :class:`Touchy` via :func:`touchy_open`
inside a ``with`` block::

    from touchy_pad.api import touchy_open

    with touchy_open() as pad:
        pad.screen_load("default")
"""

from __future__ import annotations

import json
import threading
from collections.abc import Callable
from pathlib import Path
from typing import Union

from .. import _proto
from ..client import TouchyClient
from ..transport import PID, VID, Transport
from . import protobuf
from .screens import Screen as _DslScreen

HostEventCallback = Callable[["_proto.LvEvent"], None]

#: Minimum USB-protocol version this library understands.
#:
#: The firmware reports its current protocol version via
#: ``sys_board_info_get()``; :func:`touchy_open` raises
#: :class:`IncompatibleFirmwareError` if the device is older than this.
MINIMUM_FIRMWARE_VERSION: int = int(_proto.SysBoardInfoResponse.ProtocolVersion.CURRENT)


class IncompatibleFirmwareError(RuntimeError):
    """Raised when the attached device reports a firmware version older
    than :data:`MINIMUM_FIRMWARE_VERSION`.
    """

    def __init__(self, device_version: int, minimum: int) -> None:
        super().__init__(
            f"device firmware wire-format version {device_version} is older than "
            f"the minimum supported version {minimum}; please update the firmware"
        )
        self.device_version = device_version
        self.minimum = minimum


def touchy_get_pad_ids() -> list[str]:
    """Return the serial numbers of every Touchy-Pad currently attached.

    Each entry is suitable as the ``serial`` argument to
    :func:`touchy_open`. The list is empty if no device is plugged in.
    Requires a working ``libusb-1.0`` runtime on the host.

    When :func:`touchy_pad.api.create_sim_device` has been called this
    process, the sim's pseudo-serial is appended to the list too — so
    discovery code (StreamController, the touchydeck enumeration hook,
    etc.) sees the sim and real devices through the same API.
    """
    serials: list[str] = []
    try:
        import usb.core
    except ImportError:  # pragma: no cover — handled by libusb install docs.
        pass
    else:
        for dev in usb.core.find(idVendor=VID, idProduct=PID, find_all=True) or []:
            try:
                serials.append(usb.util.get_string(dev, dev.iSerialNumber) or "")  # type: ignore[attr-defined]
            except Exception:
                # Fall back to a stable per-device identifier when the serial
                # string isn't readable (no permission, descriptor missing, …).
                serials.append(f"bus{dev.bus}-addr{dev.address}")

    from .sim_registry import get_sim_serial

    sim_serial = get_sim_serial()
    if sim_serial is not None and sim_serial not in serials:
        serials.append(sim_serial)
    return serials


# Anything :meth:`Touchy.screen_save` is willing to accept.
ScreenLike = Union[_DslScreen, "_proto.Screen", dict, str, Path]


class Touchy:
    """A connected, ready-to-use Touchy-Pad.

    Instances are normally created via :func:`touchy_open`. The object
    owns a USB transport and a background polling thread, so it must be
    closed when no longer needed — either by calling :meth:`close` or by
    using it as a context manager::

        with touchy_open() as pad:
            pad.screen_load("home")
    """

    def __init__(
        self,
        client: TouchyClient,
        *,
        start_event_thread: bool = True,
    ) -> None:
        self._client = client
        self._host_handlers: dict[int, list[HostEventCallback]] = {}
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        if start_event_thread:
            self._thread = threading.Thread(
                target=self._event_loop,
                name="touchy-pad-events",
                daemon=True,
            )
            self._thread.start()

    # -- lifecycle ----------------------------------------------------------

    def close(self) -> None:
        """Stop the background event thread and close the USB transport."""
        self._stop.set()
        try:
            self._client.close()
        finally:
            t = self._thread
            if t is not None and t.is_alive():
                # The event thread blocks in `event_consume` on the
                # transport; closing the transport above causes that call
                # to raise, which lets the thread exit promptly.
                t.join(timeout=2.0)
            self._thread = None

    def __enter__(self) -> Touchy:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    # -- thin wrappers over the host API -----------------------------------

    def file_delete(self, path: str) -> None:
        """Delete a file or directory subtree from the device filesystem.

        *path* must be drive-prefixed
        (e.g. ``"F:host/screens/home.pb"`` to remove one screen, or
        ``"F:host"`` to wipe the whole host-uploaded area on flash).
        """
        self._client.file_delete(path)

    # Back-compat alias: previously called ``file_reset``. Wipes the
    # entire host-uploaded flash area.
    def file_reset(self) -> None:
        """Wipe every file from the device's flash host-upload area."""
        self._client.file_delete("F:host")

    def file_save(self, path: str, data: bytes | str) -> None:
        """Upload a single file to *path* on the device.

        *path* must be drive-prefixed
        (e.g. ``"F:host/images/avatar.png"`` for persistent flash or
        ``"R:host/images/avatar.bin"`` for transient PSRAM storage).
        ``data`` may be raw ``bytes`` or a ``str``. Image inputs are
        auto-converted to LVGL ``.bin`` format by the underlying
        client. Internally this issues the streaming
        ``FileOpenWrite`` / ``FileWrite`` / ``FileClose`` sequence, but
        callers see a single atomic operation.
        """
        self._client.file_save(path, data)

    def screen_load(self, path: str) -> None:
        """Activate the screen at the given drive-prefixed *path*.

        E.g. ``pad.screen_load("F:host/screens/home.pb")``. Passing an
        empty string loads the device's default screen.
        """
        self._client.screen_load(path)

    # -- higher-level screen authoring -------------------------------------

    def screen_save(self, screen: ScreenLike, *, name: str | None = None) -> str:
        """Upload a screen definition to the device.

        Accepts:

        * a host-DSL :class:`touchy_pad.api.screens.Screen` (its ``name``
          field is used);
        * a raw :class:`touchy_pad.api.protobuf.Screen` message;
        * a ``dict`` of JSON-parsed screen data (camelCase, as in
          ``proto/default_screen.json``);
        * a ``str`` or :class:`pathlib.Path` pointing at a ``.json`` file
          containing the same.

        ``name`` overrides the screen's own name if given. The screen
        is uploaded to ``F:host/screens/<name>.pb`` (persistent flash).
        Returns the final name used.
        """
        msg = self._coerce_screen(screen)
        final_name = name if name is not None else msg.name
        if not final_name:
            raise ValueError("screen_save: screen has no name and no explicit name= given")
        msg.name = final_name
        self._client.file_save(f"F:host/screens/{final_name}.pb", msg.SerializeToString())
        return final_name

    @staticmethod
    def _coerce_screen(screen: ScreenLike) -> _proto.Screen:
        if isinstance(screen, _DslScreen):
            return screen.to_proto()
        if isinstance(screen, _proto.Screen):
            return screen
        if isinstance(screen, dict):
            msg = _proto.Screen()
            from google.protobuf import json_format

            json_format.ParseDict(screen, msg)
            return msg
        if isinstance(screen, str | Path):
            text = Path(screen).read_text()
            data = json.loads(text)
            msg = _proto.Screen()
            from google.protobuf import json_format

            json_format.ParseDict(data, msg)
            return msg
        raise TypeError(f"screen_save: unsupported screen type: {type(screen).__name__}")

    # -- event dispatch ----------------------------------------------------

    def on_host_event(self, code: int, callback: HostEventCallback) -> None:
        """Register a callback for ``ActionHost`` events with the given code.

        Multiple callbacks may be registered for the same code; all are
        invoked, in registration order, from the background event
        thread. Callbacks must not block for long — they share a thread
        with USB polling.
        """
        with self._lock:
            self._host_handlers.setdefault(code, []).append(callback)

    # -- internal: event polling -------------------------------------------

    def _event_loop(self) -> None:
        try:
            for evt in self._client.stream_events():
                if self._stop.is_set():
                    return
                with self._lock:
                    handlers = list(self._host_handlers.get(evt.host_code, ()))
                for cb in handlers:
                    try:
                        cb(evt)
                    except Exception:  # noqa: BLE001 — user callback errors must not kill the thread.
                        import traceback

                        traceback.print_exc()
        except Exception:
            # Transport closed (normal during `close()`) or some other
            # error — either way, the thread is done.
            if not self._stop.is_set():
                import traceback

                traceback.print_exc()


def touchy_open(serial: str | None = None, *, transport: Transport | None = None) -> Touchy:
    """Open a connected Touchy-Pad and return a :class:`Touchy`.

    With no arguments, opens the first device found. ``serial`` selects
    a specific device by USB serial number (see
    :func:`touchy_get_pad_ids`). ``transport`` is an internal escape
    hatch used by tests — production code should leave it ``None``.

    Raises :class:`touchy_pad.transport.DeviceNotFoundError` when no
    matching device is attached, and :class:`IncompatibleFirmwareError`
    when the device reports a firmware version older than
    :data:`MINIMUM_FIRMWARE_VERSION`.

    If :func:`touchy_pad.api.create_sim_device` has been called this
    process and ``serial`` matches the sim's serial (or no real USB
    device is plugged in and ``serial`` is ``None``), the sim's
    transport is used transparently — exactly the same code path real
    hardware uses.
    """
    if transport is None:
        from .sim_registry import get_sim_serial, get_sim_transport

        sim_transport = get_sim_transport()
        sim_serial = get_sim_serial()

        use_sim = sim_transport is not None and (serial is None or serial == sim_serial)

        if use_sim and serial is None:
            # Prefer a real device over the sim when both are present
            # and the caller didn't explicitly ask for either.
            try:
                from ..transport import UsbTransport

                transport = UsbTransport()
            except Exception:
                transport = sim_transport
        elif use_sim:
            transport = sim_transport
        else:
            from ..transport import UsbTransport

            # ``serial`` filtering is currently a no-op on UsbTransport
            # (it always opens the first matching VID/PID). When
            # multi-device support lands we'll plumb ``serial`` through.
            del serial
            transport = UsbTransport()

    client = TouchyClient(transport)
    try:
        ver = client.sys_board_info_get()
    except Exception:
        client.close()
        raise

    device_version = int(getattr(ver, "protocol_version", 0))
    if device_version and device_version < MINIMUM_FIRMWARE_VERSION:
        client.close()
        raise IncompatibleFirmwareError(device_version, MINIMUM_FIRMWARE_VERSION)

    return Touchy(client)


# Public alias for star-imports.
__all__ = [
    "IncompatibleFirmwareError",
    "MINIMUM_FIRMWARE_VERSION",
    "Touchy",
    "touchy_get_pad_ids",
    "touchy_open",
]

# Re-export the protobuf submodule under our namespace for symmetry.
_ = protobuf
