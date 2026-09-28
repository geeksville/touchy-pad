# Tech Context — tooling, setup, constraints

## Technology stack

| Area | Technology |
|---|---|
| Firmware | C++17, **ESP-IDF v6.0.2** (CMake component build, `idf.py`), FreeRTOS, TinyUSB, LVGL **v9** via `espressif/esp_lvgl_port` |
| Serialisation | **nanopb** (device, C) + **protobuf** (`grpcio-tools` / `protoc`, host Python) + **prost** (Rust) — all generated from `proto/` |
| Python host | Python **3.11–3.14**, Poetry, `click` (CLI), `rich`, `requests`, `pyusb`+libusb, `pyserial`, `Pillow`, `esptool`, `cryptography`, `streamcontroller-streamdeck`; optional `PySide6` (sim GUI) |
| Python dev | `ruff` (format + lint), `pytest` (+`pytest-qt` for sim-window tests), `pre-commit` (ruff hooks on commit/push), `mkdocs` (`just build-docs`) |
| Rust | Rust **1.85+**, edition **2024**, cargo workspace: `prost`, `nusb` (+tokio), `tokio`, `thiserror`, `anyhow`, `log`/`env_logger`, `image`, `clap`, `futures`, `async-trait` |
| Simulator | Tkinter/PySide6 (`PySide6` extra), in-process or TCP (`--sim-remote`), plaintext HTTP server on port **8083** |
| Build/run glue | **`just`** (766-line `Justfile`) — the only sanctioned entry point |
| CI | `.github/workflows/app-ci.yml` — `build-app` on ubuntu / windows / macos |
| Dev environment | VS Code devcontainer (`.devcontainer/`) pre-installing ESP-IDF, nanopb, protoc, Poetry, just |

Firmware managed components (added under `firmware/main/idf_component.yml` and
per-board `idf_component.yml` files): `espressif/esp_lvgl_port` (**pinned
`>=2.8.0,<2.9.0`**), `espressif/esp_lcd_ili9341`, `atanisoft/esp_lcd_touch_xpt2046`,
`espressif/cjson` (IDF 6 moved cJSON out of core — REQUIRES name
`espressif__cjson`).

## Dev setup

```sh
just init             # one-time: poetry install (+ sim extra), git hooks, shell completions,
                      # and `just firmware-reconfigure` for the default board
just build-proto      # regenerate Python + C bindings (+ default screen json)
```

Manual prerequisites: Python ≥3.10, Poetry, `just`, and a working ESP-IDF
install (`. $HOME/esp/esp-idf/export.sh`) for firmware builds. On Linux the user
must be able to open the device (udev rule in `bin/99-touchy-pad.rules`,
installed by `bin/install-rules.sh`; docs in `docs/udev.md`).

## Standard commands

```sh
just app-test         # build-proto-py then `poetry run pytest`  (PRIMARY host test)
just app-lint         # ruff format + ruff check --fix
just app-build        # wheel + sdist into app/dist/
just app-run -- ...   # any `touchy` CLI subcommand inside Poetry
just rust-build       # cargo build --workspace (syncs version first)
just rust-test        # cargo test --workspace
just rust-lint / rust-fmt / rust-doc
just firmware-build   # build-proto-c + default screen, then `idf.py -C firmware build`
just firmware-reconfigure [board]   # reads boards/<board>/target, set-target + reconfigure
just flash            # build + flash (UART port for the board)
just merge-bin / flash-merged       # merged single image at 0x0 (used by `touchy update`)
just build-all        # firmware + app + rust
just test             # app-lint + app-test
just test-interactive # manual/author-side checks
just clean
```

**Never run bare `idf.py`** — the ESP-IDF environment isn't sourced in an agent
shell, so `just firmware-build` / `just firmware-reconfigure` (which source
`~/.espressif/tools/activate_idf_v6.0.2.sh` or `~/esp/esp-idf/export.sh`) are the
only reliable way. Same for `poetry`/`protoc` — use the `just` recipes.

## Verified environment state (2026-09-28, this workspace)

* `just app-test` → **241 passed, 2 skipped** (skips are
  `test_touchydeck.py` cases gated on `register_controllers_factory` not being
  available) in ~20 s. The suite runs fine in the devcontainer without hardware.
* `git status` at the time of writing: clean tree, only `.clinerules/` untracked.
* Firmware was **not** rebuilt during this session (no board attached); board
  builds are expected to work via `just firmware-build` but were not re-verified
  here.

## Technical constraints & hard-won gotchas

**Firmware / ESP-IDF**

* `espressif/esp_lvgl_port` is pinned `>=2.8.0,<2.9.0`. 2.9.0 moved the
  MIPI-DSI avoid-tearing callback to `on_frame_buf_complete` behind an
  `IDF >= 5.5` check, but IDF 6.0.x only declares `on_refresh_done` on
  `esp_lcd_dpi_panel_event_callbacks_t`, so the esp32p4 (`elecrow_p4_lcd_7`)
  build dies in `esp_lvgl_port_disp.c:lvgl_port_add_disp_dsi`. Lift the pin only
  once the project's IDF declares the renamed field.
* In `sdkconfig.defaults`, `# CONFIG_X is not set` is **not a comment** — it is
  the `X=n` directive and silently overrides an earlier `CONFIG_X=y`.
* Switching board/chip needs `rm -f firmware/sdkconfig firmware/sdkconfig.<board>`
  before `just firmware-reconfigure <board>` (CMakeCache pins the board).
* USB capability keys off `CONFIG_SOC_USB_OTG_SUPPORTED`; classic-ESP32 boards
  (CYD) have **no** USB-OTG, so no HID and no CDC — the protocol rides UART0 at
  115200. `firmware/main/CMakeLists.txt` REQUIRES `esp_driver_uart` (IDF v6
  split out `driver/uart.h`).
* ESP32-S3 USB-OTG exposes only 5 IN endpoints → no dedicated event mailbox;
  events are polled and the mailbox interrupt endpoint (0x85) only signals
  "events available".
* No PSRAM on CYD boards (~520 KB SRAM): `RamFs` prefers `MALLOC_CAP_SPIRAM` but
  falls back to internal RAM; `T:` resolves to a flash scratch area when
  `heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0`.
* nanopb uses `FT_POINTER` (heap) for `repeated` widget/action/step fields and
  the `FileWrite` payload; caps for other message/field limits live in
  `proto/*.options` (e.g. `Display.chains max_count:1`,
  `PanelChain.panels max_count:4`, both `FT_STATIC`).
* LVGL keeps *pointers* to styles → styles must be heap-allocated and freed from
  `LV_EVENT_DELETE`; LIFO/direct setters matter for animation invalidation
  (see the slider-artifact issue in `docs/open-issues.md`).
* jc4827w543's NV3041A QSPI tops out at **32 MHz** on that board (datasheet says
  ~40 MHz); above it, pixels corrupt. `reset_gpio_num` is `gpio_num_t` in IDF v6
  — assign the enum directly.
* Board components link `Display` from `main` via the IDF component group; every
  board × all three chips is expected to build.

**Python host**

* **Windows has no libusb.** Any path touching `usb.core.find()` must guard
  `NoBackendError` (not just `ImportError`) — see `api/device.py` and
  `touchydeck/discovery.py`.
* Generated proto bindings (`_proto/touchy_pb2.py`, `widgets_pb2.py`,
  `preferences_pb2.py`) and the embedded default screen are **not checked in**;
  every `app-*` recipe depends on `build-proto-py` / `default_screen.json`.
* `TOUCHY_URL` / `--url` selects the HTTP(S) transport; `https://` auto-loads the
  mTLS certs saved under `~/.config/touchy-pad/mtls/<host>/`.
* Public API = `touchy_pad.api` only; `touchy_pad._proto`, `sim/`, `cli.py` are
  internal.
* `touchy_pad.touchydeck.install()` must be called explicitly (no import-time
  monkey-patching).

**Justfile gotchas (learned the hard way)**

* Recipe bodies use `#!/usr/bin/env bash` and must use **relative paths** —
  `justfile_directory()` yields `D:\a\...` on Windows and bash eats the `\a`.
* Use `${SYS_PYTHON:-/usr/bin/python3}` inside recipes (runtime), **not**
  `{{sys_python}}` (parse-time expansion).
* macOS BSD `paste` needs the explicit `-` stdin marker (`paste -sd: -`).
* `just init`'s Poetry 2.x quirk: path deps in
  `[tool.poetry.dependencies]` are ignored when a `[project]` table exists, so
  `init` force-installs the editable `touchy-pad` link with `pip -e`.

## Tool usage patterns (what to reach for)

| Task | Do this |
|---|---|
| Anything host-side | `just app-test` / `just app-lint` / `just app-run -- <cmd>` |
| Proto change | edit `proto/*.proto` (+ `*.options`), then `just build-proto` |
| Firmware change | `just firmware-build` (default board `jc4827w543`); `just firmware-reconfigure <board>` to switch |
| Rust change | `just rust-test`, `just rust-lint`, `just rust-fmt` |
| Try a screen without hardware | `just app-run -- --sim-gui ...` or `just app-run -- --sim-headless` |
| Flash a board | `just flash` (UART) or end-user `touchy update` (merged image) |
| Regenerate the built-in fallback screen | `just gen-default-screen` (it *imports* `touchy_pad.api.screens`) |
| Release | `just bump-version` (bumps `VERSION`, commits, tags, pushes) |

## External references worth knowing

* LVGL v9 docs (widgets, layers, styles, animations, `lv_obj_property`),
  `lv_example_imagebutton_1` / `lv_example_style_11` for style-animation
  semantics.
* ESP-IDF v6 docs (USB device stack/TinyUSB, `esp_https_server`, `esp_lvgl_port`,
  Oct/Quad SPI panel drivers).
* OpenDeck plugin docs (`docs/opendeck-device-plugin.md`), StreamController
  shim (`tools/StreamController`, submodule branch `pr-touchypad`).
* `tools/streamdeck-probe` — the Stage 50.1 reverse-engineering tool used to
  mirror real StreamDeck behaviour.
