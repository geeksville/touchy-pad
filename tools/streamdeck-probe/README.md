# streamdeck-probe

A reverse-engineering tool that drives a real Elgato StreamDeck through the
[`streamcontroller-streamdeck`](https://pypi.org/project/streamcontroller-streamdeck/)
library and records every observable behavior to disk. The captured logs feed
into Touchy-Pad **Stage 50.2** (TouchyDeck: a Touchy-Pad device that emulates
the StreamDeck API so apps like StreamController can drive our cheap hardware).

This is a deliberately separate Poetry project — it pulls in the real StreamDeck
library, which we don't want as a dependency of the main `touchy-pad` package.

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
6. Interactive: prompts the operator to press each key once, then press-and-hold
   one key. Every `set_key_callback` invocation is logged with timestamp and
   state — this resolves the press-only-vs-press+release question we have for
   our own `host_action` widget.
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

## Output schema (`*.jsonl`)

Every line is a JSON object with at minimum:

```json
{"ts": 1716345600.123, "phase": "info", "deck_index": 0, "event": "key_image_format", "data": {...}}
```

`phase` is one of: `info`, `open`, `brightness`, `set_key_image`, `set_key_color`,
`set_screen_image`, `set_touchscreen_image`, `callback`, `close`, `error`.

The text log is a prettified rendering of the same events. The JSONL is the
authoritative artifact.
