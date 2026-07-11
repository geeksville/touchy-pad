"""Command-line interface for the Touchy-Pad device.

Run ``touchy --help`` for a list of subcommands.
"""

from __future__ import annotations

import logging
import sys
from pathlib import Path

import click

from .api import TouchyClient, TouchyError
from .api._transport import DeviceNotFoundError, Transport

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
    "115200 baud instead of auto-discovering it by USB VID/PID. "
    "Auto-discovery already covers CH340-attached CYD boards, so most "
    "users won't need this. Also used by esptool-based commands such "
    "as `update`.",
)
@click.option(
    "--listen",
    is_flag=True,
    help="After the subcommand finishes, stream device logs and host events "
    "until Ctrl-C. Works with any subcommand.",
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
    listen: bool,
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
    logging.getLogger("touchy_pad.api.client.rpc").setLevel(logging.INFO)

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
    ctx.obj["listen"] = listen

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
        from .api._transport_net import DEFAULT_SIM_PORT, TcpTransport, parse_sim_url

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
    if ctx.obj.get("listen") and not ctx.obj.get("sim_gui"):
        logger.info("streaming device logs and events (Ctrl-C to stop)\u2026")
        conn = ctx.obj.pop("live_conn", None)
        if conn is None:
            # Bare `--listen` with no subcommand that opened a
            # connection — open one now.
            _stream_events(_client())
            return
        # Reuse the exact connection the subcommand already opened
        # instead of closing it and re-opening (re-enumerating the USB
        # device in the same process transiently fails right after a
        # heavy upload — see _keep_alive_for_listen). `conn` carries a
        # stashed `_real_close` because its own `close()` was no-op'd so
        # the subcommand's `with` block wouldn't tear it down.
        from .api.device import Touchy

        real_close = getattr(conn, "_real_close", conn.close)
        if isinstance(conn, Touchy):
            # A Touchy already has a background thread streaming device
            # logs (→ `touchy_pad.device` logger) and dispatching host
            # events. Just park the main thread until interrupted.
            import time

            try:
                while True:
                    time.sleep(0.2)
            except KeyboardInterrupt:
                pass
            finally:
                real_close()
            return
        # A plain TouchyClient (no background thread): drive the event
        # stream from the main thread.
        try:
            for evt in conn.stream_events():
                logger.info("host-event code=0x%x", evt.host_code)
        except KeyboardInterrupt:
            pass
        finally:
            real_close()
        return

    if not ctx.obj.get("sim_gui"):
        return
    app = ctx.obj["sim_app"]

    if ctx.obj.get("listen"):
        logger.info("listening for host events; close the sim window or press Ctrl-C to stop.")

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
        from .api._transponrt_serial import SerialTransport

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
        return _keep_alive_for_listen(TouchyClient(t))
    try:
        return _keep_alive_for_listen(TouchyClient.open())
    except DeviceNotFoundError as e:
        logger.error("%s", e)
        sys.exit(2)


def _keep_alive_for_listen(conn):
    """Stash a freshly-opened connection so the ``--listen`` phase reuses it.

    When the top-level ``--listen`` flag is set, the subcommand's
    ``with _client()/_open_pad()`` block would normally close the USB
    transport on the way out — and the post-subcommand listen phase
    would then have to re-open it. Re-enumerating the same device inside
    one process right after a heavy upload transiently fails (libusb
    keeps returning "not found" even though the device never rebooted),
    whereas a brand-new process connects fine.

    So instead of closing and re-opening, we keep the *same* connection
    alive: stash it on the click context and neutralise its ``close()``
    (preserving the real one as ``_real_close``) so the subcommand's
    ``with`` block leaves it open for :func:`_after_subcommand`.
    """
    ctx = click.get_current_context(silent=True)
    if ctx is None or not ctx.obj or not ctx.obj.get("listen"):
        return conn
    ctx.obj["live_conn"] = conn
    conn._real_close = conn.close
    conn.close = lambda: None  # type: ignore[method-assign]
    return conn


def _stream_events(client: TouchyClient) -> None:
    """Park the main thread streaming device logs + host events."""
    try:
        for evt in client.stream_events():
            logger.info("host-event code=0x%x", evt.host_code)
    except KeyboardInterrupt:
        pass
    finally:
        getattr(client, "_real_close", client.close)()


def _open_pad():
    """Counterpart to :func:`_client` that goes through the high-level API."""
    from .api import touchy_open

    return _keep_alive_for_listen(touchy_open(transport=_make_transport()))


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
    from .api._transport_net import DEFAULT_SIM_PORT
    from .sim.server import SimServer

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


def _fmt_bytes(n: int) -> str:
    """Render a byte count as a compact human-readable string."""
    value = float(n)
    for unit in ("B", "KiB", "MiB", "GiB"):
        if value < 1024.0 or unit == "GiB":
            return f"{int(value)} {unit}" if unit == "B" else f"{value:.1f} {unit}"
        value /= 1024.0
    return f"{n} B"  # unreachable


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
    table.add_row("display", f"{v.display_width}x{v.display_height}")
    table.add_row("multitouch", "yes" if v.is_multitouch else "no")
    table.add_row("usb", "yes" if v.has_usb else "no")
    table.add_row("touchable", "yes" if v.is_touchable else "no")
    table.add_row("free RAM", _fmt_bytes(v.free_heap_bytes))
    table.add_row("free PSRAM", _fmt_bytes(v.free_psram_bytes))
    table.add_row("flash FS", f"{_fmt_bytes(v.fs_used_bytes)} / {_fmt_bytes(v.fs_total_bytes)}")
    table.add_row("temp drive", "flash" if v.temp_is_flash else "psram")
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
# Pref — persistent device preferences (Stage 82)
# ---------------------------------------------------------------------------


@cli.group()
def pref() -> None:
    """Set persistent device preferences (backlight, logging, boot delay)."""


@pref.command("backlight-timeout")
@click.argument("seconds", type=float)
def pref_backlight_timeout(seconds: float) -> None:
    """Auto-sleep the backlight after SECONDS of no input (0 disables)."""
    with _client() as c:
        c.screen_sleep_timeout(round(seconds * 1000))


@pref.command("log-level")
@click.argument(
    "level",
    type=click.Choice(["TRACE", "DEBUG", "INFO", "WARN", "ERROR"], case_sensitive=False),
)
def pref_log_level(level: str) -> None:
    """Set the minimum device log priority queued for the host.

    Lines below LEVEL are dropped device-side and never tunneled over the
    protocol. Persists across reboots (default ERROR).
    """
    from . import _proto

    value = _proto.LogPriority.Value(level.upper())
    with _client() as c:
        c.set_min_log_level(value)


@pref.command("boot-delay")
@click.argument("seconds", type=int)
def pref_boot_delay(seconds: int) -> None:
    """Sleep SECONDS early in boot so a debug-log connection can attach.

    Persists across reboots (0 disables). Applied at the next boot.
    """
    with _client() as c:
        c.set_boot_delay(seconds)


@pref.command("backlight-level")
@click.argument("level", type=click.IntRange(0, 100))
def pref_backlight_level(level: int) -> None:
    """Set the display brightness (0 = off … 100 = max). Persists."""
    with _client() as c:
        c.set_backlight_level(level)


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


@screen.command("load")
@click.argument("path")
def screen_load(path: str) -> None:
    """Activate the screen at PATH (drive-prefixed, e.g. F:host/s/home.pb)."""
    with _client() as c:
        c.screen_load(path)


_TOUCHY_IMG_PATH = "F:host/images/touchy.png"
_USER_BG_IMG_PATH = "F:host/images/user-background.bin"


def _do_write_trackpad(pad, background_image: str) -> None:
    """Upload the trackpad page body using *background_image* as the backdrop.

    Writes ``F:host/uscr/trackpad.pb`` referencing *background_image* (a
    device-side drive-prefixed path that must already exist on the device).
    Does **not** upload the image file itself — the caller is responsible for
    that.  Does **not** reload any screen.
    """
    from .pages import trackpad as _trackpad_page

    _, trackpad_widget = _trackpad_page.build(background_image=background_image)
    pad.user_screen_save("trackpad", trackpad_widget)
    logger.info("sent F:host/uscr/trackpad.pb (background=%s)", background_image)


def _do_screen_init(pad) -> None:
    """Write the default chrome screen + baseline trackpad page.

    Uploads ``F:host/s/default.pb`` (the persistent prev/next chrome with
    a ``widget_ref(id="page")`` body), the Touchy-Pad logo image, and the
    baseline ``trackpad`` page into ``F:host/uscr/`` so the device has a
    usable layout out of the box. Shared by ``touchy init`` and ``screen demo``.

    Touch-less boards (display-less LED matrices, ``board_info.is_touchable``
    false) can't use a trackpad, so they instead get the self-contained
    animated setup screen from :func:`build_setup_screen_touchless`.
    """
    from .paths import DEFAULT_SCREEN_PATH

    info = pad.board_info
    if info is not None and not info.is_touchable:
        from .api.screens import build_setup_screen_touchless

        screen = build_setup_screen_touchless(
            width=info.display_width or 32,
            height=info.display_height or 8,
        )
        pad.screen_save(screen)
        logger.info("sent %s (touch-less)", DEFAULT_SCREEN_PATH)
        pad.screen_load(DEFAULT_SCREEN_PATH)
        logger.info("loaded %s", DEFAULT_SCREEN_PATH)
    else:
        from .api.images import make_touchy_png
        from .api.screens import build_default_screen

        pad.screen_save(build_default_screen())
        logger.info("sent %s", DEFAULT_SCREEN_PATH)

        touchy_png = make_touchy_png()
        pad.file_save(_TOUCHY_IMG_PATH, touchy_png, max_width=128, max_height=128)
        logger.info("sent %s (%d bytes source)", _TOUCHY_IMG_PATH, len(touchy_png))

        _do_write_trackpad(pad, _TOUCHY_IMG_PATH)

        pad.screen_load(DEFAULT_SCREEN_PATH)
        logger.info("loaded %s", DEFAULT_SCREEN_PATH)


@screen.command("demo")
@click.option(
    "--json",
    "as_json",
    is_flag=True,
    help="Print the screen definition as protobuf JSON to stdout; " "do not talk to the device.",
)
@click.pass_context
def screens_demo(ctx: click.Context, as_json: bool) -> None:
    """Upload the sample demo on top of the default chrome.

    Runs ``touchy init`` first (writing ``F:host/s/default.pb`` plus the
    baseline ``F:host/uscr/trackpad.pb``), then adds the widget-showcase
    ``test`` page into ``F:host/uscr/`` and the 16x16 smiley image asset.

    The default chrome's persistent ``[< Prev | Next >]`` row pages its
    body ``widget_ref`` through ``F:host/uscr/``; flip to the ``test``
    page on-device with ``Next >``.

    Pass the top-level ``--listen`` flag to stream host events after
    uploading: ``touchy --listen screen demo``.
    """
    from .api.images import make_smiley_png
    from .api.screens import build_default_screen, build_user_pages
    from .paths import DEFAULT_SCREEN_PATH

    if as_json:
        import tempfile

        from google.protobuf import json_format

        tmpdir = Path(tempfile.mkdtemp(prefix="touchy-screens-"))
        screen_msg = build_default_screen()
        p = tmpdir / f"{screen_msg.name}.json"
        p.write_text(json_format.MessageToJson(screen_msg.to_proto(), indent=2))
        click.echo(str(p))
        for name, w in build_user_pages():
            p = tmpdir / f"{name}.json"
            p.write_text(json_format.MessageToJson(w, indent=2))
            click.echo(str(p))
        return

    smiley = make_smiley_png()
    pages = dict(build_user_pages())

    with _open_pad() as pad:
        if pad.board_info is not None:
            info = pad.board_info
            logger.info(
                "board %s  firmware %s  protocol %s",
                info.board_name or "(unknown)",
                info.firmware_version_str or str(info.firmware_version),
                str(info.protocol_version),
            )
        # Baseline: default chrome + trackpad page (and load default).
        _do_screen_init(pad)

        # Demo extras: the smiley asset and the showcase `test` page.
        pad.file_save("F:host/images/smiley.png", smiley)
        logger.info("sent F:host/images/smiley.png (%d bytes source)", len(smiley))
        pad.user_screen_save("test", pages["test"])
        logger.info("sent F:host/uscr/test.pb")
        pad.screen_load(DEFAULT_SCREEN_PATH)
        logger.info("loaded %s", DEFAULT_SCREEN_PATH)
        pad.show_user_screen("test")
        logger.info("showing F:host/uscr/test.pb")


@cli.command("reboot-bootloader")
def reboot_bootloader() -> None:
    """Reboot the device into its USB DFU bootloader."""
    with _client() as c:
        c.sys_reboot_bootloader()


@cli.command("init")
def init() -> None:
    """Write the default chrome screen and baseline trackpad page.

    Creates ``F:host/s/default.pb`` (the persistent ``[< Prev | Next >]``
    chrome with a ``widget_ref(id="page")`` body) and
    ``F:host/uscr/trackpad.pb`` (the baseline multitouch trackpad page),
    then loads the default screen. Run this once to give a freshly-wiped
    device a usable layout; afterwards push your own pages into
    ``F:host/uscr/`` with the Python API's
    :meth:`~touchy_pad.api.Touchy.user_screen_save`.
    """
    with _open_pad() as pad:
        if pad.board_info is not None:
            info = pad.board_info
            logger.info(
                "board %s  firmware %s  protocol %s",
                info.board_name or "(unknown)",
                info.firmware_version_str or str(info.firmware_version),
                str(info.protocol_version),
            )
        _do_screen_init(pad)


# ---------------------------------------------------------------------------
# Touchpad — trackpad-specific management
# ---------------------------------------------------------------------------


@cli.group()
def touchpad() -> None:
    """Manage the touchpad settings."""


@touchpad.command("image")
@click.argument("url")
def touchpad_image(url: str) -> None:
    """Set a custom background image for the trackpad page.

    Fetches the image at URL, scales it to fit within 180×180 px (preserving
    aspect ratio), converts it to LVGL's native format, and saves it to
    ``F:host/images/user-background.bin`` on the device.  The trackpad page
    (``F:host/uscr/trackpad.pb``) is then regenerated to reference the new
    image.  Reload the default screen afterwards to see the change.

    URL must be an http or https address pointing at a supported image
    (PNG, JPEG, BMP, GIF, or WebP).
    """
    import urllib.request

    if not url.lower().startswith(("http://", "https://")):
        raise click.BadParameter("URL must start with http:// or https://", param_hint="URL")

    logger.info("fetching %s …", url)
    # cloudflare blocks python by default
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "Mozilla/5.0 (compatible; touchy-pad/1.0)"},
    )
    with urllib.request.urlopen(req, timeout=30) as resp:  # noqa: S310 – scheme validated above
        image_data = resp.read()
    logger.info("fetched %d bytes", len(image_data))

    from .api.lvgl_image import is_gif

    # GIFs keep their extension (the firmware's lv_gif decoder is selected
    # by the `.gif` suffix); everything else converts to LVGL `.bin`.
    bg_path = "F:host/images/user-background.gif" if is_gif(image_data) else _USER_BG_IMG_PATH

    with _open_pad() as pad:
        pad.file_save(bg_path, image_data, max_width=180, max_height=180)
        logger.info("sent %s", bg_path)
        _do_write_trackpad(pad, bg_path)


@touchpad.command("gif")
def touchpad_gif() -> None:
    """Set the bundled animated cat GIF as the trackpad background.

    Uploads the packaged ``touchy_pad/assets/cat-space.gif`` (scaled to fit
    within 180×180 px) and regenerates the trackpad page to reference it,
    just like ``touchpad image URL`` but with no network fetch.
    """
    from importlib import resources

    image_data = resources.files("touchy_pad.assets").joinpath("cat-space.gif").read_bytes()
    logger.info("loaded bundled cat-space.gif (%d bytes)", len(image_data))

    bg_path = "F:host/images/user-background.gif"
    with _open_pad() as pad:
        pad.file_save(bg_path, image_data, max_width=180, max_height=180)
        logger.info("sent %s", bg_path)
        _do_write_trackpad(pad, bg_path)


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
