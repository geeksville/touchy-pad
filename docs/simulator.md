# Linux device simulator

The `touchy-pad` Python package ships an in-process **device simulator**
so you can develop screens, exercise the host API, and run the test
suite on a Linux box with no hardware attached. The simulator implements
the same protobuf protocol as the firmware, so anything that talks to a
real Touchy-Pad over USB also talks to the sim.

Two flavours are available:

| Mode               | Flag             | Needs Qt? | Use case                                     |
|--------------------|------------------|-----------|----------------------------------------------|
| **GUI**            | `--sim`          | yes       | See the screens render; click widgets by hand. |
| **Headless**       | `--sim-headless` | no        | CI, scripting, tests, container builds.      |

Both flavours run entirely in your shell — no flashing, no USB.

## Install

The GUI window pulls in PySide6 (Qt 6), so it lives in an optional
extra:

```bash
# Headless only (no Qt, ~20 MB):
pip install touchy-pad

# GUI window + headless (adds PySide6):
pip install 'touchy-pad[sim]'
```

On Debian / Ubuntu you'll also need a handful of native libraries that
PySide6 dlopens at startup. They're already installed in the project's
devcontainer (see [.devcontainer/Containerfile](../.devcontainer/Containerfile));
elsewhere:

```bash
sudo apt-get install -y \
    libglib2.0-0 libgl1 libegl1 libfontconfig1 \
    libdbus-1-3 libxkbcommon0 libnss3
```

## Quick start — GUI window

```bash
touchy --sim sim
```

This:

1. Spins up a `SimDevice` backed by a per-serial pseudo-filesystem
   under `platformdirs.user_cache_dir('touchy-pad')/SIM0000/`.
2. Loads the lexicographically first uploaded screen (or the embedded
   default if the fs is empty).
3. Opens a Qt window with the device canvas on the left and an event
   log on the right.

If the cache is empty, push the bundled demo screens first:

```bash
touchy --sim screen demo   # uploads F:host/images/smiley.png + F:host/screens/{home,test}.pb
touchy --sim sim           # now the window has something to render
```

### Home screen — the demo trackpad page

![Sim showing the home screen with a Sim Trackpad placeholder and Prev / Next buttons](images/sim-home.png)

The shaded "Sim Trackpad" area is a placeholder; the simulator
intentionally doesn't emulate multitouch. The `< Prev` and `Next >`
buttons live on the **top LVGL layer** and dispatch
`ActionSwitchScreen{NEXT/PREVIOUS}` actions exactly like they would on
hardware.

### Test screen — interactive widgets

![Sim showing the test screen after clicking widgets; event log on the right](images/sim-test.png)

Click `Ping host`, drag the slider, toggle `Enabled`, or press the
smiley `image_button` to see how each widget action surfaces. The log
panel timestamps every event:

* `switch: → 'test'` — `ActionSwitchScreen` ran on the device.
* `host: code=0x100 widget='ping'` — `ActionHost` queued an `LvEvent`
  for any connected client.
* `macro: widget=... steps=N (not emulated)` — `ActionMacro` is logged
  but **not** replayed; the sim doesn't pretend to be a HID device.

`Force` toggles the firmware's force-render debug flag (display-only in
the sim). The green `60 fps` readout is a static placeholder for the
on-device FPS widget.

## Headless mode

Drop the window and drive the sim purely from your CLI / scripts:

```bash
# Any subcommand transparently uses the sim instead of looking for USB:
touchy --sim-headless version
touchy --sim-headless screen demo
touchy --sim-headless screen list
touchy --sim-headless file ls
```

`--sim-headless` is what the test suite uses. It implies `--sim` and
never imports PySide6, so it's safe in minimal containers.

## CLI flags

All `--sim*` flags belong to the top-level `touchy` command, so they go
**before** any subcommand:

| Flag                | Default        | Meaning                                                 |
|---------------------|----------------|---------------------------------------------------------|
| `--sim`             | off            | Route every subcommand through the in-process sim.      |
| `--sim-headless`    | off            | Same as `--sim` but never opens a GUI window.           |
| `--sim-size WxH`    | `480x300`      | Canvas size for the GUI window (matches your hardware). |
| `--sim-serial STR`  | `SIM0000`      | Pseudo-USB serial. Picks the per-device cache dir.      |
| `--sim-dir PATH`    | platformdirs   | Override the pseudo-fs root (e.g. for tests).           |

Example: a 800×480 sim that lives in a scratch directory:

```bash
touchy \
    --sim --sim-size 800x480 \
    --sim-serial DEV1 --sim-dir /tmp/touchy-sim \
    sim
```

## Programmatic use

Skip the CLI entirely if you're writing host-side software:

```python
from touchy_pad import TouchyClient
from touchy_pad.sim.transport import make_tempdir_transport

with make_tempdir_transport() as transport:
    client = TouchyClient(transport)
    print(client.sys_version_get().firmware_version_str)   # "sim"
    client.screen_load("F:host/screens/home.pb")
```

`make_tempdir_transport()` returns a fully-functional
[`SimDeviceTransport`](../app/src/touchy_pad/sim/transport.py) backed by
a throwaway tempdir, so unit tests don't need to think about cache
locations. See [`tests/test_sim_transport.py`](../app/tests/test_sim_transport.py)
and [`tests/test_sim_window.py`](../app/tests/test_sim_window.py) for
worked examples.

## What's emulated, what isn't

| Feature                          | Sim behaviour                                    |
|----------------------------------|--------------------------------------------------|
| `Command` / `Response` protocol  | Full — same protobuf wire format as firmware.    |
| Screen storage (`F:host/screens/*.pb`) | Sandbox-rooted pseudo-fs; survives across runs.  |
| `R:` PSRAM filesystem              | Mirrored to the same pseudo-fs under an `R/` subdir; behaves as transient (the sim doesn't enforce loss-on-reboot, but treat it that way for parity with hardware). |
| Image asset upload               | Stored as-is; **no `.png → .bin` conversion**.   |
| Widget rendering                 | Buttons, labels, sliders, checkboxes, toggles, images, image-buttons, FPS, log, force-render checkbox. |
| Trackpad widget                  | Static "Sim Trackpad" placeholder — no touches.  |
| LVGL layer ordering              | Bottom → active → top → sys, transparent overlay.|
| `ActionSwitchScreen`             | Loads / cycles through registered screens.       |
| `ActionHost`                     | Pushes `LvEvent` onto the event queue.           |
| `ActionMacro`                    | Logged only; no HID replay.                      |
| Haptics, USB HID                 | Not emulated.                                    |

## Running the test suite

The sim window tests use Qt's `offscreen` platform plugin so they run
without a display:

```bash
cd app
QT_QPA_PLATFORM=offscreen pytest
```

When `PySide6` isn't installed, `tests/test_sim_window.py` skips
automatically via `pytest.importorskip`.
