#!/usr/bin/env python3
"""Diagnose Touchy-Pad USB enumeration — especially in dev containers.

Why this exists
---------------
A dev container gets its own ``/dev/bus/usb`` at *start-up* (Docker builds it
from the host's device nodes), while the host's live ``/dev`` is bind-mounted
at ``/host/dev``. So a pad plugged in later, or unplugged and re-plugged onto a
new bus address, can be invisible to libusb *inside* the container — and pyusb
caches exactly **one** libusb context for the life of the process
(``libusb1.get_backend()`` returns a module-level singleton), which is a second
way for a long-running client to go blind.

Every few seconds this prints:

* what a *live sysfs* scan (shared kernel → always current) sees, plus that
  device's node in ``/dev/bus/usb`` and ``/host/dev/bus/usb`` — so you can see
  which view is stale;
* what **this** process's raw pyusb/libusb enumeration sees (this legitimately
  stays stale forever — libusb caches the list per context);
* what a **fresh** process sees (subprocess) — separates "our process has gone
  stale" from "the system view is stale";
* what ``touchy_pad._usb.find_usb_devices`` (the code that ships) sees — it
  retries with a fresh context, so this is the line that must go green on a
  re-plug;
* whether the pad can actually be opened and driven right now.

Usage::

    # loop until ctrl-c; unplug / re-plug the pad while it runs
    just usb-diag

    # one snapshot and exit (what the subprocess check itself uses)
    just usb-diag --once

    # faster sampling for a narrow window
    just usb-diag --interval 0.5 --loops 30
"""

from __future__ import annotations

import argparse
import ctypes
import glob
import json
import os
import subprocess
import sys
import time

#: Keep in sync with app/src/touchy_pad/api/_usb_ids.py.
VID, PID = 0x303A, 0x8369
CONTAINER_USB_ROOT = "/dev/bus/usb"
HOST_USB_ROOT = "/host/dev/bus/usb"


def _read(path: str) -> str | None:
    try:
        with open(path) as fh:
            return fh.read().strip()
    except OSError:
        return None


def libusb_version() -> str:
    """The *system* libusb version — its device-list caching differs by version."""
    for name in ("libusb-1.0.so.0", "libusb-1.0.so", "libusb-1.0.dylib"):
        try:
            lib = ctypes.CDLL(name)
        except OSError:
            continue

        class _Version(ctypes.Structure):
            _fields_ = [
                ("major", ctypes.c_uint16),
                ("minor", ctypes.c_uint16),
                ("micro", ctypes.c_uint16),
                ("rc", ctypes.c_char_p),
                ("describe", ctypes.c_char_p),
            ]

        lib.libusb_get_version.restype = ctypes.POINTER(_Version)
        v = lib.libusb_get_version().contents
        return f"{v.major}.{v.minor}.{v.micro}{('-' + v.rc.decode()) if v.rc else ''}"
    return "unknown"


def sysfs_touchys() -> list[dict]:
    """Live sysfs scan for our VID/PID (the kernel's view — never cached)."""
    found = []
    for devdir in sorted(glob.glob("/sys/bus/usb/devices/*")):
        vid = _read(f"{devdir}/idVendor")
        pid = _read(f"{devdir}/idProduct")
        if vid is None or f"{int(vid, 16):04x}" != f"{VID:04x}":
            continue
        if pid is None or f"{int(pid, 16):04x}" != f"{PID:04x}":
            continue
        busnum = _read(f"{devdir}/busnum")
        devnum = _read(f"{devdir}/devnum")
        if not busnum or not devnum:
            continue
        node = f"{int(busnum):03d}/{int(devnum):03d}"
        found.append(
            {
                "sysfs": os.path.basename(devdir),
                "node": node,
                "in_dev_bus_usb": os.path.exists(f"{CONTAINER_USB_ROOT}/{node}"),
                "in_host_dev_bus_usb": os.path.exists(f"{HOST_USB_ROOT}/{node}"),
                "product": _read(f"{devdir}/product"),
            }
        )
    return found


def node_count(root: str) -> int:
    return len(glob.glob(f"{root}/*/*"))


def snapshot() -> dict:
    """Enumerate the way the real client does, from *this* process."""
    snap: dict = {"libusb_error": None, "libusb_total": None, "libusb_touchy": None}
    try:
        import usb.core

        devices = list(usb.core.find(find_all=True))
    except Exception as exc:  # noqa: BLE001 — diagnostic: report anything
        snap["libusb_error"] = f"{type(exc).__name__}: {exc}"
    else:
        snap["libusb_total"] = len(devices)
        hits = [
            d
            for d in devices
            if getattr(d, "idVendor", None) == VID and getattr(d, "idProduct", None) == PID
        ]
        snap["libusb_touchy"] = [f"{d.bus:03d}/{d.address:03d}" for d in hits] or None

    snap["sysfs_touchy"] = sysfs_touchys()
    # What the shipping code sees: `find_usb_devices` retries with a fresh
    # libusb context, so this line should go green on a re-plug even while the
    # raw "this process" line above stays stale (libusb's per-context cache).
    try:
        from touchy_pad._usb import find_usb_devices

        hits = find_usb_devices(usb.core, VID, PID, find_all=True)
        snap["helper_touchy"] = [f"{d.bus:03d}/{d.address:03d}" for d in hits] or None
    except Exception as exc:  # noqa: BLE001 — diagnostic: report anything
        snap["helper_touchy"] = f"error: {type(exc).__name__}: {exc}"
    snap["nodes"] = {
        "dev_bus_usb": node_count(CONTAINER_USB_ROOT),
        "host_dev_bus_usb": node_count(HOST_USB_ROOT) if os.path.isdir(HOST_USB_ROOT) else None,
    }
    return snap


def fresh_process_snapshot() -> dict:
    """A brand-new process's enumeration — separates us from the system view."""
    try:
        out = subprocess.run(
            [sys.executable, os.path.abspath(__file__), "--once"],
            capture_output=True,
            text=True,
            timeout=30,
            check=True,
        )
    except Exception as exc:  # noqa: BLE001
        return {"error": f"{type(exc).__name__}: {exc}"}
    try:
        return json.loads(out.stdout.strip().splitlines()[-1])
    except Exception:  # noqa: BLE001
        return {"error": "unparsable --once output", "stdout": out.stdout, "stderr": out.stderr}


def probe_open() -> str:
    """Can we open the pad *right now* and read board-info out of it?"""
    try:
        from touchy_pad.api import touchy_open

        pad = touchy_open()
    except Exception as exc:  # noqa: BLE001
        return f"FAILED ({type(exc).__name__}: {exc})"
    try:
        info = pad.board_info
        return f"OK ({info.display_width}x{info.display_height}, serial={info.serial!r})"
    finally:
        try:
            pad.close()
        except Exception:  # noqa: BLE001
            pass


def _fmt_enum(snap: dict) -> str:
    if snap.get("error"):
        return snap["error"]
    if snap.get("libusb_error"):
        return f"enumeration error: {snap['libusb_error']}"
    touchy = snap.get("libusb_touchy")
    return f"total={snap.get('libusb_total')} touchy={touchy or 'NOT FOUND'}"


def _fmt_sysfs(snap: dict) -> str:
    hits = snap.get("sysfs_touchy") or []
    if not hits:
        return "no Touchy-Pad in sysfs"
    return "; ".join(
        f"{h['sysfs']} -> node {h['node']} "
        f"(/dev:{'yes' if h['in_dev_bus_usb'] else 'NO'}, "
        f"/host/dev:{'yes' if h['in_host_dev_bus_usb'] else 'NO'})"
        for h in hits
    )


def report(snap: dict, fresh: dict, t: float) -> None:
    nodes = snap.get("nodes", {})
    helper = snap.get("helper_touchy")
    print(
        f"[{t:6.1f}s] sysfs(live): {_fmt_sysfs(snap)}\n"
        f"          this process (raw pyusb): {_fmt_enum(snap)}\n"
        f"          fresh process (raw pyusb): {_fmt_enum(fresh)}\n"
        f"          touchy_pad._usb (shipping code): {helper or 'NOT FOUND'}\n"
        f"          nodes: /dev/bus/usb={nodes.get('dev_bus_usb')} "
        f"/host/dev/bus/usb={nodes.get('host_dev_bus_usb')}"
    )
    if helper and not isinstance(helper, str):
        print(f"          open+board-info: {probe_open()}")
    sys.stdout.flush()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--interval", type=float, default=2.0, help="seconds between samples")
    ap.add_argument("--loops", type=int, default=0, help="0 = until ctrl-c")
    ap.add_argument("--once", action="store_true", help="one machine-readable snapshot")
    args = ap.parse_args()

    if args.once:
        print(json.dumps(snapshot()))
        return 0

    print(
        f"usb-diag: VID:PID {VID:04x}:{PID:04x}  libusb {libusb_version()}  "
        f"dev-container={os.environ.get('REMOTE_CONTAINERS', 'no')}  "
        f"/host/dev/bus/usb={'present' if os.path.isdir(HOST_USB_ROOT) else 'ABSENT'}\n"
        f"Unplug and re-plug the pad while this loops. ctrl-c to stop.\n"
    )
    t0 = time.monotonic()
    n = 0
    try:
        while args.loops <= 0 or n < args.loops:
            report(snapshot(), fresh_process_snapshot(), time.monotonic() - t0)
            n += 1
            if args.loops <= 0 or n < args.loops:
                time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\nusb-diag: bye")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
