# Touchy-Pad task runner. Run `just` to list recipes, `just <name>` to run one.
#
# https://just.systems/

# Default recipe: show the list of available targets.
default:
    @just --list

# Where generated proto outputs land.
#  - Python output is dropped directly into the host package so `poetry build`
#    picks it up automatically.
#  - C output is dropped next to touchy.proto so the firmware's CMake glue
#    can find it via a fixed relative path. Both are gitignored.
py_proto_dst := "app/src/touchy_pad/_proto"
c_proto_dst  := "proto"

# The system Python (NOT the ESP-IDF venv). The ESP-IDF activate script
# overrides `python3` on PATH inside this devcontainer, but the host-side
# protobuf and nanopb packages were pip-installed to /usr/bin/python3.
#
# Override by setting SYS_PYTHON in the environment, e.g.:
#   SYS_PYTHON=python3 just build-proto-py
# CI sets this to `python3` so it uses the actions/setup-python interpreter
# where grpcio-tools and nanopb are installed.
sys_python := env("SYS_PYTHON", "/usr/bin/python3")

# ---------------------------------------------------------------------------
# Proto generation
#
# Recipes use shell-level mtime checks so they're effectively no-ops when
# touchy.proto / touchy.options haven't changed since the last build —
# `just` itself doesn't track file dependencies the way Make does, so we
# emulate it with `[ src -nt dst ]`.
# ---------------------------------------------------------------------------

proto_src    := "proto/touchy.proto"
proto_opts   := "proto/touchy.options"
py_proto_out := py_proto_dst + "/touchy_pb2.py"
c_proto_out  := c_proto_dst  + "/touchy.pb.c"

# Regenerate both Python and C protobuf bindings (if stale).
build-proto: build-proto-py build-proto-c

# Regenerate Python bindings into the host package iff touchy.proto is
# newer than the generated module (or the module doesn't exist yet).
# `poetry build` then bundles them into the wheel/sdist so PyPI users don't
# need protoc.
build-proto-py:
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -f "{{py_proto_out}}" ] \
       && [ ! "{{proto_src}}" -nt "{{py_proto_out}}" ]; then
        echo "build-proto-py: {{py_proto_out}} is up to date"
        exit 0
    fi
    mkdir -p {{py_proto_dst}}
    {{sys_python}} -m grpc_tools.protoc \
        -Iproto \
        --python_out={{py_proto_dst}} \
        {{proto_src}}
    echo "wrote {{py_proto_out}}"

# Regenerate the embedded C bindings via nanopb iff the proto or its
# nanopb options file is newer than the generated .pb.c.
build-proto-c:
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -f "{{c_proto_out}}" ] \
       && [ ! "{{proto_src}}"  -nt "{{c_proto_out}}" ] \
       && [ ! "{{proto_opts}}" -nt "{{c_proto_out}}" ]; then
        echo "build-proto-c:  {{c_proto_out}} is up to date"
        exit 0
    fi
    # nanopb's generator wants to run from a directory that contains the
    # .proto file so its --proto_path defaults line up.
    cd {{c_proto_dst}} && {{sys_python}} -m nanopb.generator.nanopb_generator \
        --output-dir=. \
        touchy.proto
    echo "wrote {{c_proto_out}}"

# ---------------------------------------------------------------------------
# Host app (app/) — Poetry-driven, but the proto module is a build-time
# dependency so every recipe depends on build-proto-py to keep the package
# self-consistent.
# ---------------------------------------------------------------------------

# Install dependencies into the Poetry-managed venv.
app-install:
    cd app && poetry install --no-interaction

# Run the test suite. Ensures the generated proto module is present first.
app-test: build-proto-py
    cd app && poetry run pytest

# Run the linter.
app-lint: build-proto-py
    cd app && poetry run ruff check src tests

# Build wheel + sdist into app/dist/. Regenerates proto first so the
# wheel always contains an up-to-date touchy_pb2.py.
app-build: build-proto-py
    cd app && poetry build

# Run the touchy CLI inside the Poetry venv. Forward extra args:
#   just app-run -- version
app-run *ARGS: build-proto-py
    cd app && poetry run touchy {{ARGS}}

show-screen: build-proto-py
    cd app && poetry run touchy writefiles ../ui

# ---------------------------------------------------------------------------
# Aggregate convenience targets
# ---------------------------------------------------------------------------

# Lint + test everything (currently just the host app).
test: app-lint app-test

# Remove generated artifacts. The proto outputs are rebuilt on the next
# `just build-proto*` invocation.
clean:
    rm -f {{py_proto_dst}}/touchy_pb2.py {{py_proto_dst}}/touchy_pb2.pyi
    rm -f {{c_proto_dst}}/touchy.pb.c {{c_proto_dst}}/touchy.pb.h
    rm -rf app/dist app/build app/src/*.egg-info

