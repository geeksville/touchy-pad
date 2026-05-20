"""Transport abstractions for the Touchy-Pad device.

The wire format is length-prefixed protobuf framing (see
``docs/host-api.md``); each protobuf message is preceded by a single
little-endian uint32 giving the byte count of the payload.

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

# Wire framing: u32 little-endian length prefix, then payload bytes.
# The header is 4 bytes, hosting a payload size up to 4 GiB; in practice
# we cap accepted frames at _MAX_FRAME bytes so a corrupt or malicious
# length field can't make us allocate a huge buffer.
_LEN_STRUCT = struct.Struct("<I")
_MAX_FRAME = 0x10_0000  # 1 MiB


class TransportError(Exception):
    """Raised on USB I/O failures or framing violations."""


class DeviceNotFoundError(TransportError):
    """No matching device is plugged in."""


class Transport(ABC):
    """Abstract bidirectional message transport.

    Two logical channels exist (mapped onto USB bulk endpoints by the
    concrete subclass):
      * ``send_command(payload)`` — host → device, bulk OUT.
      * ``recv_response(timeout)`` — device → host, bulk IN.

    Events are delivered by polling ``EventConsumeCmd`` over the same
    command/response pair (see ``docs/host-api.md``).

    ``payload`` is the already-serialised protobuf message; framing
    (length prefix, endpoint selection) is handled here.
    """

    @abstractmethod
    def send_command(self, payload: bytes) -> None: ...

    @abstractmethod
    def recv_response(self, timeout_ms: int = 2000) -> bytes: ...

    @abstractmethod
    def close(self) -> None: ...

    def __enter__(self) -> Transport:
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()


# -- pyusb-backed implementation --------------------------------------------


def _pack(payload: bytes) -> bytes:
    if len(payload) > _MAX_FRAME:
        raise TransportError(f"payload exceeds {_MAX_FRAME}-byte cap: {len(payload)} bytes")
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
_LIBUSB_ERROR_NOT_FOUND = 2  # pyusb maps LIBUSB_ERROR_NOT_FOUND → errno 2

# Linux usbfs ioctl numbers. Computed at module import time from the
# kernel ABI in <linux/usbdevice_fs.h>. We use these to detach kernel
# drivers (cdc_acm, usbhid) and claim interfaces ourselves, because
# `libusb_claim_interface()` is broken on devices opened via
# `libusb_wrap_sys_device()` in libusb 1.0.26 (it returns
# LIBUSB_ERROR_NOT_FOUND on every interface, even valid ones).

# USBDEVFS_DISCONNECT_CLAIM = _IOR('U', 27, struct usbdevfs_disconnect_claim)
# struct is { unsigned int interface; unsigned int flags; char driver[256]; }
# → 4 + 4 + 256 = 264 bytes; direction = read; type 'U' = 0x55; nr = 27.
_USBDEVFS_DISCONNECT_CLAIM = (2 << 30) | (264 << 16) | (ord("U") << 8) | 27


def _usbfs_disconnect_claim(fd: int, ifnum: int) -> None:
    """Disconnect any kernel driver bound to ``ifnum`` and claim it for ``fd``.

    Silently returns on any errno: the device may not have a kernel
    driver bound to the interface (in which case the ioctl still
    succeeds), the interface number may be out of range, or the kernel
    may simply not implement the ioctl on the running version.
    """
    import fcntl

    # struct usbdevfs_disconnect_claim — 264 bytes laid out as
    # uint32 interface; uint32 flags; char driver[256];
    buf = bytearray(264)
    buf[0:4] = ifnum.to_bytes(4, "little")
    buf[4:8] = (0).to_bytes(4, "little")  # flags = 0 → disconnect any driver
    try:
        fcntl.ioctl(fd, _USBDEVFS_DISCONNECT_CLAIM, bytes(buf))
    except OSError:
        pass


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
            raise TransportError(f"device node not found under {_HOST_DEV_USB_ROOT}: {path}") from e
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
        # on dispose. Stash it so the patched claim_interface below can
        # issue the per-interface USBDEVFS_DISCONNECT_CLAIM on the same fd
        # libusb is using for I/O.
        self._touchy_host_fd = fd  # type: ignore[attr-defined]

    _lb._DeviceHandle.__init__ = _patched_init

    # libusb_claim_interface() is broken on wrapped sys-device handles in
    # libusb 1.0.26: it always returns LIBUSB_ERROR_NOT_FOUND even for
    # interfaces that exist and are unclaimed. We've already done the
    # USBDEVFS_DISCONNECT_CLAIM ourselves above (which is per-fd, so the
    # claim persists for libusb's I/O). Wrap the backend's
    # claim/release to swallow ENOENT so pyusb's `managed_claim_interface`
    # path succeeds.
    _orig_claim = _lb._LibUSB.claim_interface
    _orig_release = _lb._LibUSB.release_interface

    def _patched_claim(self_be, dev_handle, intf):  # noqa: ANN001
        # On wrapped sys-device handles, do the per-interface usbfs
        # DISCONNECT_CLAIM ourselves *only* for the interface pyusb is
        # actually claiming (the vendor interface). Doing this in a
        # blanket loop at handle-open time would also detach the kernel
        # driver from the HID mouse/keyboard interfaces, leaving the host
        # without working USB mouse input for the lifetime of the CLI.
        fd = getattr(dev_handle, "_touchy_host_fd", None)
        if fd is not None:
            _usbfs_disconnect_claim(fd, intf)
        try:
            _orig_claim(self_be, dev_handle, intf)
        except _lb.USBError as exc:
            if getattr(exc, "errno", None) != _LIBUSB_ERROR_NOT_FOUND:
                raise
            if fd is None:
                raise
            # Already claimed via usbfs ioctl above; nothing to do.

    def _patched_release(self_be, dev_handle, intf):  # noqa: ANN001
        try:
            _orig_release(self_be, dev_handle, intf)
        except _lb.USBError as exc:
            if getattr(exc, "errno", None) != _LIBUSB_ERROR_NOT_FOUND:
                raise
            if not hasattr(dev_handle, "_touchy_host_fd"):
                raise
            # libusb's release_interface failed because it never
            # successfully claimed via libusb_claim_interface (we did
            # the usbfs ioctl ourselves). Issue the matching usbfs
            # RELEASEINTERFACE so the kernel reattaches usbhid /
            # cdc_acm / etc. when the CLI exits.
            import fcntl

            _USBDEVFS_RELEASEINTERFACE = (1 << 30) | (4 << 16) | (ord("U") << 8) | 16
            try:
                fcntl.ioctl(
                    dev_handle._touchy_host_fd,
                    _USBDEVFS_RELEASEINTERFACE,
                    intf.to_bytes(4, "little"),
                )
            except OSError:
                pass

    _lb._LibUSB.claim_interface = _patched_claim
    _lb._LibUSB.release_interface = _patched_release
    _lb._touchy_host_dev_patched = True


class UsbTransport(Transport):
    """pyusb-backed transport over the device's vendor-specific interface.

    The vendor interface is located by ``bInterfaceClass == 0xFF`` and is
    expected to expose two bulk endpoints:
        * 1× bulk OUT       — commands from host
        * 1× bulk IN        — responses to host

    Events are delivered by host polling of ``EventConsumeCmd`` on the
    same endpoint pair (the ESP32-S3 USB controller has no spare IN
    endpoint for a dedicated interrupt-IN mailbox). See
    ``docs/host-api.md`` for the rationale.
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
        #
        # We *only* detach the kernel driver from the vendor interface
        # we're about to claim. Earlier versions detached every kernel
        # driver on the device, which broke USB HID mouse / keyboard
        # reports from the trackpad widget: the kernel's `usbhid` driver
        # stays detached for the lifetime of this transport, so report
        # IN packets the firmware emits are silently dropped by the host
        # USB stack. Leaving `usbhid` and `cdc_acm` attached lets those
        # interfaces keep working while we drive the vendor interface
        # from userspace.
        #
        # On Linux the kernel auto-configures composite USB devices on
        # plug-in, so we don't need `libusb_set_configuration()` either —
        # which used to be the original justification for the
        # detach-everything dance.
        def _detach_vendor_kernel_driver(interface_number: int) -> None:
            try:
                if dev.is_kernel_driver_active(interface_number):
                    dev.detach_kernel_driver(interface_number)
            except (usb.core.USBError, NotImplementedError):
                # Not all backends implement these; ignore so we still
                # get a usable transport on platforms where the kernel
                # never claims the device in the first place (macOS,
                # Windows with WinUSB). For the vendor interface this is
                # always best-effort — there's no in-kernel driver for
                # vendor-class 0xFF interfaces anyway.
                pass

        # On Linux the kernel always auto-configures USB devices, so the
        # first descriptor-listed configuration is the active one — fall
        # back to it when pyusb's `get_active_configuration()` can't query
        # the device directly. This happens when the device was opened via
        # `libusb_wrap_sys_device()` (our `/host/dev/bus/usb` fallback for
        # the devcontainer), where `libusb_set_configuration()` and
        # `libusb_get_configuration()` return spurious errors / zero even
        # though the kernel has already configured the device. We seed
        # pyusb's internal `_active_cfg_index` so subsequent endpoint
        # read/write paths skip the broken descriptor query.
        try:
            cfg = dev.get_active_configuration()
        except usb.core.USBError:
            try:
                dev.set_configuration()
                cfg = dev.get_active_configuration()
            except usb.core.USBError:
                cfg = dev.configurations()[0]
                try:
                    dev._ctx._active_cfg_index = cfg.index
                except AttributeError:
                    pass

        intf = None
        for candidate in cfg:
            if candidate.bInterfaceClass == VENDOR_INTERFACE_CLASS:
                intf = candidate
                break
        if intf is None:
            raise TransportError("Device has no vendor-specific (0xFF) interface")
        self._intf = intf

        # Now that we know which interface we're going to claim, detach
        # only its kernel driver (if any). Leaves usbhid / cdc_acm on the
        # HID / CDC interfaces intact so the host keeps receiving mouse
        # and keyboard reports while the Python CLI is connected.
        _detach_vendor_kernel_driver(intf.bInterfaceNumber)

        ep_out = ep_in = None
        for ep in intf:
            attrs = ep.bmAttributes & 0x03  # 0x02 = bulk
            direction = ep.bEndpointAddress & 0x80
            if attrs == 0x02 and direction == 0x00:
                ep_out = ep
            elif attrs == 0x02 and direction == 0x80:
                ep_in = ep
        if ep_out is None or ep_in is None:
            raise TransportError("Vendor interface is missing bulk OUT or bulk IN endpoint")
        self._ep_out = ep_out
        self._ep_in = ep_in

        # Serialise concurrent command/response pairs.
        self._cmd_lock = threading.Lock()

    # -- Transport API ------------------------------------------------------

    def send_command(self, payload: bytes) -> None:
        framed = _pack(payload)
        with self._cmd_lock:
            self._ep_out.write(framed, timeout=2000)

    def recv_response(self, timeout_ms: int = 2000) -> bytes:
        # The wire framing is a 4-byte LE length followed by `length`
        # payload bytes. libusb requires each bulk-IN read buffer to be a
        # whole multiple of the endpoint's wMaxPacketSize (otherwise a
        # full-size packet from the device causes EOVERFLOW), so we round
        # every read up to the next packet boundary and stitch the chunks
        # together until we have the full frame.
        mps = self._ep_in.wMaxPacketSize

        def _read_at_least(min_bytes: int, accum: bytearray) -> None:
            while len(accum) < min_bytes:
                want = ((min_bytes - len(accum) + mps - 1) // mps) * mps
                # Cap any individual URB at a sensible size — the device
                # only sends frames up to ImageSaveCmd-equivalent (~32 KB),
                # but using a big-enough buffer is fine.
                want = min(want, 64 * 1024)
                chunk = bytes(self._ep_in.read(want, timeout=timeout_ms))
                if not chunk:
                    raise TransportError("device returned 0-byte read")
                accum.extend(chunk)

        buf = bytearray()
        _read_at_least(_LEN_STRUCT.size, buf)
        (length,) = _LEN_STRUCT.unpack_from(buf, 0)
        if length > _MAX_FRAME:
            raise TransportError(f"device announced oversize frame: {length} bytes")
        _read_at_least(_LEN_STRUCT.size + length, buf)
        return bytes(buf[_LEN_STRUCT.size : _LEN_STRUCT.size + length])

    def close(self) -> None:
        try:
            self._usb_util.dispose_resources(self._dev)
        except Exception:
            pass
