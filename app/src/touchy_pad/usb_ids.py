"""USB vendor/product IDs for the Touchy-Pad device.

These identify the device on the USB bus; see ``docs/host-api.md`` for the
endpoint/interface layout.
"""

from __future__ import annotations

# Allocated for this project (see firmware/main/usb_hid.cpp).
VID = 0x4403
PID = 0x1002

# The custom protocol lives on a dedicated vendor-specific interface in
# addition to the standard HID interfaces. The host library locates it by
# bInterfaceClass == 0xFF (vendor-specific) rather than by interface number,
# so the firmware can rearrange the composite descriptor without breaking
# host code.
VENDOR_INTERFACE_CLASS = 0xFF
