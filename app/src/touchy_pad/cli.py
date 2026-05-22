"""Command-line interface for the Touchy-Pad device.

Run ``touchy --help`` for a list of subcommands.
"""

from __future__ import annotations

import sys
from pathlib import Path

import click

from .client import TouchyClient, TouchyError
from .transport import DeviceNotFoundError, Transport


def _parse_size(ctx, param, value: str | None) -> tuple[int, int] | None:
    if value is None:
        return None
    try:
        w, h = value.lower().split("x", 1)
        return (int(w), int(h))
    except Exception as e:
        raise click.BadParameter(f"--sim-size must be WxH (e.g. 480x300), got {value!r}") from e


@click.group(invoke_without_command=True)
@click.version_option()
@click.option(
    "--sim",
    "sim",
    is_flag=True,
    help="Talk to an in-process Python device simulator instead of USB. "
    "Opens a Qt window unless --sim-headless is also given.",
)
@click.option(
    "--sim-headless",
    is_flag=True,
    help="Like --sim, but skips the GUI window. Implies --sim.",
)
@click.option(
    "--sim-size",
    metavar="WxH",
    callback=_parse_size,
    default=None,
    help="Sim window size in pixels (default 480x300). Ignored in headless mode.",
)
@click.option(
    "--sim-serial",
    default="SIM0000",
    show_default=True,
    metavar="SERIAL",
    help="Pseudo-USB serial for the sim (separates per-serial cache dirs).",
)
@click.option(
    "--sim-dir",
    type=click.Path(file_okay=False, path_type=Path),
    default=None,
    metavar="DIR",
    help="Sim pseudo-fs root (default: platformdirs user cache).",
)
@click.pass_context
def cli(
    ctx: click.Context,
    sim: bool,
    sim_headless: bool,
    sim_size: tuple[int, int] | None,
    sim_serial: str,
    sim_dir: Path | None,
) -> None:
    """Talk to a connected Touchy-Pad USB device."""
    ctx.ensure_object(dict)
    sim_active = sim or sim_headless
    sim_gui = sim and not sim_headless
    ctx.obj["sim"] = sim_active
    ctx.obj["sim_headless"] = sim_headless
    ctx.obj["sim_gui"] = sim_gui
    ctx.obj["sim_size"] = sim_size or (480, 300)
    ctx.obj["sim_serial"] = sim_serial
    ctx.obj["sim_dir"] = sim_dir

    if not sim_active:
        if ctx.invoked_subcommand is None:
            click.echo(ctx.get_help())
            ctx.exit()
        return

    # Build one shared SimDeviceTransport (and, in GUI mode, the
    # Qt window pointing at its SimDevice) up-front. The subcommand
    # then talks to that same SimDevice, so its effects (uploads,
    # screen switches, host events) show up live in the window.
    #
    # Goes through the public `create_sim_device` helper so the sim
    # is registered in the process-wide registry — that's what makes
    # `touchy_get_pad_ids()` and the touchydeck StreamDeck-compat
    # enumeration hook see it too.
    from .api import create_sim_device

    transport = create_sim_device(
        headless=not sim_gui,
        serial=sim_serial,
        fs_root=sim_dir,
    )
    ctx.obj["sim_transport"] = transport
    # Closed after the result callback's app.exec() returns (or
    # immediately after the subcommand in headless mode).
    from .api import destroy_sim_device

    ctx.call_on_close(destroy_sim_device)

    if not sim_gui:
        return

    try:
        from PySide6 import QtWidgets

        from .sim.window import SimWindow
    except ImportError as e:
        click.echo(
            "error: PySide6 is required for the --sim GUI window "
            "(install with `pip install 'touchy-pad[sim]'`), or pass "
            f"--sim-headless to skip the window: {e}",
            err=True,
        )
        sys.exit(2)

    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    window = SimWindow(transport.device, size=ctx.obj["sim_size"])
    window.show()
    ctx.obj["sim_app"] = app
    ctx.obj["sim_window"] = window


@cli.result_callback()
@click.pass_context
def _after_subcommand(ctx: click.Context, _result, **_kwargs) -> None:
    """Run the Qt event loop after the subcommand body returns.

    In GUI sim mode the subcommand finishes its synchronous work
    (uploads, screen_load, registering host-event handlers, ...) and
    then we hand control to ``QApplication.exec()`` so the window
    stays interactive until the user closes it or hits Ctrl+C.
    """
    if not ctx.obj.get("sim_gui"):
        return
    app = ctx.obj["sim_app"]

    import signal

    from PySide6 import QtCore

    signal.signal(signal.SIGINT, lambda *_: app.quit())
    # No-op timer keeps the Python interpreter ticking so the SIGINT
    # handler runs promptly instead of being parked in Qt's C loop.
    _tick = QtCore.QTimer()
    _tick.start(200)
    _tick.timeout.connect(lambda: None)
    click.echo("sim window open \u2014 close it (or Ctrl-C) to exit.")
    app.exec()


def _make_transport() -> Transport | None:
    """Return the shared sim transport built by the group, or ``None``.

    A thin :class:`_SharedSimTransport` proxy is returned so that the
    subcommand's ``with _open_pad() as pad:`` block doesn't tear down
    the real transport (the GUI loop runs *after* the subcommand and
    keeps using it). Callers fall back to USB when this returns
    ``None``.
    """
    ctx = click.get_current_context(silent=True)
    if ctx is None or not ctx.obj or not ctx.obj.get("sim"):
        return None
    inner = ctx.obj.get("sim_transport")
    if inner is None:
        return None
    return _SharedSimTransport(inner)


class _SharedSimTransport(Transport):
    """Forwarding proxy that makes :meth:`close` a no-op.

    The underlying sim transport is owned by the click context and
    closed via :meth:`click.Context.call_on_close`; the subcommand's
    client / pad context manager would otherwise close it on the way
    out of its ``with`` block, killing the device before the GUI loop
    even starts.
    """

    def __init__(self, inner: Transport) -> None:
        self._inner = inner
        # Inherit the inner transport's image-conversion policy so the
        # high-level API skips PNG→LVGL conversion for the sim.
        self.needs_image_conversion = getattr(inner, "needs_image_conversion", True)

    def send_command(self, payload: bytes) -> None:
        self._inner.send_command(payload)

    def recv_response(self, timeout_ms: int = 2000) -> bytes:
        return self._inner.recv_response(timeout_ms)

    def close(self) -> None:
        # Intentional no-op; see class docstring.
        return


def _client() -> TouchyClient:
    t = _make_transport()
    if t is not None:
        return TouchyClient(t)
    try:
        return TouchyClient.open()
    except DeviceNotFoundError as e:
        click.echo(f"error: {e}", err=True)
        sys.exit(2)


def _open_pad():
    """Counterpart to :func:`_client` that goes through the high-level API."""
    from .api import touchy_open

    return touchy_open(transport=_make_transport())


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
@click.pass_context
def screens_demo(ctx: click.Context, listen: bool, as_json: bool) -> None:
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

    with _open_pad() as pad:
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
            if ctx.obj.get("sim_gui"):
                # The group's result callback runs QApplication.exec()
                # right after this returns, which both keeps the
                # window interactive *and* parks the main thread
                # — host events fire from the sim's worker thread
                # and print straight to stdout from there.
                click.echo(
                    "listening for host events; close the sim window or press " "Ctrl-C to stop."
                )
            else:
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
