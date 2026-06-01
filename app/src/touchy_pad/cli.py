"""Command-line interface for the Touchy-Pad device.

Run ``touchy --help`` for a list of subcommands.
"""

from __future__ import annotations

import logging
import sys
from pathlib import Path

import click

from .client import TouchyClient, TouchyError
from .transport import DeviceNotFoundError, Transport

logger = logging.getLogger(__name__)


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
    "--debug",
    is_flag=True,
    help="Enable debug logging (sets log level to DEBUG).",
)
@click.option(
    "--sim-remote",
    "sim_remote",
    metavar="[HOST:PORT]",
    is_flag=False,
    flag_value="",  # bare --sim-remote means "default loopback"
    default=None,
    help="Connect to an out-of-process simulator over TCP. "
    "Defaults to 127.0.0.1:8935; pass HOST:PORT to override.",
)
@click.option(
    "--sim-headless",
    "sim_headless",
    is_flag=True,
    help="Spawn an in-process sim server on an ephemeral loopback port "
    "and connect to it. No GUI window.",
)
@click.option(
    "--sim-gui",
    "sim_gui",
    is_flag=True,
    help="Spawn an in-process sim server and open a Qt window viewing it.",
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
@click.option(
    "--port",
    "port",
    type=click.Path(dir_okay=False, path_type=str),
    default=None,
    metavar="PATH",
    help="Serial port path (e.g. `/dev/ttyACM0` or `COM3`). When set, "
    "protocol commands talk to the device over this serial port at "
    "115200 baud instead of auto-discovering it by USB VID/PID. Also "
    "used by esptool-based commands such as `update`.",
)
@click.pass_context
def cli(
    ctx: click.Context,
    debug: bool,
    sim_remote: str | None,
    sim_headless: bool,
    sim_gui: bool,
    sim_size: tuple[int, int] | None,
    sim_serial: str,
    sim_dir: Path | None,
    port: str | None,
) -> None:
    """Talk to a connected Touchy-Pad USB device."""
    # Configure logging level
    log_level = logging.DEBUG if debug else logging.INFO
    logging.basicConfig(
        level=log_level,
        format="%(levelname)s:%(name)s:%(message)s",
        force=True,  # Override any existing configuration
    )
    # Suppress noisy RPC trace logs even in debug mode
    logging.getLogger("touchy_pad.client.rpc").setLevel(logging.INFO)

    ctx.ensure_object(dict)
    sim_modes = [bool(sim_remote is not None), sim_headless, sim_gui]
    if sum(sim_modes) > 1:
        raise click.UsageError("--sim-remote, --sim-headless and --sim-gui are mutually exclusive.")
    sim_active = any(sim_modes)
    ctx.obj["sim"] = sim_active
    ctx.obj["sim_remote"] = sim_remote
    ctx.obj["sim_headless"] = sim_headless
    ctx.obj["sim_gui"] = sim_gui
    ctx.obj["sim_size"] = sim_size or (480, 300)
    ctx.obj["sim_serial"] = sim_serial
    ctx.obj["sim_dir"] = sim_dir
    ctx.obj["port"] = port

    if not sim_active:
        if ctx.invoked_subcommand is None:
            click.echo(ctx.get_help())
            ctx.exit()
        return

    # Stage 63: every sim path now goes through the network transport
    # so the wire framing is exercised identically to USB.
    #
    # * --sim-remote: just connect a TcpTransport to the user-specified
    #   (or default) host:port; no server is owned by this process.
    # * --sim-headless / --sim-gui: spin up an in-process SimServer on
    #   an ephemeral loopback port and connect a TcpTransport to it.
    #   GUI mode additionally opens a Qt window viewing the server's
    #   SimDevice. Registered with the api.sim_registry so
    #   touchy_get_pad_ids() and the touchydeck enumerate hook still
    #   see it.

    if sim_remote is not None:
        from .transport_net import DEFAULT_SIM_PORT, TcpTransport, parse_sim_url

        if sim_remote:
            host, tcp_port = parse_sim_url(sim_remote)
        else:
            host, tcp_port = ("127.0.0.1", DEFAULT_SIM_PORT)
        for opt, val in (
            ("--sim-size", sim_size),
            ("--sim-serial", sim_serial if sim_serial != "SIM0000" else None),
            ("--sim-dir", sim_dir),
        ):
            if val:
                logger.warning("%s ignored under --sim-remote", opt)
        transport = TcpTransport(host, tcp_port)
        ctx.obj["sim_transport"] = transport
        ctx.call_on_close(transport.close)
        return

    from .api import create_sim_device, destroy_sim_device

    transport = create_sim_device(
        headless=not sim_gui,
        serial=sim_serial,
        fs_root=sim_dir,
        display_size=tuple(ctx.obj["sim_size"]),
        network=True,
    )
    ctx.obj["sim_transport"] = transport
    ctx.call_on_close(destroy_sim_device)

    if not sim_gui:
        return

    try:
        from PySide6 import QtWidgets

        from .sim.window import SimWindow
    except ImportError as e:
        logger.error(
            "PySide6 is required for the --sim-gui window "
            "(install with `pip install 'touchy-pad[sim]'`), or pass "
            "--sim-headless to skip the window: %s",
            e,
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
    logger.info("sim window open \u2014 close it (or Ctrl-C) to exit.")
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
    if ctx is None or not ctx.obj:
        return None
    if ctx.obj.get("sim"):
        inner = ctx.obj.get("sim_transport")
        if inner is None:
            return None
        return _SharedSimTransport(inner)
    # Not a sim run: if the user pointed us at a serial port, talk the
    # protocol over it instead of auto-discovering a USB device.
    port = ctx.obj.get("port")
    if port:
        from .transport_serial import SerialTransport

        return SerialTransport(port)
    return None


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
        logger.error("%s", e)
        sys.exit(2)


def _open_pad():
    """Counterpart to :func:`_client` that goes through the high-level API."""
    from .api import touchy_open

    return touchy_open(transport=_make_transport())


@cli.command("simulator")
@click.option(
    "--headless",
    is_flag=True,
    help="Don't open a Qt window; just listen on TCP.",
)
@click.option(
    "--bind",
    "bind",
    default="127.0.0.1",
    show_default=True,
    metavar="HOST",
    help="Bind address. Use 0.0.0.0 to accept non-loopback clients "
    "(prints a warning; no auth is performed).",
)
@click.option(
    "--port",
    "sim_port",
    type=int,
    default=None,
    show_default=False,
    metavar="PORT",
    help="TCP port (default: TOUCHY_SIM_PORT constant, currently 8935).",
)
@click.pass_context
def simulator_cmd(ctx: click.Context, headless: bool, bind: str, sim_port: int | None) -> None:
    """Run the device simulator as a standalone TCP server.

    Speaks the same length-prefixed nanopb framing the real device's
    USB bulk endpoints do, so any host-side client (Python, Rust,
    StreamController, OpenDeck plugin, ...) reaches it the same way
    it reaches hardware.

    By default opens a Qt window viewing the simulated screen. Pass
    ``--headless`` for CI / SSH use.
    """
    from .sim.server import SimServer
    from .transport_net import DEFAULT_SIM_PORT

    if bind != "127.0.0.1":
        logger.warning(
            "sim: --bind %s is non-loopback. No authentication is performed; "
            "anyone who can reach this port can drive the simulator.",
            bind,
        )
    server = SimServer(
        host=bind,
        port=sim_port if sim_port is not None else DEFAULT_SIM_PORT,
        serial=ctx.obj["sim_serial"],
        fs_root=ctx.obj["sim_dir"],
        display_size=tuple(ctx.obj["sim_size"]),
    )
    click.echo(f"sim listening on {server.host}:{server.port}", err=True)

    if headless:
        import signal
        import threading

        stop = threading.Event()
        signal.signal(signal.SIGINT, lambda *_: stop.set())
        signal.signal(signal.SIGTERM, lambda *_: stop.set())
        try:
            stop.wait()
        finally:
            server.close()
        return

    try:
        from PySide6 import QtCore, QtWidgets

        from .sim.window import SimWindow
    except ImportError as e:
        logger.error(
            "PySide6 is required for the simulator GUI window "
            "(install with `pip install 'touchy-pad[sim]'`), or pass "
            "--headless to skip the window: %s",
            e,
        )
        server.close()
        sys.exit(2)

    import signal

    app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
    window = SimWindow(server.device, size=ctx.obj["sim_size"])
    window.show()
    signal.signal(signal.SIGINT, lambda *_: app.quit())
    _tick = QtCore.QTimer()
    _tick.start(200)
    _tick.timeout.connect(lambda: None)
    try:
        app.exec()
    finally:
        server.close()


@cli.command("board-info")
def board_info() -> None:
    """Print board name, protocol version, and firmware version."""
    from rich.console import Console
    from rich.table import Table

    with _client() as c:
        v = c.sys_board_info_get()
    table = Table(show_header=False, box=None)
    table.add_row("board", v.board_name or "(unknown)")
    table.add_row("protocol", str(v.protocol_version))
    table.add_row("firmware", f"{v.firmware_version} ({v.firmware_version_str})")
    Console().print(table)


@cli.command("update")
@click.option(
    "--board",
    "board",
    default=None,
    metavar="BOARD",
    help="Board name to flash (e.g. waveshare_s3_lcd_7b). "
    "Required when no running Touchy device is reachable.",
)
@click.option(
    "--version",
    "version",
    default="latest",
    show_default=True,
    metavar="VERSION",
    help="GitHub release tag to fetch (e.g. v0.2.0) or 'latest'.",
)
@click.pass_context
def update_cmd(ctx: click.Context, board: str | None, version: str) -> None:
    """Download a firmware release from GitHub and flash it over USB."""
    from . import update as _update

    _update.run_update(
        board=board,
        version=version,
        port=ctx.obj.get("port"),
        client_factory=_client,
    )


@cli.command("file-reset")
def file_reset() -> None:
    """Delete every file the host has uploaded to the device's flash."""
    with _client() as c:
        c.file_delete("F:host")


@cli.command("file-delete")
@click.argument("path")
def file_delete(path: str) -> None:
    """Delete a single file (or subtree) at PATH from the device."""
    with _client() as c:
        c.file_delete(path)


@cli.command("file-save")
@click.argument("path")
@click.argument("file", type=click.File("rb"))
def file_save(path: str, file) -> None:
    """Upload a single file to the device under PATH.

    PATH must be drive-prefixed (e.g. ``F:host/images/avatar.png`` for
    persistent flash or ``R:host/images/avatar.bin`` for transient
    PSRAM storage).
    """
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
    SRCDIR and uploads every file at its relative path under
    ``F:host/<rel>``.
    """
    with _client() as c:
        c.file_delete("F:host")
        for p in sorted(srcdir.rglob("*")):
            if not p.is_file():
                continue
            rel = p.relative_to(srcdir).as_posix()
            c.file_save(f"F:host/{rel}", p.read_bytes())
            logger.info("sent %s (%d bytes)", rel, p.stat().st_size)


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
                logger.info(
                    "event code=%d host_code=0x%x widget=%r%s",
                    evt.code,
                    evt.host_code,
                    evt.user_data,
                    f" {state_str}" if state_str else "",
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
@click.argument("path")
def screen_load(path: str) -> None:
    """Activate the screen at PATH (drive-prefixed, e.g. F:host/screens/home.pb)."""
    with _client() as c:
        c.screen_load(path)


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
    from .api.screens import build_demo

    screen, widgets = build_demo()

    if as_json:
        from google.protobuf import json_format

        click.echo(f"// screen: {screen.name}")
        click.echo(json_format.MessageToJson(screen.to_proto(), indent=2))
        for name, w in widgets:
            click.echo(f"// widget: {name}")
            click.echo(json_format.MessageToJson(w, indent=2))
        return

    smiley = make_smiley_png()

    with _open_pad() as pad:
        if pad.board_info is not None:
            info = pad.board_info
            logger.info(
                "board %s  firmware %s  protocol %s",
                info.board_name or "(unknown)",
                info.firmware_version_str or str(info.firmware_version),
                str(info.protocol_version),
            )
        pad.file_save("F:host/images/smiley.png", smiley)
        logger.info("sent F:host/images/smiley.png (%d bytes source)", len(smiley))
        for name, w in widgets:
            pad.widget_save(name, w)
            logger.info("sent F:host/w/%s.pb", name)
        pad.screen_save(screen)
        logger.info("sent F:host/screens/%s.pb (%d widgets)", screen.name, len(screen.widgets))
        pad.screen_load(f"F:host/screens/{screen.name}.pb")
        logger.info("loaded screen %r", screen.name)

        if listen:

            def on_ping(evt):
                logger.info("[ping]   widget=%r", evt.user_data)

            def on_level(evt):
                logger.info("[slider] widget=%r value=%s", evt.user_data, evt.value)

            def on_enable(evt):
                logger.info("[check]  widget=%r on=%s", evt.user_data, evt.checked)

            def on_smile(evt):
                logger.info("[smile]  widget=%r", evt.user_data)

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
                logger.info(
                    "listening for host events; close the sim window or press Ctrl-C to stop."
                )
            else:
                logger.info("listening for host events (Ctrl-C to stop)...")
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
    from rich.logging import RichHandler

    logging.basicConfig(
        level=logging.INFO,
        format="%(message)s",
        datefmt="[%X]",
        handlers=[RichHandler(show_path=False)],
    )
    try:
        cli()
    except TouchyError as e:
        logger.error("device error: %s", e)
        sys.exit(1)


if __name__ == "__main__":
    main()
