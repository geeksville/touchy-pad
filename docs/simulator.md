# Device simulator

The `touchy-pad` Python package ships a **device simulator** so you can
develop screens, exercise the host API, and run the test suite on a
machine with no hardware attached. The simulator implements the same
nanopb command / response protocol as the firmware. Since Stage 63 the
sim also speaks that protocol over **TCP**, using the exact same
self-synchronising framing the firmware uses on its USB bulk pipes — so
anything that talks to a real Touchy-Pad over USB also talks to the
sim, unchanged, over a socket.

Three flavours, all of which use TCP under the hood:

| Mode                  | Flag                       | Needs Qt? | Use case                                                                 |
|-----------------------|----------------------------|-----------|--------------------------------------------------------------------------|
| **GUI (in-process)**  | `--sim-gui`                | yes       | See screens render *and* drive them from the same CLI invocation.        |
| **Headless (in-process)** | `--sim-headless`       | no        | CI, scripting, tests, container builds.                                  |
| **Remote (TCP)**      | `--sim-remote [HOST:PORT]` | no        | Connect to a separately-launched `touchy simulator`, possibly on another machine / OS. |

Plus one new subcommand:

| Subcommand            | Purpose                                                                                                |
|-----------------------|--------------------------------------------------------------------------------------------------------|
| `touchy simulator`    | Run a standalone sim server that listens on TCP (port 8935 by default). Optional `--headless` for no Qt window. |

The wire format is identical to USB: the Stage 64.3 self-synchronising
frame `MAGIC(0xA5 0x5A) | LEN(u16 LE) | payload | CRC8`, capped at a
65535-byte payload. See [the host-API framing section](host-api.md#wire-framing).

![sim touchpad](images/sim-home.png)
![sim demo](images/sim-test.png)

## Install

The GUI window pulls in PySide6 (Qt 6), so it lives in an optional
extra:

```bash
# Headless or remote only (no Qt, ~20 MB):
pip install touchy-pad

# Adds the GUI window:
pip install 'touchy-pad[sim]'
```

On Debian / Ubuntu, PySide6 dlopens a handful of native libraries that
are already installed in the project's devcontainer
(see [.devcontainer/Containerfile](../.devcontainer/Containerfile));
to install them by hand:

```bash
sudo apt-get install -y \
    libglib2.0-0 libgl1 libegl1 libfontconfig1 \
    libdbus-1-3 libxkbcommon0 libnss3
```

## Quick start — in-process GUI

```bash
touchy --sim-gui screen demo   # upload demo assets to the sim's pseudo-fs
touchy --sim-gui screen load F:host/s/home.pb
```

Each `--sim-gui` invocation spins up an in-process `SimServer` on an
ephemeral loopback port, connects to it over TCP, opens a Qt window
viewing the simulated screen, and runs the subcommand. The exact same
nanopb framing runs through the kernel TCP stack, so the host-side
image pipeline (PNG → LVGL `.bin`) is exercised end-to-end.

## Headless in-process

Drop the window and drive the sim purely from your CLI / scripts:

```bash
touchy --sim-headless board-info
touchy --sim-headless screen demo
touchy --sim-headless screen list
touchy --sim-headless file ls
```

This is what the test suite uses. It never imports PySide6, so it's
safe in minimal containers.

## Standalone sim server (`touchy simulator`)

For long-lived sessions, run the simulator as its own process:

```bash
# In one terminal:
touchy simulator                 # GUI window + TCP listener on 127.0.0.1:8935
touchy simulator --headless      # No window; just listen
touchy simulator --port 9000     # Pick a different port
touchy simulator --bind 0.0.0.0  # Accept non-loopback clients (no auth!)
```

Then drive it from anywhere else:

```bash
touchy --sim-remote                       # 127.0.0.1:8935
touchy --sim-remote 127.0.0.1:9000        # explicit port
touchy --sim-remote host.docker.internal  # cross-container / cross-VM
```

This is the recommended workflow when you want to run the GUI on your
host machine but the host code under test inside a Linux devcontainer
without USB / display passthrough: launch `touchy simulator` on the host
and `--sim-remote host.docker.internal` from inside the container.

> **Security note.** The sim server performs no authentication. Default
> bind is `127.0.0.1` (loopback only). Use `--bind 0.0.0.0` only on
> trusted networks; the CLI prints a warning when you do.

## `TOUCHY_SIM_URL` — global env-var fallback

Any host-side consumer of the touchy-pad Python or Rust API that doesn't
explicitly pass a transport will check the `TOUCHY_SIM_URL` environment
variable before falling back to USB enumeration:

```bash
export TOUCHY_SIM_URL=tcp://127.0.0.1:8935
# Now this connects to the sim instead of looking for USB:
touchy board-info
# As does anything built on touchy_pad.touchy_open() / TouchyClient.open():
python -c "from touchy_pad import touchy_open; print(touchy_open().sys_board_info_get())"
# And the Rust client, transparently:
cargo run -p touchy-demo
```

This is the cleanest way to let third-party plugins
(StreamController, OpenDeck, ad-hoc Python scripts) reach the sim
without code changes. Accepted formats:

| Value                   | Parses as                          |
|-------------------------|------------------------------------|
| `127.0.0.1:8935`        | `("127.0.0.1", 8935)`              |
| `127.0.0.1`             | `("127.0.0.1", 8935)` (default port) |
| `tcp://example.test`    | `("example.test", 8935)`           |
| `tcp://10.0.0.1:1234`   | `("10.0.0.1", 1234)`               |
| `[::1]:9000`            | `("::1", 9000)` (IPv6)             |

Unset / empty / malformed → fall through to USB.

## CLI flags

All `--sim*` flags belong to the top-level `touchy` command, so they go
**before** any subcommand:

| Flag                       | Default        | Meaning                                                                          |
|----------------------------|----------------|----------------------------------------------------------------------------------|
| `--sim-remote [HOST:PORT]` | off (loopback when bare) | Connect to an out-of-process simulator over TCP.                  |
| `--sim-headless`           | off            | Spawn an in-process sim server on an ephemeral loopback port + connect to it. No window. |
| `--sim-gui`                | off            | Same as `--sim-headless` plus open a Qt viewer.                                  |
| `--sim-size WxH`           | `480x300`      | Canvas size for the GUI window (in-process modes only).                          |
| `--sim-serial STR`         | `SIM0000`      | Pseudo-USB serial. Picks the per-device cache dir.                               |
| `--sim-dir PATH`           | platformdirs   | Override the pseudo-fs root (e.g. for tests).                                    |

The three sim-source flags are mutually exclusive; supplying more than
one is an error.

`touchy simulator` accepts:

| Flag           | Default       | Meaning                                                    |
|----------------|---------------|------------------------------------------------------------|
| `--headless`   | off           | Don't open a Qt window; just listen on TCP.                |
| `--bind HOST`  | `127.0.0.1`   | Bind address. `0.0.0.0` accepts non-loopback clients.      |
| `--port N`     | `8935`        | TCP port to listen on.                                     |

`--sim-serial`, `--sim-dir` and `--sim-size` from the root group still
apply to the in-process sim that `touchy simulator` owns.

## Programmatic use

### Python — out-of-process server + client

```python
from touchy_pad.api import TouchyClient
from touchy_pad.sim.server import make_tempdir_server_transport

with make_tempdir_server_transport() as transport:
    client = TouchyClient(transport)
    print(client.sys_board_info_get().board_name)   # "sim"
    client.screen_load("F:host/s/home.pb")
```

`make_tempdir_server_transport()` returns a `SimServerTransport` (a
`TcpTransport` wrapping an in-process `SimServer` bound to an ephemeral
loopback port) and pins its pseudo-fs to a throwaway tempdir, so unit
tests don't need to think about cache locations.

### Python — env-var auto-discovery

```python
import os
os.environ["TOUCHY_SIM_URL"] = "tcp://127.0.0.1:8935"

from touchy_pad import touchy_open
pad = touchy_open()   # connects to the sim, no flags needed
```

### Rust

```rust
use touchy_pad::Touchy;

#[tokio::main]
async fn main() -> touchy_pad::Result<()> {
    // Honours TOUCHY_SIM_URL, then falls back to USB.
    let pad = Touchy::open().await?;
    println!("{}", pad.client().sys_board_info_get().await?.board_name);
    Ok(())
}
```

Or connect explicitly:

```rust
use std::sync::Arc;
use touchy_pad::{Touchy, transport::Transport, transport_net::TcpTransport};

let t = Arc::new(TcpTransport::connect("127.0.0.1", 8935).await?) as Arc<dyn Transport>;
let pad = Touchy::from_transport(t);
```

## What's emulated, what isn't

| Feature                                | Sim behaviour                                                                                                                   |
|----------------------------------------|----------------------------------------------------------------------------------------------------------------------------------|
| `Command` / `Response` protocol        | Full — identical protobuf framing on TCP as the firmware uses on USB bulk.                                                       |
| Screen storage (`F:host/s/*.pb`) | Sandbox-rooted pseudo-fs; survives across runs.                                                                                  |
| `R:` PSRAM filesystem                  | Mirrored under an `R/` subdir; treat as transient for parity with hardware.                                                      |
| Image asset upload                     | PNG/JPEG/etc. are converted to LVGL `.bin` by the host pipeline (same path the real device exercises). The GUI window decodes `.bin` natively. |
| Widget rendering                       | Buttons, labels, sliders, checkboxes, toggles, images, image-buttons, FPS, log, force-render checkbox.                           |
| Trackpad widget                        | Static "Sim Trackpad" placeholder — no touches.                                                                                  |
| LVGL layer ordering                    | Bottom → active → top → sys, transparent overlay.                                                                                |
| `ActionSwitchScreen`                   | Loads / cycles through registered screens.                                                                                       |
| `ActionHost`                           | Pushes `LvEvent` onto the event queue.                                                                                           |
| `ActionMacro`                          | Logged only; no HID replay.                                                                                                      |
| Haptics, USB HID                       | Not emulated.                                                                                                                    |

## Running the test suite

The sim window tests use Qt's `offscreen` platform plugin so they run
without a display:

```bash
just app-test
# or, by hand:
cd app && QT_QPA_PLATFORM=offscreen poetry run pytest
```

When `PySide6` isn't installed, `tests/test_sim_window.py` skips
automatically via `pytest.importorskip`. The Stage 63 network tests in
[`tests/test_transport_net.py`](../app/tests/test_transport_net.py)
cover URL parsing, TCP round-trip framing, busy-client rejection, and
the `TOUCHY_SIM_URL` env-var fallback.

A matching Rust integration test in
[`rust/touchy-pad/tests/sim_tcp.rs`](../rust/touchy-pad/tests/sim_tcp.rs)
spawns `touchy simulator --headless` and drives it from the Rust client
over TCP — skipped automatically when `poetry` isn't on `PATH`.
