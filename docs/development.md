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

## Day-to-day recipes

```sh
# from the repo root
just app-install          # re-run poetry install (after pyproject.toml changes)
just app-test             # regenerate proto bindings, then run pytest
just app-lint             # ruff check src tests
just app-build            # build wheel + sdist into app/dist/
just app-run -- version   # poetry run touchy version (or any other subcommand)
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
