# Developer setup

## Getting the source

```sh
git clone https://github.com/geeksville/touchy-pad
cd touchy-pad
```

## Option A — Dev container (recommended)

The repo ships a VS Code dev container (`.devcontainer/`) that pre-installs
all firmware and host toolchains (ESP-IDF, nanopb, protoc, Poetry, just, …).
Open the folder in VS Code and choose **Reopen in Container** when prompted,
then run:

```sh
just init
```

This installs Python dependencies via Poetry and registers `pre-commit` and
`pre-push` git hooks that enforce linting before any push.

## Option B — Manual setup

Prerequisites: **Python ≥ 3.10**, [**Poetry**](https://python-poetry.org/),
and [**just**](https://just.systems/) installed on your host.

```sh
# Install Python deps + register git hooks
just init

# Generate protobuf bindings (Python + C)
just build-proto
```

For the C/firmware build you also need a working ESP-IDF installation; see
[firmware/README.md](../firmware/README.md) for details.

### If `just firmware-build` says `idf.py: command not found`

That almost always means the ESP-IDF **Python venv** is incomplete, not that
`idf.py` is missing: `export.sh` reports the real error first, e.g.

```
error: Unable to import the rich module: No module named 'rich'. Please execute the install script.
ERROR: Activation script failed
```

(left over from an interrupted `install.sh`, or a stale CI cache). Repair it with
the idempotent install script for the architectures this repo builds
(`esp32` for the CYD boards, `esp32s3` for most, `esp32p4` for
`elecrow_p4_lcd_7`):

```sh
( cd ~/esp/esp-idf && ./install.sh esp32,esp32s3,esp32p4 )
```

CI does this automatically: `.github/workflows/build-firmware.yml` validates the
cached toolchain with `idf.py --version` and re-runs `install.sh` when that
fails, instead of trusting `export.sh`'s presence.

## Day-to-day recipes

```sh
# from the repo root
just app-install          # re-run poetry install (after pyproject.toml changes)
just app-test             # regenerate proto bindings, then run pytest
just app-lint             # ruff check src tests
just app-build            # build wheel + sdist into app/dist/
just app-run -- board-info   # poetry run touchy board-info (or any other subcommand)
just build-proto          # regenerate Python + C protobuf bindings
just firmware-build       # build ESP-IDF firmware
just flash                # build + flash to a connected device
```

The generated protobuf bindings (`touchy_pb2.py`, `widgets_pb2.py`) are
**not** checked in; every `app-*` recipe depends on `build-proto-py`, which
regenerates them from [`proto/`](../proto/).

## Git hooks

`just init` installs two hooks via `pre-commit`:

| Hook | Trigger | What it checks |
|------|---------|----------------|
| `ruff-check` | every commit **and** push | `ruff check src tests` — lint errors block the operation |
| `ruff-format-check` | push only | `ruff format --check` — formatting drift blocks the push |

Run `cd app && poetry run pre-commit run --all-files` at any time to check
the whole tree manually.

## Firmware versioning

The single source of truth is the **`VERSION`** file at the repo root:

```
0.1.0
1
```

Line 1 is the semver base string; line 2 is a monotonically-increasing integer
build number.  Both values are committed to source control.

### How they are embedded in the binary

`firmware/version.cmake` is included by the top-level `firmware/CMakeLists.txt`
before `project()`.  It reads `VERSION`, queries `git status` and
`git rev-parse`, then:

* Sets `PROJECT_VER` (consumed by ESP-IDF → `esp_app_get_description()->version`).
* Writes `firmware/build/version.h` (gitignored) via `configure_file`.

`firmware/main/host_api.cpp` `#include`s that header and serves the constants
over USB in the `SysVersionGetCmd` response:

| Proto field | Source |
|---|---|
| `firmware_version` (uint32) | `FIRMWARE_BUILD_NUMBER` — the build number integer |
| `firmware_version_str` | `FIRMWARE_VERSION_STR` — the full version string |

### Clean vs. dirty builds

| Tree state | `firmware_version_str` example |
|---|---|
| Clean (all changes committed) | `0.1.0` |
| Dirty (uncommitted edits) | `0.1.0.dev47+a1b2c3d` |

The dirty suffix follows PEP 440 local-version conventions:
`dev<N>` where N is the total commit count, `+<hash>` is the short git hash.

CMake re-runs configuration automatically when `VERSION` changes (tracked via a
`configure_file COPYONLY` stamp), so a `just firmware-build` after a version
bump picks up the new values without a manual `idf.py reconfigure`.

### Releasing a new version

```sh
just bump-version
```

This increments the patch component of the semver and the build number, commits
`VERSION`, creates a `vX.Y.Z` git tag, and pushes both the commit and the tag
to origin.  The CI workflow uploads the built `.bin` as a
`firmware-<board>-<version>` artifact on every push (including tagged releases).
