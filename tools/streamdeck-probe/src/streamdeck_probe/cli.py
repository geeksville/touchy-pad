"""CLI entry point for `streamdeck-probe`."""
from __future__ import annotations

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
def main(
    out_dir: Path,
    no_interactive: bool,
    brightness_pause_ms: int,
    press_timeout: float,
) -> None:
    """Enumerate any attached StreamDeck devices and record their behavior."""
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
        },
    )

    try:
        from StreamDeck.DeviceManager import DeviceManager  # type: ignore
    except ImportError as e:
        log.log("error", "import_failed", data={"error": str(e)})
        log.close()
        raise click.ClickException(
            "streamcontroller-streamdeck not installed. Run `poetry install`."
        ) from e

    try:
        decks = DeviceManager().enumerate()
    except Exception as e:  # noqa: BLE001
        log.log("error", "enumerate_failed", data={"error_type": type(e).__name__, "error": str(e)})
        log.close()
        raise click.ClickException(f"DeviceManager().enumerate() failed: {e}") from e

    log.log("info", "enumerated", data={"deck_count": len(decks)})

    if not decks:
        click.echo("No StreamDecks found. Plug one in and re-run.", err=True)
        log.close()
        sys.exit(2)

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

    log.log("info", "done", data={"jsonl": str(log.jsonl_path), "text": str(log.text_path)})
    click.echo(f"\nLogs written to:\n  {log.jsonl_path}\n  {log.text_path}", err=True)
    log.close()


if __name__ == "__main__":  # pragma: no cover
    main()
