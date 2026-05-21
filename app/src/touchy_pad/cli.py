"""Command-line interface for the Touchy-Pad device.

Run ``touchy --help`` for a list of subcommands.
"""

from __future__ import annotations

import sys
from pathlib import Path

import click

from .client import TouchyClient, TouchyError
from .transport import DeviceNotFoundError


@click.group()
@click.version_option()
def cli() -> None:
    """Talk to a connected Touchy-Pad USB device."""


def _client() -> TouchyClient:
    try:
        return TouchyClient.open()
    except DeviceNotFoundError as e:
        click.echo(f"error: {e}", err=True)
        sys.exit(2)


@cli.command()
def version() -> None:
    """Print device protocol & firmware version."""
    with _client() as c:
        v = c.sys_version_get()
        click.echo(f"protocol: {v.protocol_version}")
        click.echo(f"firmware: {v.firmware_version} ({v.firmware_version_str})")


# Alias to match the spelling used in docs/design.md (Stage 12).
cli.add_command(version, name="getversion")


@cli.command("file-reset")
def file_reset() -> None:
    """Delete every file the host has uploaded to the device."""
    with _client() as c:
        c.file_reset()


@cli.command("file-save")
@click.argument("path")
@click.argument("file", type=click.File("rb"))
def file_save(path: str, file) -> None:
    """Upload a single file to the device under /from_host/<PATH>."""
    with _client() as c:
        c.file_save(path, file.read())


@cli.command("writefiles")
@click.argument(
    "srcdir",
    type=click.Path(exists=True, file_okay=False, path_type=Path),
)
def writefiles(srcdir: Path) -> None:
    """Mirror SRCDIR onto the device.

    First clears the host-uploaded file area, then recursively walks
    SRCDIR and uploads every file at its relative path.
    """
    with _client() as c:
        c.file_reset()
        for p in sorted(srcdir.rglob("*")):
            if not p.is_file():
                continue
            rel = p.relative_to(srcdir).as_posix()
            c.file_save(rel, p.read_bytes())
            click.echo(f"sent {rel} ({p.stat().st_size} bytes)")


@cli.command()
def events() -> None:
    """Stream events from the device until interrupted.

    Every queued `ActionHost` event is printed; the exit is on Ctrl-C.
    """
    with _client() as c:
        try:
            for evt in c.stream_events():
                which = evt.WhichOneof("state")
                if which == "value":
                    state_str = f"value={evt.value}"
                elif which == "checked":
                    state_str = f"checked={evt.checked}"
                else:
                    state_str = ""
                click.echo(
                    f"event code={evt.code} host_code=0x{evt.host_code:x} "
                    f"widget={evt.user_data!r}" + (f" {state_str}" if state_str else "")
                )
        except KeyboardInterrupt:
            pass


# ---------------------------------------------------------------------------
# Screen — backlight control, layout management, and screen authoring
# ---------------------------------------------------------------------------


@cli.group()
def screen() -> None:
    """Manage screens: backlight, layout upload, and screen authoring."""


@screen.command("wake")
def screen_wake() -> None:
    """Turn the device backlight on (cancels any pending auto-sleep)."""
    with _client() as c:
        c.screen_wake()


@screen.command("set-timeout")
@click.argument("seconds", type=float)
def screen_set_timeout(seconds: float) -> None:
    """Auto-sleep the backlight after SECONDS of no input (0 disables)."""
    with _client() as c:
        c.screen_sleep_timeout(round(seconds * 1000))


@screen.command("load")
@click.argument("name")
def screen_load(name: str) -> None:
    """Switch the currently displayed screen to NAME."""
    with _client() as c:
        c.screen_load(name)


@screen.command("demo")
@click.option(
    "--listen",
    is_flag=True,
    help="After uploading, stream host events from the demo screen until "
    "Ctrl-C. Registers handlers for the demo's host action codes.",
)
@click.option(
    "--json",
    "as_json",
    is_flag=True,
    help="Print the screen definition as protobuf JSON to stdout; " "do not talk to the device.",
)
def screens_demo(listen: bool, as_json: bool) -> None:
    """Upload the sample multi-screen demo (stages 16, 18, 20, 24).

    Uploads two screens that share a ``[Prev | FPS | Next]`` header
    row driven by Stage-24 device-side ``ActionSwitchScreen`` actions:

      * ``home`` — full-bleed multitouch trackpad for USB HID mouse;
      * ``test`` — the widget showcase (macro button, ping/slider/
        checkbox host actions 0x100/0x101/0x102, the Stage-20 smiley
        image button on 0x103, log line).

    The 16x16 smiley PNG asset is auto-uploaded to
    ``/from_host/images/smiley.png``; the host transparently converts
    it to LVGL's native ``.bin`` format before sending.

    After upload the device is told to load ``home``. With ``--listen``
    the CLI registers Python handlers for the test screen's host
    action codes and prints incoming events (flip to the ``test``
    screen on-device with the ``Next >`` button).
    """
    from .api.images import make_smiley_png
    from .api.screens import build_demo_screens

    screens = build_demo_screens()

    if as_json:
        from google.protobuf import json_format

        for s in screens:
            click.echo(f"// screen: {s.name}")
            click.echo(json_format.MessageToJson(s.to_proto(), indent=2))
        return

    smiley = make_smiley_png()
    from .api import touchy_open

    with touchy_open() as pad:
        pad.file_save("images/smiley.png", smiley)
        click.echo(f"sent images/smiley.png ({len(smiley)} bytes source)")
        for s in screens:
            pad.screen_save(s)
            click.echo(f"sent screens/{s.name}.pb ({len(s.widgets)} widgets)")
        pad.screen_load("home")
        click.echo("loaded screen 'home'")

        if listen:

            def on_ping(evt):
                click.echo(f"[ping]   widget={evt.user_data!r}")

            def on_level(evt):
                click.echo(f"[slider] widget={evt.user_data!r} value={evt.value}")

            def on_enable(evt):
                click.echo(f"[check]  widget={evt.user_data!r} on={evt.checked}")

            def on_smile(evt):
                click.echo(f"[smile]  widget={evt.user_data!r}")

            pad.on_host_event(0x100, on_ping)
            pad.on_host_event(0x101, on_level)
            pad.on_host_event(0x102, on_enable)
            pad.on_host_event(0x103, on_smile)
            click.echo("listening for host events (Ctrl-C to stop)...")
            try:
                import threading

                threading.Event().wait()
            except KeyboardInterrupt:
                pass


@cli.command("reboot-bootloader")
def reboot_bootloader() -> None:
    """Reboot the device into its USB DFU bootloader."""
    with _client() as c:
        c.sys_reboot_bootloader()


def main() -> None:
    try:
        cli()
    except TouchyError as e:
        click.echo(f"device error: {e}", err=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
