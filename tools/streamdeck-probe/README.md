# streamdeck-probe

A reverse-engineering tool that drives StreamDeck devices through the
**touchy-pad** facade (`touchy_pad.touchydeck`) — which wraps the upstream
[`streamcontroller-streamdeck`](https://pypi.org/project/streamcontroller-streamdeck/)
library and additionally exposes our own TouchyDecks via the same
`DeviceManager().enumerate()` surface. Every observable behavior is recorded
to disk.

The captured logs feed into Touchy-Pad **Stage 50.2** (TouchyDeck: a Touchy-Pad
device that emulates the StreamDeck API so apps like StreamController can drive
our cheap hardware).

This is a deliberately separate Poetry project — it pulls in the real StreamDeck
library (transitively, via `touchy-pad[streamdeck]`), which we don't want as a
direct dependency of the main `touchy-pad` package.

## What it does

For every StreamDeck returned by `DeviceManager().enumerate()`, the probe runs a
scripted sequence and logs every observation:

1. Device introspection (`deck_type`, `id`, serial, firmware, key layout,
   `key_image_format`, etc.).
2. `open()` → `reset()`.
3. `set_brightness(0/30/100)`.
4. `set_key_image(k, None)` for every key, followed by `set_key_image(k, tile)`
   with a runtime-generated test image. Each call's timing and exceptions are
   captured.
5. `set_key_color`, `set_screen_image`, `set_touchscreen_image` where supported.
6. Interactive: prompts the operator to press each key. **While any key is
   held down the probe immediately uploads a 180°-rotated (upside-down)
   version of that key's tile** via `set_key_image`, then restores the normal
   tile on release. This stress-tests the press→image-update→release round-trip
   latency and verifies that `Button.on_release` host events are delivered. On
   exit from this phase all keys are restored to their normal tiles.
7. `close()`.

## Install & run

The library needs a HIDAPI system shared library at runtime. On Debian/Ubuntu:

```bash
sudo apt install libhidapi-libusb0
```

(macOS: `brew install hidapi`. Windows: bundled with the Python `hidapi` wheel.)

Then:

```bash
cd tools/streamdeck-probe
poetry install
poetry run streamdeck-probe
```

Logs land in `logs/probe-<timestamp>.jsonl` (machine-readable) and
`logs/probe-<timestamp>.txt` (human-readable). Both are `.gitignored`.

After a successful run, copy ONE pair into `samples/` and commit them. That
captured run is the source of truth used while implementing Stage 50.2.

```bash
cp logs/probe-<ts>.jsonl samples/
cp logs/probe-<ts>.txt   samples/
git add samples/probe-*.jsonl samples/probe-*.txt
```

## Options

```
streamdeck-probe --help
```

| Flag                    | Default | Notes                                             |
|-------------------------|---------|---------------------------------------------------|
| `--out-dir DIR`         | `logs/` | Where to write the `probe-<ts>.{jsonl,txt}` pair. |
| `--no-interactive`      | off     | Skip the press-key prompt; non-interactive runs.  |
| `--brightness-pause MS` | `300`   | Delay between brightness changes (lets you see).  |
| `--press-timeout SEC`   | `60`    | Max time to wait for the press-test phase.        |
| `--sim`                 | off     | Spin up the in-process Touchy-Pad simulator and probe it as a `TouchyDeck`. |
| `--sim-headless`        | off     | With `--sim`: skip the PySide6 SimWindow (CI / smoke tests). |
| `--sim-size W H`        | `480 300` | With `--sim`: simulated panel size in pixels. TouchyDeck packs as many native 72×72 px keys as fit, so this also picks the advertised StreamDeck grid (e.g. `480 272` → 6×3, `1024 600` → 13×7). |

## Output schema (`*.jsonl`)

Every line is a JSON object with at minimum:

```json
{"ts": 1716345600.123, "phase": "info", "deck_index": 0, "event": "key_image_format", "data": {...}}
```

`phase` is one of: `info`, `open`, `brightness`, `set_key_image`, `set_key_color`,
`set_screen_image`, `set_touchscreen_image`, `callback`, `close`, `error`.

The text log is a prettified rendering of the same events. The JSONL is the
authoritative artifact.
