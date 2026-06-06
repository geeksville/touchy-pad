"""USB vendor/product IDs for the Touchy-Pad device.

These identify the device on the USB bus; see ``docs/host-api.md`` for the
endpoint/interface layout.
"""

from __future__ import annotations

# Read from the generated protobuf so this stays in sync with the firmware
# automatically.  Source of truth: the Constants enum in proto/touchy.proto.
from ._proto.touchy_pb2 import Constants as _C

VID: int = _C.Value("USB_VID")  # 0x303A — Espressif-assigned vendor ID
PID: int = _C.Value("USB_PID")  # 0x8369 — touchy-pad product ID

# The custom protocol lives on a dedicated vendor-specific interface in
# addition to the standard HID interfaces. The host library locates it by
# bInterfaceClass == 0xFF (vendor-specific) rather than by interface number,
# so the firmware can rearrange the composite descriptor without breaking
# host code.
VENDOR_INTERFACE_CLASS = 0xFF

# Stage 83 — Touchy-Pad variants without native USB (the classic-ESP32 CYD
# family) appear on the host as USB-to-UART bridges. Discovery treats any
# serial port whose USB descriptor matches one of these (vid, pid) pairs as
# a Touchy candidate. New bridges are added with one line.
UART_BRIDGE_VID_PIDS: tuple[tuple[int, int], ...] = (
    (0x1A86, 0x7523),  # QinHeng CH340 (CYD2USB classic-ESP32 boards)
)
