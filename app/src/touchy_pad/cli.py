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


@cli.command("screen-wake")
def screen_wake() -> None:
    """Turn the device backlight on (cancels any pending auto-sleep)."""
    with _client() as c:
        c.screen_wake()


@cli.command("screen-sleep-timeout")
@click.argument("timeout_ms", type=int)
def screen_sleep_timeout(timeout_ms: int) -> None:
    """Set the backlight auto-sleep timeout in milliseconds (0 disables)."""
    with _client() as c:
        c.screen_sleep_timeout(timeout_ms)


@cli.command("screen-load")
@click.argument("name")
def screen_load(name: str) -> None:
    """Switch the currently displayed screen."""
    with _client() as c:
        c.screen_load(name)


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
    """Stream events from the device until interrupted."""
    with _client() as c:
        try:
            for evt in c.stream_events():
                which = evt.WhichOneof("evt")
                if which == "lv":
                    click.echo(
                        f"event code={evt.lv.code} user_data={evt.lv.user_data!r}"
                    )
                else:
                    click.echo(f"event {which}")
        except KeyboardInterrupt:
            pass


# ---------------------------------------------------------------------------
# Screens — protobuf-encoded layouts (see touchy_pad.screens)
# ---------------------------------------------------------------------------


@cli.group()
def screens() -> None:
    """Author and upload declarative screen layouts."""


@screens.command("push")
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
    help="Compile the script and report what would be uploaded; "
         "do not talk to a device.",
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
            click.echo(f"would upload screens/{s.name}.pb ({len(data)} bytes, "
                       f"{len(s.widgets)} widgets)")
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


@screens.command("demo")
@click.option(
    "--name",
    default="demo",
    show_default=True,
    help="Screen name to register on the device.",
)
@click.option(
    "--no-load",
    is_flag=True,
    help="Upload the screen but do not switch to it.",
)
def screens_demo(name: str, no_load: bool) -> None:
    """Upload a 4-widget sample screen and switch to it.

    Smoke test for the stage-15 layout pipeline: builds a screen with a
    label, button, slider, and switch via the Python DSL, sends it as
    ``screens/<name>.pb``, and (unless ``--no-load``) calls
    ``screen-load`` to activate it.
    """
    from .screens import build_demo_screen

    s = build_demo_screen(name)
    data = s.to_bytes()
    with _client() as c:
        c.file_save(f"screens/{s.name}.pb", data)
        click.echo(f"sent screens/{s.name}.pb ({len(data)} bytes, "
                   f"{len(s.widgets)} widgets)")
        if not no_load:
            c.screen_load(s.name)
            click.echo(f"loaded screen {s.name!r}")


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
