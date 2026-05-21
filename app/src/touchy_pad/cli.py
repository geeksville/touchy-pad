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


@screen.command("push")
@click.argument(
    "script",
    type=click.Path(exists=True, dir_okay=False, path_type=Path),
)
@click.option(
    "--load",
    "load_name",
    metavar="NAME",
    help="After pushing, immediately switch to this screen.",
)
@click.option(
    "--dry-run",
    is_flag=True,
    help="Compile the script and report what would be uploaded; " "do not talk to a device.",
)
def screens_push(script: Path, load_name: str | None, dry_run: bool) -> None:
    """Compile SCRIPT (a Python file using touchy_pad.screens) and upload
    every Screen it defines to the device as screens/<name>.pb.
    """
    # Imported lazily so `touchy --help` doesn't pull protobuf in unless
    # the user actually invokes the screens subgroup.
    from .screens import _collect_from_script

    found = _collect_from_script(script)
    if not found:
        click.echo(f"warning: {script} defined no Screen objects", err=True)
        return

    if dry_run:
        for s in found:
            data = s.to_bytes()
            click.echo(
                f"would upload screens/{s.name}.pb ({len(data)} bytes, "
                f"{len(s.widgets)} widgets)"
            )
        if load_name:
            click.echo(f"would load screen {load_name!r}")
        return

    with _client() as c:
        for s in found:
            data = s.to_bytes()
            c.file_save(f"screens/{s.name}.pb", data)
            click.echo(f"sent screens/{s.name}.pb ({len(data)} bytes)")
        if load_name:
            c.screen_load(load_name)
            click.echo(f"loaded screen {load_name!r}")


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
    """Upload a sample screen exercising stage-16 actions.

    The screen contains:
      * a "hi" button wired to a device-side macro that types the text
        over USB HID (no host involvement);
      * a "ping" button, slider and checkbox wired to host actions
        (codes 0x100 / 0x101 / 0x102);
      * a 16x16 PNG image button (Stage 20) wired to host action 0x103.
        Its asset is auto-uploaded to /from_host/images/smiley.png; the
        host transparently converts the PNG to LVGL's native .bin
        format before sending it.

    With ``--listen`` the CLI registers Python handlers for the host
    action codes and prints the incoming events.
    """
    from .images import make_smiley_png
    from .screens import build_demo_screen

    s = build_demo_screen("demo")

    if as_json:
        from google.protobuf import json_format

        click.echo(json_format.MessageToJson(s.to_proto(), indent=2))
        return

    smiley = make_smiley_png()
    data = s.to_bytes()
    with _client() as c:
        c.file_save("images/smiley.png", smiley)
        click.echo(f"sent images/smiley.png ({len(smiley)} bytes source)")
        c.file_save(f"screens/{s.name}.pb", data)
        click.echo(f"sent screens/{s.name}.pb ({len(data)} bytes, " f"{len(s.widgets)} widgets)")
        c.screen_load(s.name)
        click.echo(f"loaded screen {s.name!r}")

        if listen:

            def on_ping(evt):
                click.echo(f"[ping]   widget={evt.user_data!r}")

            def on_level(evt):
                click.echo(f"[slider] widget={evt.user_data!r} value={evt.value}")

            def on_enable(evt):
                click.echo(f"[check]  widget={evt.user_data!r} on={evt.checked}")

            def on_smile(evt):
                click.echo(f"[smile]  widget={evt.user_data!r}")

            c.on_host_event(0x100, on_ping)
            c.on_host_event(0x101, on_level)
            c.on_host_event(0x102, on_enable)
            c.on_host_event(0x103, on_smile)
            click.echo("listening for host events (Ctrl-C to stop)...")
            try:
                for _ in c.stream_events():
                    pass
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
