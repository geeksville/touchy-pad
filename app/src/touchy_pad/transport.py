"""Transport abstractions for the Touchy-Pad device.

The wire format is length-prefixed protobuf framing (see
``docs/host-api.md``); each protobuf message is preceded by a single
little-endian uint16 giving the byte count of the payload.

This module exposes:
    - ``Transport``: the abstract base class.
    - ``UsbTransport``: a pyusb-backed implementation that talks to the
      device's vendor-specific interface.

Tests can subclass ``Transport`` to provide an in-memory loopback.
"""

from __future__ import annotations

import struct
import threading
from abc import ABC, abstractmethod

from .usb_ids import PID, VENDOR_INTERFACE_CLASS, VID

# Wire framing: u16 little-endian length prefix, then payload bytes.
_LEN_STRUCT = struct.Struct("<H")
_MAX_FRAME = 0xFFFF


class TransportError(Exception):
    """Raised on USB I/O failures or framing violations."""


class DeviceNotFoundError(TransportError):
    """No matching device is plugged in."""


class Transport(ABC):
    """Abstract bidirectional message transport.

    Three logical channels exist (mapped onto USB endpoints by the concrete
    subclass):
      * ``send_command(payload)`` — host → device, bulk OUT.
      * ``recv_response(timeout)`` — device → host, bulk IN.
      * ``recv_event(timeout)`` — device → host, interrupt IN.

    ``payload`` is the already-serialised protobuf message; framing
    (length prefix, endpoint selection) is handled here.
    """

    @abstractmethod
    def send_command(self, payload: bytes) -> None: ...

    @abstractmethod
    def recv_response(self, timeout_ms: int = 2000) -> bytes: ...

    @abstractmethod
    def recv_event(self, timeout_ms: int = 0) -> bytes | None: ...

    @abstractmethod
    def close(self) -> None: ...

    def __enter__(self) -> Transport:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()


# -- pyusb-backed implementation --------------------------------------------


def _pack(payload: bytes) -> bytes:
    if len(payload) > _MAX_FRAME:
        raise TransportError(f"payload too large for u16 frame: {len(payload)} bytes")
    return _LEN_STRUCT.pack(len(payload)) + payload


def _unpack(buf: bytes) -> bytes:
    if len(buf) < _LEN_STRUCT.size:
        raise TransportError(f"short frame: got {len(buf)} bytes, need >= 2")
    (length,) = _LEN_STRUCT.unpack_from(buf, 0)
    if len(buf) < _LEN_STRUCT.size + length:
        raise TransportError(
            f"truncated frame: header says {length} bytes, got {len(buf) - _LEN_STRUCT.size}"
        )
    return bytes(buf[_LEN_STRUCT.size : _LEN_STRUCT.size + length])


# Some sandboxed environments — most notably the touchy-pad devcontainer —
# only bind-mount the USB root-hub nodes from the host into the container's
# /dev/bus/usb, but expose the live udev tree at /host/dev/bus/usb. libusb's
# enumeration via /sys still finds the device (so pyusb's find() succeeds),
# but the subsequent libusb_open() fails with ENODEV because the BBB/DDD
# character device is missing under /dev/bus/usb.
#
# Recover by opening the matching node under /host/dev/bus/usb ourselves
# and handing the fd to libusb_wrap_sys_device(); the resulting
# libusb_device_handle is indistinguishable from a normal open as far as
# pyusb's bulk_read/bulk_write paths are concerned.
_HOST_DEV_USB_ROOT = "/host/dev/bus/usb"
_LIBUSB_ERROR_NO_DEVICE = 19  # pyusb maps LIBUSB_ERROR_NO_DEVICE → errno 19


def _install_host_dev_fallback() -> None:
    import ctypes
    import os

    from usb.backend import libusb1 as _lb

    if getattr(_lb, "_touchy_host_dev_patched", False):
        return

    # pyusb defers loading the libusb DLL until the backend is first used;
    # poke it so `_lb._lib` is populated before we touch its symbols.
    if _lb._lib is None:
        _lb.get_backend()
    lib = _lb._lib
    if lib is None:
        # libusb isn't available at all — nothing to patch.
        return

    # libusb_wrap_sys_device(ctx, sys_dev, dev_handle*)
    lib.libusb_wrap_sys_device.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.POINTER(_lb._libusb_device_handle),
    ]
    lib.libusb_wrap_sys_device.restype = ctypes.c_int
    lib.libusb_get_bus_number.argtypes = [ctypes.c_void_p]
    lib.libusb_get_bus_number.restype = ctypes.c_uint8
    lib.libusb_get_device_address.argtypes = [ctypes.c_void_p]
    lib.libusb_get_device_address.restype = ctypes.c_uint8

    _orig_init = _lb._DeviceHandle.__init__

    def _patched_init(self: object, dev: object) -> None:  # type: ignore[override]
        try:
            _orig_init(self, dev)
            return
        except _lb.USBError as exc:
            if getattr(exc, "errno", None) != _LIBUSB_ERROR_NO_DEVICE:
                raise
            if not os.path.isdir(_HOST_DEV_USB_ROOT):
                raise

        bus = int(lib.libusb_get_bus_number(dev.devid))
        addr = int(lib.libusb_get_device_address(dev.devid))
        path = f"{_HOST_DEV_USB_ROOT}/{bus:03d}/{addr:03d}"
        try:
            fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
        except FileNotFoundError as e:
            raise TransportError(
                f"device node not found under {_HOST_DEV_USB_ROOT}: {path}"
            ) from e
        except PermissionError as e:
            raise TransportError(
                f"permission denied opening {path}; check udev rules / group"
            ) from e

        self.handle = _lb._libusb_device_handle()
        self.devid = dev.devid
        rv = lib.libusb_wrap_sys_device(None, fd, ctypes.byref(self.handle))
        if rv < 0:
            os.close(fd)
            # Re-raise as the standard pyusb USBError so callers see the
            # familiar error type.
            raise _lb.USBError(_lb._strerror(rv), rv, _lb._libusb_errno.get(rv))
        # libusb has taken ownership of fd — it'll be closed by libusb_close
        # on dispose. Stash it for introspection only.
        self._touchy_host_fd = fd  # type: ignore[attr-defined]

    _lb._DeviceHandle.__init__ = _patched_init
    _lb._touchy_host_dev_patched = True



class UsbTransport(Transport):
    """pyusb-backed transport over the device's vendor-specific interface.

    The vendor interface is located by ``bInterfaceClass == 0xFF`` and is
    expected to expose three endpoints:
        * 1× bulk OUT       — commands from host
        * 1× bulk IN        — responses to host
        * 1× interrupt IN   — asynchronous events

    See ``docs/host-api.md`` for the rationale.
    """

    def __init__(self, vid: int = VID, pid: int = PID) -> None:
        # Import lazily so the module is still importable on systems without
        # libusb installed (e.g. CI lint stages).
        import usb.core
        import usb.util

        # Wire up the /host/dev fallback for sandboxed environments where
        # newly-attached devices don't appear under /dev/bus/usb. No-op
        # outside such environments.
        _install_host_dev_fallback()

        self._usb_core = usb.core
        self._usb_util = usb.util

        dev = usb.core.find(idVendor=vid, idProduct=pid)
        if dev is None:
            raise DeviceNotFoundError(
                f"No Touchy-Pad device with VID=0x{vid:04x} PID=0x{pid:04x} found"
            )
        self._dev = dev

        # Find the vendor-specific interface and its three endpoints.
        try:
            cfg = dev.get_active_configuration()
        except usb.core.USBError:
            dev.set_configuration()
            cfg = dev.get_active_configuration()

        intf = None
        for candidate in cfg:
            if candidate.bInterfaceClass == VENDOR_INTERFACE_CLASS:
                intf = candidate
                break
        if intf is None:
            raise TransportError("Device has no vendor-specific (0xFF) interface")
        self._intf = intf

        ep_out = ep_in = ep_evt = None
        for ep in intf:
            attrs = ep.bmAttributes & 0x03  # 0x02 = bulk, 0x03 = interrupt
            direction = ep.bEndpointAddress & 0x80
            if attrs == 0x02 and direction == 0x00:
                ep_out = ep
            elif attrs == 0x02 and direction == 0x80:
                ep_in = ep
            elif attrs == 0x03 and direction == 0x80:
                ep_evt = ep
        if ep_out is None or ep_in is None:
            raise TransportError(
                "Vendor interface is missing bulk OUT or bulk IN endpoint"
            )
        # The interrupt-IN event endpoint is optional: stage 13 firmware
        # ships only the bulk command/response pair. recv_event() raises
        # when no event endpoint was advertised.
        self._ep_out = ep_out
        self._ep_in = ep_in
        self._ep_evt = ep_evt

        # Serialise concurrent command/response pairs; events use a separate
        # endpoint and need no locking against commands.
        self._cmd_lock = threading.Lock()

    # -- Transport API ------------------------------------------------------

    def send_command(self, payload: bytes) -> None:
        framed = _pack(payload)
        with self._cmd_lock:
            self._ep_out.write(framed, timeout=2000)

    def recv_response(self, timeout_ms: int = 2000) -> bytes:
        # Pull a frame's worth of bytes. The endpoint's wMaxPacketSize is
        # typically 64 (full-speed) or 512 (high-speed); responses fit easily
        # below the u16 limit, so one read is enough.
        size = max(self._ep_in.wMaxPacketSize, _MAX_FRAME + _LEN_STRUCT.size)
        buf = bytes(self._ep_in.read(size, timeout=timeout_ms))
        return _unpack(buf)

    def recv_event(self, timeout_ms: int = 0) -> bytes | None:
        # timeout_ms=0 means non-blocking via a very short USB timeout.
        if self._ep_evt is None:
            raise TransportError(
                "Device does not expose an event endpoint (firmware predates "
                "stage 14)"
            )
        try:
            buf = bytes(self._ep_evt.read(self._ep_evt.wMaxPacketSize, timeout=timeout_ms or 1))
        except self._usb_core.USBError as e:
            # pyusb raises USBError(110, 'Operation timed out') on libusb
            # ETIMEDOUT; treat that as "no event pending".
            if getattr(e, "errno", None) in (110, 60):
                return None
            raise
        if not buf:
            return None
        return _unpack(buf)

    def close(self) -> None:
        try:
            self._usb_util.dispose_resources(self._dev)
        except Exception:
            pass
