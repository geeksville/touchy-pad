# Project Brief — touchy-pad

| | |
|---|---|
| **Repo** | https://github.com/geeksville/touchy-pad (remote `origin`) |
| **Branch** | `main` @ `40a133e` (as of 2026-09-28) |
| **Version** | `VERSION` = `0.3.4` / build `21` — single source of truth for Python **and** CMake |
| **License** | GPL-3.0-or-later |
| **Maintainer** | @geeksville, with community board contributions (@ckirmse, @chmouel) |

## One-liner

An open-source "cheap yellow display" firmware that turns $15–30 ESP32-S3 /
ESP32-P4 / classic-ESP32 boards into a **multitouch USB touchpad**, a
**StreamDeck-style macro keypad**, and a **remotely-driven display** — plus the
host-side tooling (Python package `touchy-pad`, Rust crates, an OpenDeck plugin,
a StreamController shim and a device simulator) that drives it.

## Scope (what this project is)

1. **Firmware** (`firmware/`, ESP-IDF C++/LVGL) for many boards. Renders
   *host-authored* LVGL screens; emits USB-HID mouse/keyboard and its own
   protobuf RPC protocol over USB vendor bulk, USB-CDC, UART or
   WiFi HTTP(S).
2. **Host library** (`app/`, Python 3.11+) — `touchy` CLI, `touchy_pad.api`
   high-level API, screen/widget DSL, content-addressed image cache, mTLS
   provisioning, Tkinter/PySide6 simulator.
3. **Rust workspace** (`rust/`) — async `touchy-pad` client crate, `touchy-demo`
   CLI, `touchy-opendeck` OpenDeck device plugin.
4. **Integrations** — `TouchyDeck` (StreamDeck shim for StreamController),
   `tools/streamdeck-probe` (reverse-engineering tool),
   `tools/StreamController` (git submodule, branch `pr-touchypad`).
5. **Docs** (`docs/`) — `docs/design.md` is the authoritative stage history.

## Core requirements / design goals

(from `docs/design.md` §"General design goals", updated to today's reality)

* Build with **ESP-IDF/CMake** (originally PlatformIO — since replaced), **not**
  Arduino.
* **LVGL** for all rendering (layers + widgets); no direct framebuffer writes.
* **C++** for the device, with classes for new subsystems; new subsystems get
  their own `.cpp/.h` pair in `firmware/main/`; long-running work gets its own
  FreeRTOS task.
* Push almost all "smarts" to the host: the device is deliberately
  *layout-agnostic* and just materialises whatever protobuf widget tree the host
  sends. `main.cpp` stays thin.
* One wire protocol for every transport (USB, UART, TCP sim, HTTP) — a single
  framing + a single dispatcher per side.
* Cheap commodity hardware only ($15–30 boards, no soldering); one-click
  install via `pip install touchy-pad && touchy update`.

## Non-goals / out of scope

* No cloud service, no account, no always-on companion process required for
  basic use (HID behaviour works device-side with no host app).
* Firmware has **no unit-test suite** — host-side pytest only (`app/tests/`).
* No BLE, no e-ink, no stylus/paintbrush, no full multitouch-HID digitizer
  (trackpad gestures are exposed through protocol `Action`s + HID mouse, not a
  multitouch HID report).
* Simulator is not a cycle-accurate emulator — it implements the protocol and
  an LVGL-ish layout engine, not the real panel timings.

## Definition of done for a change

1. Implemented in firmware and/or host with matching proto changes
   (`proto/*.proto` + `.options` for nanopb caps).
2. `just build-proto` regenerates Python **and** C bindings (generated files are
   gitignored — never hand-edit).
3. `just app-test` green (241 passed / 2 skipped at HEAD; see `progress.md`).
   `just rust-test` / `just rust-lint` for Rust changes.
4. `just firmware-build` green for at least the default board (`jc4827w543`);
   board/chip switches go through `just firmware-reconfigure [board]`.
5. Wire-format versions bumped where the schema changed (see
   `systemPatterns.md` → "Wire-format versioning").
6. `docs/design.md` gets a new `## Stage NN` section written *and* `AGENTS.md`
   updated when a stage lands. `VERSION` is bumped only by `just bump-version`.

## Sources of truth

| Question | File |
|---|---|
| What stage are we on / why was it built this way | `docs/design.md` (newest stage wins) |
| Agent-facing orientation (boards, gotchas, conventions) | `AGENTS.md` (= `CLAUDE.md` symlink) |
| Wire protocol + filesystem paths | `docs/host-api.md`, `proto/*.proto` |
| Network/HTTP(S) API | `docs/network-api.md` |
| Python API | `docs/python-api.md`, `app/src/touchy_pad/api/__init__.py` |
| Rust API | `docs/rust-api.md`, `rust/touchy-pad/src/lib.rs` |
| Build/test commands | `Justfile` (766 lines) |
| Version | `VERSION` |
| Known bugs | `docs/open-issues.md` |
| Remaining work | `docs/TODO.md` |

## Workflow rules (non-negotiable)

* **Never auto-commit or push.** Make edits, let the user commit.
* Prefer `just <recipe>` over raw `idf.py` / `poetry` / `protoc`.
* **Never run bare `idf.py`** in an agent shell — ESP-IDF is not sourced there.
* `docs/design.md` is updated when a stage completes.
