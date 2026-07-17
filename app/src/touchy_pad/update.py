"""End-user firmware update flow for ``touchy update``.

This module knows how to:

1. Detect an ESP32-S3 ROM bootloader on the USB bus (VID/PID 0x303A/0x1001).
2. Talk to a running Touchy device to learn its board name.
3. Fetch a merged firmware image from a GitHub release.
4. Drive ``esptool`` to flash it.

The merged image is produced in CI by ``idf.py merge-bin`` and is laid
out so it can be written at flash offset ``0x0`` — bootloader, partition
table, and app are all bundled in. See ``.github/workflows/release.yml``
and the ``merge-bin`` / ``flash-merged`` recipes in the repository
``Justfile`` for the equivalent local flow.
"""

from __future__ import annotations

import glob
import os
import sys
import tempfile
import time
from collections.abc import Callable
from pathlib import Path

import click
import requests
import usb.core
from rich.console import Console
from rich.panel import Panel
from rich.progress import (
    BarColumn,
    DownloadColumn,
    Progress,
    TextColumn,
    TimeRemainingColumn,
    TransferSpeedColumn,
)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

GITHUB_REPO = "geeksville/touchy-pad"

# ESP32-S3 in ROM (or stock second-stage) USB bootloader mode.
ESP_BOOTLOADER_VID = 0x303A
ESP_BOOTLOADER_PID = 0x1001

# Mirrors the matrix in .github/workflows/release.yml and the
# directories under firmware/boards/. Update both sides together when
# adding a new board.
SUPPORTED_BOARDS: tuple[str, ...] = (
    "jc4827w543",
    "jc4827w543r",
    "waveshare_s3_lcd_7b",
    "esp32_2432s028rv3",
    "esp32_s3_devkitc_1",
    "elecrow_p4_lcd_7",
    "elecrow_s3_lcd_7",
    "elecrow_s3_lcd_7_adv",
    "squixl",
    "matouch_43",
)

# How long to wait for the user to enter the bootloader after we ask
# them to hold BOOT and replug the device.
_BOOTLOADER_WAIT_SECS = 60


# ---------------------------------------------------------------------------
# Public entry point
# ---------------------------------------------------------------------------


def run_update(
    *,
    board: str | None,
    version: str,
    port: str | None,
    client_factory: Callable[[], object],
) -> None:
    """Top-level driver for ``touchy update``.

    ``client_factory`` is the CLI's ``_client`` helper; it lets us reuse
    the same device-opening logic (and its error messages) without a
    circular import.
    """
    console = Console()

    bootloader_present = _bootloader_visible()

    # ------------------------------------------------------------------
    # Case 1: bootloader already visible on the bus.
    # ------------------------------------------------------------------
    if bootloader_present:
        if not board:
            _die_no_board(console)
        _validate_board(board, console)
        _confirm_dangerous_board(console, board)
        _do_flash(console, board=board, version=version, port=port)
        return

    # ------------------------------------------------------------------
    # Case 2: no bootloader — try the running device to learn its board.
    # ------------------------------------------------------------------
    detected_board: str | None = None
    try:
        # client_factory typically exits the process if no device is
        # reachable; we intercept that with SystemExit below.
        client = client_factory()
    except SystemExit:
        client = None
    except Exception as e:  # pragma: no cover - defensive
        console.print(f"[yellow]warning:[/yellow] could not query device: {e}")
        client = None

    if client is not None:
        try:
            with client as c:  # type: ignore[attr-defined]
                info = c.sys_board_info_get()
            detected_board = info.board_name or None
            if detected_board:
                console.print(
                    f"[green]✓[/green] detected running device: "
                    f"board=[bold]{detected_board}[/bold] "
                    f"firmware={info.firmware_version_str}"
                )
        except Exception as e:
            console.print(f"[yellow]warning:[/yellow] device query failed: {e}")

    # Reconcile --board with what the device reports.
    if detected_board and board and board != detected_board:
        if not click.confirm(
            f"--board={board!r} differs from connected device ({detected_board!r}). "
            "Flash anyway?",
            default=False,
        ):
            console.print("aborted.")
            sys.exit(1)
        target_board = board
    else:
        target_board = board or detected_board

    if not target_board:
        _die_no_board(console)
    _validate_board(target_board, console)

    # ------------------------------------------------------------------
    # Walk the user into the bootloader.
    # ------------------------------------------------------------------
    console.print(
        Panel.fit(
            "[bold]Hold the BOOT button[/bold] on the device, then unplug and re-plug it "
            "via USB. Keep BOOT held until the device re-enumerates.\n\n"
            "Waiting for the ESP32-S3 bootloader to appear...",
            title="Manual step required",
            border_style="cyan",
        )
    )
    if not _wait_for_bootloader(console, timeout=_BOOTLOADER_WAIT_SECS):
        console.print(
            "[red]error:[/red] timed out waiting for the bootloader to appear. "
            "Re-run the command after entering bootloader mode."
        )
        sys.exit(2)

    _confirm_dangerous_board(console, target_board)
    _do_flash(console, board=target_board, version=version, port=port)


# ---------------------------------------------------------------------------
# Bootloader / USB helpers
# ---------------------------------------------------------------------------


def _bootloader_visible() -> bool:
    """Return True iff an ESP32-S3 ROM/serial-JTAG bootloader is on the bus.

    Prefer a direct sysfs scan over ``usb.core.find()``: pyusb/libusb
    enumeration is unreliable in the touchy-pad devcontainer (it tends
    to miss devices that were hot-plugged after the libusb context was
    created, even though they show up in /sys and /host/dev), which is
    exactly the case we hit when the user enters bootloader mode by
    holding BOOT and replugging mid-command. See the matching
    ``_install_host_dev_fallback`` workaround in ``api/_transport.py`` —
    same root cause (the sandboxed /dev/bus/usb), different libusb code
    path.

    /sys/bus/usb/devices/*/idVendor and /idProduct are populated by the
    kernel synchronously on every enumeration, so this sees the device
    the moment it appears.
    """
    # 1. Authoritative: sysfs (works for hot-plugged devices).
    try:
        for entry in glob.glob("/sys/bus/usb/devices/*/idVendor"):
            try:
                vid = int(open(entry).read().strip(), 16)
                pid = int(open(entry.replace("idVendor", "idProduct")).read().strip(), 16)
            except (OSError, ValueError):
                continue
            if vid == ESP_BOOTLOADER_VID and pid == ESP_BOOTLOADER_PID:
                return True
    except OSError:
        pass

    # 2. Fallback for non-Linux hosts (macOS/Windows have no /sys).
    try:
        dev = usb.core.find(idVendor=ESP_BOOTLOADER_VID, idProduct=ESP_BOOTLOADER_PID)
    except Exception:
        return False
    return dev is not None


def _wait_for_bootloader(console: Console, *, timeout: int) -> bool:
    deadline = time.monotonic() + timeout
    with console.status("[cyan]Polling USB for the bootloader...", spinner="dots"):
        while time.monotonic() < deadline:
            if _bootloader_visible():
                console.print("[green]✓[/green] bootloader detected.")
                return True
            time.sleep(0.5)
    return False


def _autodetect_serial_port() -> str | None:
    """Pick the first writable ttyACM*/ttyUSB* on Linux, or None.

    Mirrors the heuristic the Justfile `flash` recipe uses. esptool can
    also auto-detect via libusb when ``-p`` is omitted, so a None return
    is fine — we just delegate to esptool.
    """
    candidates: list[str] = []
    for root in ("/host/dev", "/dev"):
        candidates.extend(sorted(glob.glob(f"{root}/ttyACM*")))
        candidates.extend(sorted(glob.glob(f"{root}/ttyUSB*")))
    for c in candidates:
        if os.access(c, os.R_OK | os.W_OK):
            return c
    return None


# ---------------------------------------------------------------------------
# Board validation / user-facing errors
# ---------------------------------------------------------------------------


def _validate_board(board: str, console: Console) -> None:
    if board in SUPPORTED_BOARDS:
        return
    console.print(f"[red]error:[/red] unknown board {board!r}. Supported boards:")
    for b in SUPPORTED_BOARDS:
        console.print(f"  • {b}")
    sys.exit(2)


def _die_no_board(console: Console) -> None:
    console.print(
        Panel(
            "[red]No board name supplied[/red] and no running Touchy device "
            "is reachable to auto-detect one.\n\n"
            "[bold yellow]⚠  WARNING:[/bold yellow] flashing the wrong firmware "
            "for your hardware can [bold]permanently damage the display or "
            "other peripherals[/bold]. Always pick the board that matches "
            "your physical device.\n\n"
            "Supported boards:\n"
            + "\n".join(f"  • {b}" for b in SUPPORTED_BOARDS)
            + "\n\nRetry with: [cyan]touchy update --board <BOARD>[/cyan]",
            title="Cannot proceed",
            border_style="red",
        )
    )
    sys.exit(2)


def _confirm_dangerous_board(console: Console, board: str) -> None:
    console.print(f"About to flash firmware for board [bold cyan]{board}[/bold cyan].")
    console.print("[yellow]⚠  Flashing the wrong board image can damage hardware.[/yellow]")
    if not click.confirm("Continue?", default=True):
        console.print("aborted.")
        sys.exit(1)


# ---------------------------------------------------------------------------
# Download
# ---------------------------------------------------------------------------


def _asset_url(board: str, version: str) -> str:
    if version == "latest":
        return f"https://github.com/{GITHUB_REPO}/releases/latest/download/{board}.bin"
    # Allow either "v0.2.0" or "0.2.0".
    if not version.startswith("v"):
        version = f"v{version}"
    return f"https://github.com/{GITHUB_REPO}/releases/download/{version}/{board}.bin"


def _download(console: Console, url: str, dest: Path) -> None:
    console.print(f"Downloading [cyan]{url}[/cyan]")
    with requests.get(url, stream=True, allow_redirects=True, timeout=30) as resp:
        if resp.status_code == 404:
            console.print(
                f"[red]error:[/red] no release asset found at {url}\n"
                "  (Has CI finished publishing this version for this board?)"
            )
            sys.exit(2)
        resp.raise_for_status()
        total = int(resp.headers.get("Content-Length") or 0)
        with (
            Progress(
                TextColumn("[bold blue]downloading"),
                BarColumn(),
                DownloadColumn(),
                TransferSpeedColumn(),
                TimeRemainingColumn(),
                console=console,
            ) as progress,
            dest.open("wb") as f,
        ):
            task = progress.add_task("download", total=total or None)
            for chunk in resp.iter_content(chunk_size=64 * 1024):
                if not chunk:
                    continue
                f.write(chunk)
                progress.update(task, advance=len(chunk))


# ---------------------------------------------------------------------------
# Flash
# ---------------------------------------------------------------------------


def _do_flash(
    console: Console,
    *,
    board: str,
    version: str,
    port: str | None,
) -> None:
    url = _asset_url(board, version)
    with tempfile.TemporaryDirectory(prefix="touchy-update-") as tmp:
        firmware_path = Path(tmp) / f"{board}.bin"
        _download(console, url, firmware_path)
        size = firmware_path.stat().st_size
        console.print(f"[green]✓[/green] downloaded {size:,} bytes")

        resolved_port = port or _autodetect_serial_port()
        if resolved_port:
            console.print(f"using serial port [cyan]{resolved_port}[/cyan]")
        else:
            console.print(
                "[yellow]note:[/yellow] no --port given and no ttyACM*/ttyUSB* "
                "auto-detected; letting esptool probe the bus."
            )

        _invoke_esptool(console, firmware=firmware_path, port=resolved_port)
    console.print(
        Panel.fit(
            "[bold green]✓ Update succeeded.[/bold green]\n"
            "Please unplug and replug your Touchy-Pad to run the application.",
            border_style="green",
        )
    )


def _invoke_esptool(
    console: Console,
    *,
    firmware: Path,
    port: str | None,
) -> None:
    """Call esptool's Python entry point with arguments mirroring the Justfile.

    esptool.main() raises SystemExit on failure (Python esptool >= 4),
    so we don't need to inspect a return value.
    """
    import esptool

    args: list[str] = []
    if port:
        args += ["-p", port]
    args += [
        "-b",
        "460800",
        "--before",
        "default-reset",
        "--after",
        "hard-reset",
        "--chip",
        "esp32s3",
        "write-flash",
        "0x0",
        str(firmware),
    ]
    console.print(f"[bold]running:[/bold] esptool {' '.join(args)}")
    try:
        esptool.main(args)
    except SystemExit as e:
        # esptool exits with a non-zero code on failure.
        code = e.code if isinstance(e.code, int) else 1
        if code != 0:
            console.print(f"[red]esptool failed (exit {code})[/red]")
            sys.exit(code)
    except esptool.util.FatalError as e:
        console.print(f"[red]esptool fatal error: {e}[/red]")
        console.print(
            "[yellow]You might need to follow the instructions at: https://github.com/geeksville/touchy-pad/blob/main/docs/udev.md[/yellow]"
        )
        sys.exit(1)
