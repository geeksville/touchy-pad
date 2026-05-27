"""CLI entry point for `streamdeck-probe`."""
from __future__ import annotations

import logging
import sys
import time
from pathlib import Path

import click

from .log import ProbeLogger
from .probe import probe_deck


@click.command()
@click.option(
    "--out-dir",
    type=click.Path(file_okay=False, dir_okay=True, path_type=Path),
    default=Path("logs"),
    show_default=True,
    help="Where to write probe-<timestamp>.{jsonl,txt}.",
)
@click.option(
    "--no-interactive",
    is_flag=True,
    help="Skip the key-press prompt phase. Useful for CI or quick smoke tests.",
)
@click.option(
    "--brightness-pause",
    "brightness_pause_ms",
    type=int,
    default=300,
    show_default=True,
    help="Milliseconds to sleep between brightness changes.",
)
@click.option(
    "--press-timeout",
    type=float,
    default=60.0,
    show_default=True,
    help="Maximum seconds to wait for the press-test phase.",
)
@click.option(
    "--sim",
    is_flag=True,
    help=(
        "Spin up the in-process touchy-pad sim and install the "
        "touchydeck DeviceManager monkey-patch before enumeration. "
        "Used to validate that the StreamDeck-compat layer is wired "
        "correctly end-to-end without real hardware. By default this "
        "also opens a PySide6 SimWindow so you can SEE the screen the "
        "probe is driving; pass --sim-headless to suppress it."
    ),
)
@click.option(
    "--sim-headless",
    is_flag=True,
    help="With --sim: do not open the PySide6 SimWindow (CI / smoke tests).",
)
@click.option(
    "--sim-size",
    type=(int, int),
    default=(480, 300),
    show_default=True,
    help=(
        "With --sim: simulated display panel size in pixels (W H). Also "
        "drives the SimWindow size in GUI mode, and \u2014 because TouchyDeck "
        "lays out as many native 72x72 px keys as fit \u2014 determines the "
        "advertised StreamDeck grid dimensions."
    ),
)
def main(
    out_dir: Path,
    no_interactive: bool,
    brightness_pause_ms: int,
    press_timeout: float,
    sim: bool,
    sim_headless: bool,
    sim_size: tuple[int, int],
) -> None:
    """Enumerate any attached StreamDeck devices and record their behavior."""
    # Enable DEBUG logging for touchy_pad modules so we can see
    # file_save / screen_load / etc. operations during the probe run.
    # These logs go to stderr alongside the probe's own text output.
    logging.basicConfig(
        level=logging.WARNING,
        format="[%(levelname)s] %(name)s: %(message)s",
        stream=sys.stderr,
    )
    logging.getLogger("touchy_pad").setLevel(logging.DEBUG)
    # event_consume is polled at ~20 Hz; suppress its per-call trace to
    # avoid flooding stderr. Set to DEBUG to re-enable.
    logging.getLogger("touchy_pad.client.rpc").setLevel(logging.INFO)

    out_dir.mkdir(parents=True, exist_ok=True)
    ts = time.strftime("%Y%m%d-%H%M%S")
    log = ProbeLogger(out_dir / f"probe-{ts}.jsonl", out_dir / f"probe-{ts}.txt")

    log.log(
        "info",
        "start",
        data={
            "python": sys.version.split()[0],
            "interactive": not no_interactive,
            "brightness_pause_ms": brightness_pause_ms,
            "press_timeout_s": press_timeout,
            "sim": sim,
        },
    )

    # Always route enumeration through the touchy-pad facade. The
    # `touchy_pad.touchydeck` package wraps `streamcontroller-streamdeck`
    # by monkey-patching `DeviceManager.enumerate` to also yield
    # TouchyDecks alongside any real StreamDecks — exactly what
    # StreamController-style apps see at runtime. Probing through this
    # path means the probe exercises the same surface our users do.
    try:
        from touchy_pad.touchydeck import install as touchydeck_install
    except ImportError as e:
        log.log("error", "import_failed", data={"error": str(e)})
        log.close()
        raise click.ClickException(
            "touchy-pad not installed (with the [streamdeck] extra). "
            "Run `poetry install`."
        ) from e

    sim_gui = sim and not sim_headless
    sim_app = None
    sim_window = None
    if sim:
        try:
            from touchy_pad.api import create_sim_device
        except ImportError as e:
            log.log("error", "sim_import_failed", data={"error": str(e)})
            log.close()
            raise click.ClickException(
                "--sim requires the touchy-pad [sim] extra. Run `poetry install`."
            ) from e

        # `headless=False` is advisory at the registry level; the Qt
        # window itself has to be built by the host app (the registry
        # owns the transport, not the UI). We do that below for GUI
        # mode.
        sim_transport = create_sim_device(
            headless=not sim_gui,
            display_size=tuple(sim_size),
        )
        log.log(
            "info",
            "sim_ready",
            data={"serial": sim_transport.serial, "gui": sim_gui},
        )

        if sim_gui:
            try:
                from PySide6 import QtWidgets

                from touchy_pad.sim.window import SimWindow
            except ImportError as e:
                log.log("error", "sim_gui_import_failed", data={"error": str(e)})
                log.close()
                raise click.ClickException(
                    "--sim GUI mode requires PySide6 (touchy-pad [sim] extra). "
                    "Pass --sim-headless to skip the window."
                ) from e

            sim_app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
            sim_window = SimWindow(sim_transport.device, size=tuple(sim_size))
            sim_window.show()
            log.log("info", "sim_window_shown", data={"size": list(sim_size)})

    try:
        touchydeck_install()
    except RuntimeError as e:
        log.log("error", "touchydeck_install_failed", data={"error": str(e)})
        log.close()
        raise click.ClickException(str(e)) from e

    from StreamDeck.DeviceManager import DeviceManager  # type: ignore

    try:
        decks = DeviceManager().enumerate()
    except Exception as e:  # noqa: BLE001
        log.log("error", "enumerate_failed", data={"error_type": type(e).__name__, "error": str(e)})
        log.close()
        raise click.ClickException(f"DeviceManager().enumerate() failed: {e}") from e

    # Record per-deck class so the log makes the real-vs-Touchy split
    # obvious without grepping for `Touchy-Pad` in deck_type strings.
    log.log(
        "info",
        "enumerated",
        data={
            "deck_count": len(decks),
            "deck_classes": [type(d).__name__ for d in decks],
        },
    )

    if not decks:
        click.echo("No StreamDecks found. Plug one in and re-run.", err=True)
        log.close()
        sys.exit(2)

    def _run_all_probes() -> None:
        for idx, deck in enumerate(decks):
            try:
                probe_deck(
                    deck,
                    log,
                    idx,
                    interactive=not no_interactive,
                    brightness_pause_ms=brightness_pause_ms,
                    press_timeout=press_timeout,
                )
            except Exception as e:  # noqa: BLE001
                log.log(
                    "error",
                    "probe_aborted",
                    deck_index=idx,
                    data={"error_type": type(e).__name__, "error": str(e)},
                )
                click.echo(f"deck {idx} aborted: {e}", err=True)

    if sim_gui and sim_app is not None:
        # In GUI mode the probe has to run off the Qt main thread or
        # the window won't paint while the probe is running. We spawn
        # one worker thread, kick off the Qt event loop on main, and
        # quit the loop when the probe finishes — except in interactive
        # mode, where we leave the window open until the user closes
        # it so they can poke at the simulated screen.
        import threading

        done = threading.Event()

        def _worker() -> None:
            try:
                _run_all_probes()
            finally:
                done.set()
                if not no_interactive:
                    click.echo(
                        "\nProbe finished. Close the SimWindow to exit.",
                        err=True,
                    )
                else:
                    # Headless-press mode: auto-quit when done.
                    sim_app.quit()

        worker = threading.Thread(target=_worker, name="probe-worker", daemon=True)
        worker.start()

        # Ctrl+C in the terminal cleanly closes the Qt loop.
        import signal

        from PySide6 import QtCore

        signal.signal(signal.SIGINT, lambda *_: sim_app.quit())
        _tick = QtCore.QTimer()
        _tick.start(200)
        _tick.timeout.connect(lambda: None)

        sim_app.exec()
        # If the user closed the window mid-probe, wait briefly for
        # the worker to notice (the next blocking RPC will fail) so we
        # don't tear down the sim transport from under it.
        worker.join(timeout=2.0)
    else:
        _run_all_probes()

    log.log("info", "done", data={"jsonl": str(log.jsonl_path), "text": str(log.text_path)})
    click.echo(f"\nLogs written to:\n  {log.jsonl_path}\n  {log.text_path}", err=True)
    log.close()

    # Belt-and-braces against libusb teardown crashing the interpreter after we
    # already wrote logs. Drop all deck references, force GC, and pause briefly
    # so any internal threads have time to fully exit.
    import gc
    del decks
    gc.collect()
    time.sleep(0.2)


if __name__ == "__main__":  # pragma: no cover
    main()
