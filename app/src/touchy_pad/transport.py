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

# Wire framing (Stage 64.3): a self-synchronising frame used by *every*
# transport (USB bulk, TCP sim, serial):
#
#     MAGIC(2) | LEN(u16 LE) | payload | CRC8(1)
#
#   * MAGIC  — fixed 2-byte sentinel marking a frame start; the resync
#              anchor a byte-stream reader scans for.
#   * LEN    — u16 little-endian payload length. The largest message is a
#              ~4 KiB FileWriteCmd, so two bytes (64 KiB ceiling) is ample
#              and makes an over-cap length impossible to express.
#   * CRC8   — single-byte CRC (poly 0x07) over LEN || payload. Cheap by
#              design: just enough to *detect* a corrupt frame and trigger
#              a resync, not to correct errors.
#
# USB/TCP are already reliable underneath, so the CRC/resync machinery is
# effectively a no-op there; it earns its keep on a real UART.
_MAGIC = b"\xa5\x5a"
_LEN_STRUCT = struct.Struct("<H")
_MAX_FRAME = 0xFFFF  # bounded by the u16 length field


def _crc8(data: bytes) -> int:
    """CRC-8 (poly 0x07, init 0x00) over ``data``. Table-free."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


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

    #: True when the transport speaks to real device firmware that only
    #: understands raw LVGL ``.bin`` image assets. The host-side image
    #: pipeline in :mod:`touchy_pad.api.images` converts PNG/JPG/etc.
    #: to that format before upload when this flag is set.
    #:
    #: Subclasses targeting a Python-side consumer (e.g. the Stage 30
    #: device simulator, which decodes images with Pillow) should
    #: override this to ``False`` so source-format bytes pass through
    #: untouched.
    needs_image_conversion: bool = True

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
    body = _LEN_STRUCT.pack(len(payload)) + payload
    return _MAGIC + body + bytes((_crc8(body),))


def _unpack(buf: bytes) -> bytes:
    """Decode exactly one framed message from ``buf`` (no resync).

    Convenience for tests / callers that already hold a single complete
    frame starting at the magic. Stream readers use :class:`_FrameDecoder`
    instead, which tolerates leading garbage and CRC errors.
    """
    hdr = len(_MAGIC) + _LEN_STRUCT.size
    if len(buf) < hdr + 1:
        raise TransportError(f"short frame: got {len(buf)} bytes")
    if not buf.startswith(_MAGIC):
        raise TransportError("frame does not start with magic")
    (length,) = _LEN_STRUCT.unpack_from(buf, len(_MAGIC))
    end = hdr + length
    if len(buf) < end + 1:
        raise TransportError(f"truncated frame: header says {length} bytes, got {len(buf) - hdr}")
    body = bytes(buf[len(_MAGIC) : end])
    if _crc8(body) != buf[end]:
        raise TransportError("frame CRC8 mismatch")
    return body[_LEN_STRUCT.size :]


class _FrameDecoder:
    """Stateful, resynchronising decoder for the Stage 64.3 wire frame.

    Feed it arbitrary byte chunks; it yields complete, CRC-validated
    payloads. Leading garbage (boot-log noise on a UART) is skipped by
    scanning for :data:`_MAGIC`; a CRC mismatch drops one byte past the
    candidate magic and rescans. On reliable transports (USB/TCP) it
    never actually has to resync — the magic is always frame-aligned.
    """

    _HDR = len(_MAGIC) + _LEN_STRUCT.size

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, data: bytes) -> None:
        self._buf.extend(data)

    def next_frame(self) -> bytes | None:
        """Return the next complete payload, or ``None`` if more bytes are needed."""
        buf = self._buf
        while True:
            idx = buf.find(_MAGIC)
            if idx < 0:
                # No magic yet. Retain a trailing byte in case the magic
                # straddles this chunk and the next one.
                if len(buf) > 1:
                    del buf[:-1]
                return None
            if idx > 0:
                del buf[:idx]  # drop leading garbage
            if len(buf) < self._HDR + 1:
                return None  # need length + at least the CRC byte
            (length,) = _LEN_STRUCT.unpack_from(buf, len(_MAGIC))
            end = self._HDR + length
            if len(buf) < end + 1:
                return None  # full payload + CRC not here yet
            body = bytes(buf[len(_MAGIC) : end])
            if _crc8(body) == buf[end]:
                del buf[: end + 1]
                return body[_LEN_STRUCT.size :]
            # CRC failed: this magic was spurious (or the frame is
            # corrupt). Skip past it and rescan.
            del buf[:1]


class _StreamFramedTransport(Transport):
    """Base for byte-stream transports (TCP, serial).

    Subclasses implement the two stream primitives below; this class
    owns the shared framing, the resync decoder, and the command lock.
    """

    needs_image_conversion = True

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._decoder = _FrameDecoder()
        self._closed = False

    # -- stream primitives (subclass-provided) --------------------------

    def _write_all(self, data: bytes) -> None:  # pragma: no cover - abstract
        raise NotImplementedError

    def _read_some(self, timeout_ms: int) -> bytes:  # pragma: no cover - abstract
        """Read and return up to a chunk of bytes; ``b""`` on EOF.

        Must raise :class:`TransportError` on timeout.
        """
        raise NotImplementedError

    # -- Transport API --------------------------------------------------

    def send_command(self, payload: bytes) -> None:
        if self._closed:
            raise TransportError(f"{type(self).__name__} is closed")
        frame = _pack(payload)
        with self._lock:
            self._write_all(frame)

    def recv_response(self, timeout_ms: int = 2000) -> bytes:
        if self._closed:
            raise TransportError(f"{type(self).__name__} is closed")
        while True:
            frame = self._decoder.next_frame()
            if frame is not None:
                return frame
            chunk = self._read_some(timeout_ms)
            if not chunk:
                self._closed = True
                raise TransportError("connection closed by peer")
            self._decoder.feed(chunk)


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
_LIBUSB_ERROR_ACCESS = 13  # pyusb maps LIBUSB_ERROR_ACCESS    → errno 13
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


def _in_dev_container() -> bool:
    """Return True when running inside a VS Code / GitHub dev container."""
    import os

    return os.environ.get("REMOTE_CONTAINERS", "").lower() in ("true", "1", "yes")


def _install_host_dev_fallback() -> None:
    import ctypes
    import os

    from usb.backend import libusb1 as _lb

    if not _in_dev_container():
        return

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
            if getattr(exc, "errno", None) not in (
                _LIBUSB_ERROR_NO_DEVICE,
                _LIBUSB_ERROR_ACCESS,
            ):
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
        # Resynchronising frame decoder shared with the stream transports.
        self._rx_decoder = _FrameDecoder()

    # -- Transport API ------------------------------------------------------

    def send_command(self, payload: bytes) -> None:
        framed = _pack(payload)
        with self._cmd_lock:
            self._ep_out.write(framed, timeout=2000)

    def recv_response(self, timeout_ms: int = 2000) -> bytes:
        # The wire framing (Stage 64.3) is MAGIC | LEN(u16) | payload |
        # CRC8. libusb requires each bulk-IN read buffer to be a whole
        # multiple of the endpoint's wMaxPacketSize (otherwise a
        # full-size packet from the device causes EOVERFLOW), so we read
        # in packet-sized chunks and feed them to the shared
        # `_FrameDecoder`, which stitches and validates one frame.
        mps = self._ep_in.wMaxPacketSize

        while True:
            frame = self._rx_decoder.next_frame()
            if frame is not None:
                return frame
            # Round each URB up to a packet boundary; the device only
            # sends small frames, but a generous buffer is harmless.
            want = min(((mps + mps - 1) // mps) * mps * 16, 64 * 1024)
            chunk = bytes(self._ep_in.read(want, timeout=timeout_ms))
            if not chunk:
                # A 0-byte read is a USB Zero-Length Packet (ZLP).
                # TinyUSB appends one whenever a bulk transfer's total
                # length is an exact multiple of wMaxPacketSize, to
                # signal "transfer complete". Skip it and keep waiting;
                # a true disconnect raises USBError on timeout instead.
                continue
            self._rx_decoder.feed(chunk)

    def close(self) -> None:
        try:
            self._usb_util.dispose_resources(self._dev)
        except Exception:
            pass
