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
import logging
import threading
from collections.abc import Callable
from pathlib import Path
from typing import Union

from google.protobuf.message import Message as _PbMessage

from .. import _proto
from ..client import TouchyClient
from ..paths import DYNAMIC_IMAGE_DIR, screen_path
from ..transport import PID, VID, Transport
from . import _events, images_dynamic, protobuf
from .screens import Screen as _DslScreen

logger = logging.getLogger(__name__)


HostEventCallback = Callable[["_proto.LvEvent"], None]

#: Minimum USB-protocol version this library understands.
#:
#: The firmware reports its current protocol version via
#: ``sys_board_info_get()``; :func:`touchy_open` raises
#: :class:`IncompatibleFirmwareError` if the device is older than this.
MINIMUM_FIRMWARE_VERSION: int = int(_proto.SysBoardInfoResponse.ProtocolVersion.CURRENT)


class IncompatibleFirmwareError(RuntimeError):
    """Raised when the device's wire-format version is incompatible.

    ``device_too_new`` is ``True`` when the device is ahead of the library
    (upgrade ``touchy-pad`` with ``pip install -U touchy-pad``); ``False``
    when the device is behind (flash newer firmware).
    """

    def __init__(self, device_version: int, minimum: int, *, device_too_new: bool = False) -> None:
        if device_too_new:
            msg = (
                f"device firmware version {device_version} is newer than "
                f"this library supports (max {minimum}); "
                f"please upgrade: pip install -U touchy-pad"
            )
        else:
            msg = (
                f"device firmware version {device_version} is older than "
                f"the minimum supported version {minimum}; please run 'touchy update' "
                f"to flash the latest firmware"
            )
        super().__init__(msg)
        self.device_version = device_version
        self.minimum = minimum
        self.device_too_new = device_too_new


def touchy_get_pad_ids() -> list[str]:
    """Return the serial numbers of every Touchy-Pad currently attached.

    Each entry is suitable as the ``serial`` argument to
    :func:`touchy_open`. The list is empty if no device is plugged in.
    Requires a working ``libusb-1.0`` runtime on the host.

    UART-bridge Touchys (Stage 83 — the no-USB CYD family appearing on the
    host as a CH340 serial port) are returned with a ``"uart:"`` prefix
    followed by the device-node path, e.g. ``"uart:/dev/ttyUSB0"`` or
    ``"uart:COM3"``. Pass that string back to :func:`touchy_open` to open
    the matching device.

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
        try:
            # NoBackendError is raised on systems where pyusb is installed but
            # libusb is not (e.g. Windows CI runners). Treat it like "no devices".
            usb_devs = usb.core.find(idVendor=VID, idProduct=PID, find_all=True) or []
        except Exception:
            usb_devs = []
        for dev in usb_devs:
            try:
                serials.append(usb.util.get_string(dev, dev.iSerialNumber) or "")  # type: ignore[attr-defined]
            except Exception:
                # Fall back to a stable per-device identifier when the serial
                # string isn't readable (no permission, descriptor missing, …).
                serials.append(f"bus{dev.bus}-addr{dev.address}")

    # Stage 83 — UART-bridge Touchys (CH340-attached CYD boards). Appended
    # after native USB so callers that want "the first device" still pick
    # native hardware first.
    try:
        from ..transport_serial import discover_serial_ports
    except Exception:  # noqa: BLE001 - pyserial optional
        pass
    else:
        for path in discover_serial_ports():
            serials.append(f"uart:{path}")

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
        board_info=None,
    ) -> None:
        self._client = client
        self.board_info = board_info
        self._host_handlers: dict[int, list[HostEventCallback]] = {}
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        # Stage 87 — bound dynamic image sources (timers stopped on close)
        # and a one-shot wipe of the T:dyn/ scratch dir per connection.
        self._image_sources: list[images_dynamic.ImageSource] = []
        self._dyn_dir_wiped = False
        if start_event_thread:
            self._thread = threading.Thread(
                target=self._event_loop,
                name="touchy-pad-events",
                daemon=True,
            )
            self._thread.start()

    # -- lifecycle ----------------------------------------------------------

    @property
    def client(self) -> TouchyClient:
        """The underlying :class:`~touchy_pad.client.TouchyClient`.

        Exposed for advanced uses that need a client-level RPC not yet
        surfaced on :class:`Touchy` (e.g. the StreamDeck-compat shim
        polls ``event_consume`` itself rather than using the background
        event thread). Most callers should use the high-level methods on
        this class instead.
        """
        return self._client

    def close(self) -> None:
        """Stop the background event thread and close the USB transport."""
        self._stop.set()
        # Stop any dynamic-image refresh timers before tearing down the
        # transport so their final update() doesn't race the close.
        for src in self._image_sources:
            try:
                src.stop()
            except Exception:  # noqa: BLE001
                logger.exception("failed to stop image source %s", src.path)
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
        (e.g. ``"F:host/s/home.pb"`` to remove one screen, or
        ``"F:host"`` to wipe the whole host-uploaded area on flash).
        """
        self._client.file_delete(path)

    # Back-compat alias: previously called ``file_reset``. Wipes the
    # entire host-uploaded flash area.
    def file_reset(self) -> None:
        """Wipe every file from the device's flash host-upload area."""
        self._client.file_delete("F:host")

    def file_save(
        self,
        path: str,
        data: bytes | str,
        *,
        max_width: int | None = None,
        max_height: int | None = None,
    ) -> None:
        """Upload a single file to *path* on the device.

        *path* must be drive-prefixed
        (e.g. ``"F:host/images/avatar.png"`` for persistent flash or
        ``"R:host/images/avatar.bin"`` for transient PSRAM storage).
        ``data`` may be raw ``bytes`` or a ``str``. Image inputs are
        auto-converted to LVGL ``.bin`` format by the underlying
        client. Internally this issues the streaming
        ``FileOpenWrite`` / ``FileWrite`` / ``FileClose`` sequence, but
        callers see a single atomic operation.

        *max_width* and *max_height* — if given, the image is scaled down
        (preserving aspect ratio) so that neither dimension exceeds the
        respective limit before conversion.  Non-image payloads silently
        ignore these parameters.
        """
        self._client.file_save(path, data, max_width=max_width, max_height=max_height)

    def screen_load(self, path: str) -> None:
        """Activate the screen at the given drive-prefixed *path*.

        E.g. ``pad.screen_load("F:host/s/home.pb")``. Passing an
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
        is uploaded to ``F:host/s/<name>.pb`` (persistent flash).
        Returns the final name used.
        """
        if name is not None:
            final_name = name
        elif isinstance(screen, _DslScreen):
            final_name = screen.name
        else:
            raise ValueError("screen_save: screen has no name; pass name= explicitly")
        msg = self._coerce_screen(screen)
        # Count widgets in all layers (active + optional top/sys/bottom).
        # Each layer is a Widget, and layout-kind widgets hold children
        # in their repeated `widgets` field (relative to the oneof).
        widget_count = self._count_widgets(msg.active)
        if msg.HasField("top"):
            widget_count += self._count_widgets(msg.top)
        if msg.HasField("sys"):
            widget_count += self._count_widgets(msg.sys)
        if msg.HasField("bottom"):
            widget_count += self._count_widgets(msg.bottom)
        logger.debug(
            "screen_save: %s (%d widgets)",
            final_name,
            widget_count,
        )
        self._register_inline_callbacks(msg)
        self._bind_image_sources(msg)
        self._client.file_save(screen_path(final_name), msg.SerializeToString())
        return final_name

    @staticmethod
    def _count_widgets(widget: _proto.Widget) -> int:
        """Recursively count widgets in a tree."""
        count = 1  # the widget itself
        kind = widget.WhichOneof("kind")
        if kind in ("layout_absolute", "layout_flex", "layout_grid"):
            layout_msg = getattr(widget, kind)
            # Each layout kind has a `.layout` field which is a `Layout`
            # message containing `repeated Widget children`.
            for child in layout_msg.layout.children:
                count += Touchy._count_widgets(child)
        # Non-layout widgets (button, label, etc.) are leaves.
        return count

    def _register_inline_callbacks(self, msg: _PbMessage) -> None:
        """Wire up any ``host_action(on_event=...)`` callbacks in *msg*.

        Stage 67: walks the serialised tree for every ``ActionHost.code``
        it references, harvests the matching pending bindings from
        :mod:`touchy_pad.api._events`, and registers each via
        :meth:`on_host_event`. This is what lets inline callbacks light up
        automatically on upload, scoped to exactly the codes this screen /
        widget uses and to this device.
        """
        codes = self._collect_host_codes(msg)
        if not codes:
            return
        for code, cb in _events.harvest(codes).items():
            self.on_host_event(code, cb)

    def _bind_image_sources(self, msg: _PbMessage) -> None:
        """Bind any embedded :class:`ImageSource`s and push their first frame.

        Stage 87: walks the serialised tree for every ``Image.path`` it
        references, harvests the matching pending sources, wipes the
        ``T:dyn/`` scratch dir once per connection, then binds each
        source to this device (initial upload + timer start) and tracks
        it so :meth:`close` can stop its refresh thread.
        """
        paths = self._collect_image_paths(msg)
        if not paths:
            return
        sources = images_dynamic.harvest(paths)
        if not sources:
            return
        if not self._dyn_dir_wiped:
            try:
                self._client.file_delete(DYNAMIC_IMAGE_DIR.rstrip("/"))
            except Exception:  # noqa: BLE001 — best-effort scratch wipe.
                logger.debug("could not wipe %s", DYNAMIC_IMAGE_DIR, exc_info=True)
            self._dyn_dir_wiped = True
        for src in sources.values():
            src._bind(self)
            self._image_sources.append(src)

    @staticmethod
    def _collect_image_paths(msg: _PbMessage) -> set[str]:
        """Recursively collect every ``Image.path`` referenced in *msg*."""
        paths: set[str] = set()
        if isinstance(msg, _proto.Image):
            if msg.path:
                paths.add(msg.path)
            return paths
        for field, value in msg.ListFields():
            if field.type != field.TYPE_MESSAGE:
                continue
            if field.is_repeated:
                if field.message_type.GetOptions().map_entry:
                    continue
                for item in value:
                    paths |= Touchy._collect_image_paths(item)
            else:
                paths |= Touchy._collect_image_paths(value)
        return paths

    @staticmethod
    def _collect_host_codes(msg: _PbMessage) -> set[int]:
        """Recursively collect every ``ActionHost.code`` referenced in *msg*."""
        codes: set[int] = set()
        if isinstance(msg, _proto.ActionHost):
            codes.add(msg.code)
            return codes
        for field, value in msg.ListFields():
            if field.type != field.TYPE_MESSAGE:
                continue
            if field.is_repeated:
                # Skip protobuf map fields (their entries aren't Actions).
                if field.message_type.GetOptions().map_entry:
                    continue
                for item in value:
                    codes |= Touchy._collect_host_codes(item)
            else:
                codes |= Touchy._collect_host_codes(value)
        return codes

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

    def widget_save(
        self,
        name: str,
        widget: _proto.Widget,
        *,
        drive: str = "F",
    ) -> str:
        """Upload a standalone :class:`Widget` (Stage 54).

        Writes the serialized widget to
        ``{drive}:host/w/{name}.pb`` so a screen elsewhere may
        reference it via ``widget_ref("{drive}:host/w/{name}.pb")``.

        *drive* defaults to ``"F"`` (persistent flash); pass ``"R"`` for
        the volatile PSRAM ramdisk.
        """
        if not name:
            raise ValueError("widget_save: name must be non-empty")
        if drive not in ("F", "R"):
            raise ValueError(f"widget_save: drive must be 'F' or 'R', got {drive!r}")
        if not isinstance(widget, _proto.Widget):
            raise TypeError(
                f"widget_save: widget must be touchy_pad.api.protobuf.Widget, "
                f"got {type(widget).__name__}"
            )
        path = f"{drive}:host/w/{name}.pb"
        # Stage 56: stamp the wire-format version on the root widget
        # so the firmware can validate (and delete on mismatch) the
        # same way it does for screen files.
        stamped = _proto.Widget()
        stamped.CopyFrom(widget)
        stamped.version = _proto.Widget.Version.CURRENT
        logger.debug("widget_save: %s", path)
        self._register_inline_callbacks(stamped)
        self._bind_image_sources(stamped)
        self._client.file_save(path, stamped.SerializeToString())
        return path

    def user_screen_save(
        self,
        name: str,
        widget: _proto.Widget,
        *,
        drive: str = "F",
    ) -> str:
        """Upload a user-screen page body (Stage 68).

        Writes the serialized widget to ``{drive}:host/uscr/{name}.pb``.
        The default chrome screen (``F:host/s/default.pb``, see
        :func:`touchy_pad.api.build_default_screen`) pages its body
        ``widget_ref`` through this directory, so each file here is one
        top-level page that fills the area below the prev/next row. This
        is the directory most users push to; ``F:host/w/`` (see
        :meth:`widget_save`) is for generic widget-refs instead.

        *drive* defaults to ``"F"`` (persistent flash); pass ``"R"`` for
        the volatile PSRAM ramdisk. Returns the drive-prefixed path.
        """
        if not name:
            raise ValueError("user_screen_save: name must be non-empty")
        if drive not in ("F", "R"):
            raise ValueError(f"user_screen_save: drive must be 'F' or 'R', got {drive!r}")
        if not isinstance(widget, _proto.Widget):
            raise TypeError(
                f"user_screen_save: widget must be touchy_pad.api.protobuf.Widget, "
                f"got {type(widget).__name__}"
            )
        path = f"{drive}:host/uscr/{name}.pb"
        # Stage 56 version stamp (mirrors widget_save) so the firmware
        # validates the root widget on load.
        stamped = _proto.Widget()
        stamped.CopyFrom(widget)
        stamped.version = _proto.Widget.Version.CURRENT
        logger.debug("user_screen_save: %s", path)
        self._register_inline_callbacks(stamped)
        self._bind_image_sources(stamped)
        self._client.file_save(path, stamped.SerializeToString())
        return path

    def show_user_screen(self, name: str, *, drive: str = "F") -> None:
        """Bring a previously-saved user-screen page body to the front.

        *name* is the bare stem passed to :meth:`user_screen_save`
        (e.g. ``"demo"``). The default chrome screen
        (``F:host/s/default.pb``) renders its body through a
        ``widget_ref(id="page")``; this retargets that ref to
        ``{drive}:host/uscr/{name}.pb`` so the page shows immediately,
        without a user touch and without reloading the screen.

        Mirrors the Rust ``Touchy::show_user_screen``: it issues a
        single :class:`ActionChangeWidgetRef` (``BY_PATH``) targeting the
        chrome's ``"page"`` ref via :meth:`TouchyClient.run_actions`.
        """
        from ..paths import user_screen_path
        from .screens import change_widget_ref_action

        if drive not in ("F", "R"):
            raise ValueError(f"show_user_screen: drive must be 'F' or 'R', got {drive!r}")
        path = user_screen_path(name) if drive == "F" else f"R:host/uscr/{name}.pb"
        logger.debug("show_user_screen: %s", path)
        self._client.run_actions([change_widget_ref_action("page", path)])

    def set_image_button_slot(
        self,
        widget_id: str,
        pressed: bool,
        path: str,
    ) -> None:
        """Repoint one image slot of an on-screen ``ImageButton`` in place (Stage 86).

        Swaps exactly one of the button's image slots — ``pressed`` or
        released — to *path* without rebuilding the widget, so a key the
        user is currently holding keeps its touch state and still emits a
        release event. This is the primitive behind StreamDeck-style key
        repaints (``TouchyDeck.set_key_image`` / the Rust OpenDeck plugin):
        each distinct icon is uploaded once to a cache, then a key's
        artwork is updated by pointing its slot at the cached path.

        *widget_id* is the ``Widget.id`` of the ``ImageButton`` (its own
        id, not a parent ``WidgetRef``). *pressed* selects the slot
        (``True`` → pressed image, ``False`` → released image). *path* is
        a full drive-prefixed asset path
        (e.g. ``"T:host/icache/<hash>.bin"``).

        Mirrors the Rust ``Touchy::set_image_button_slot``. See also
        :func:`touchy_pad.api.screens.set_image_button_slot_action`.
        """
        from .screens import set_image_button_slot_action

        logger.debug("set_image_button_slot: %s pressed=%s -> %s", widget_id, pressed, path)
        self._client.run_actions([set_image_button_slot_action(widget_id, pressed, path)])

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

    With no arguments, opens the first device found, in this precedence
    order:

    1. A native-USB Touchy.
    2. A UART-bridge Touchy (Stage 83 — CH340-attached CYD boards).
    3. The out-of-process simulator named by ``TOUCHY_SIM_URL``.
    4. The in-process simulator (if :func:`create_sim_device` was called).

    ``serial`` selects a specific device by USB serial number (see
    :func:`touchy_get_pad_ids`). A value of ``"uart:<path>"`` opens the
    serial port at *path* directly via :class:`SerialTransport`.
    ``transport`` is an internal escape hatch used by tests — production
    code should leave it ``None``.

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
        # Stage 83 — explicit "uart:<path>" selector opens that serial port.
        if serial is not None and serial.startswith("uart:"):
            from ..transport_serial import SerialTransport

            transport = SerialTransport(serial[len("uart:") :])
        else:
            from .sim_registry import get_sim_serial, get_sim_transport

            sim_transport = get_sim_transport()
            sim_serial = get_sim_serial()

            use_sim = sim_transport is not None and (serial is None or serial == sim_serial)

            if use_sim and serial is None:
                # Prefer a real device (USB then UART) over the sim when both
                # are present and the caller didn't explicitly ask for either.
                transport = _open_first_real_device()
                if transport is None:
                    transport = sim_transport
            elif use_sim:
                transport = sim_transport
            else:
                # Stage 63: honour TOUCHY_SIM_URL before USB enumeration so
                # any host-side consumer (Rust client, OpenDeck plugin,
                # ad-hoc script) transparently picks up an out-of-process
                # simulator just by exporting the env var.
                from ..transport_net import TcpTransport, sim_url_from_env

                sim_url = sim_url_from_env()
                if sim_url is not None:
                    del serial
                    transport = TcpTransport(*sim_url)
                else:
                    # ``serial`` filtering is currently a no-op on UsbTransport
                    # (it always opens the first matching VID/PID). When
                    # multi-device support lands we'll plumb ``serial`` through.
                    del serial
                    real = _open_first_real_device()
                    if real is None:
                        # Re-raise the USB-specific error (DeviceNotFoundError)
                        # the way callers expect when nothing is attached.
                        from ..transport import UsbTransport

                        transport = UsbTransport()
                    else:
                        transport = real

    client = TouchyClient(transport)
    try:
        ver = client.sys_board_info_get()
    except Exception:
        client.close()
        raise

    device_version = int(getattr(ver, "protocol_version", 0))
    if device_version:
        if device_version < MINIMUM_FIRMWARE_VERSION:
            client.close()
            raise IncompatibleFirmwareError(device_version, MINIMUM_FIRMWARE_VERSION)
        if device_version > MINIMUM_FIRMWARE_VERSION:
            client.close()
            raise IncompatibleFirmwareError(
                device_version, MINIMUM_FIRMWARE_VERSION, device_too_new=True
            )

    return Touchy(client, board_info=ver)


def _open_first_real_device() -> Transport | None:
    """Try native-USB, then UART-bridge auto-discovery (Stage 83).

    Returns the first transport that opens cleanly, or ``None`` if neither
    a native-USB Touchy nor a UART-bridge Touchy is present/accessible.
    Caller decides what to do when nothing is found (open the simulator,
    or re-raise a USB-specific not-found error).
    """
    try:
        from ..transport import UsbTransport

        return UsbTransport()
    except Exception:
        pass

    try:
        from ..transport_serial import SerialTransport, discover_serial_ports
    except Exception:  # noqa: BLE001 - pyserial optional
        return None

    for path in discover_serial_ports():
        try:
            return SerialTransport(path)
        except Exception:  # noqa: BLE001 - try the next candidate
            logger.debug("touchy_open: serial port %s did not open; trying next", path)
            continue
    return None


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
