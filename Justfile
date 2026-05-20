# Touchy-Pad task runner. Run `just` to list recipes, `just <name>` to run one.
#
# https://just.systems/

# Default recipe: show the list of available targets.
default:
    @just --list

# Where generated proto outputs land.
#  - Python output is dropped directly into the host package so `poetry build`
#    picks it up automatically.
#  - C output goes into firmware/main/proto/ so it lives inside the component
#    tree; CMake finds it via a local relative path. Both are gitignored.
py_proto_dst := "app/src/touchy_pad/_proto"
c_proto_dst  := "firmware/main/proto"

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

touchy_proto     := "proto/touchy.proto"
touchy_opts      := "proto/touchy.options"
widgets_proto    := "proto/widgets.proto"
widgets_opts     := "proto/widgets.options"
py_touchy_out    := py_proto_dst + "/touchy_pb2.py"
py_widgets_out   := py_proto_dst + "/widgets_pb2.py"
c_touchy_out     := c_proto_dst  + "/touchy.pb.c"
c_widgets_out    := c_proto_dst  + "/widgets.pb.c"

# Regenerate both Python and C protobuf bindings (if stale).
build-proto: build-proto-py build-proto-c build-default-screen

# Regenerate Python bindings into the host package iff either .proto is
# newer than its generated module (or the module doesn't exist yet).
# `poetry build` then bundles them into the wheel/sdist so PyPI users don't
# need protoc.
build-proto-py:
    #!/usr/bin/env bash
    set -euo pipefail
    touchy_stale=0
    widgets_stale=0
    [ ! -f "{{py_touchy_out}}"  ] && touchy_stale=1
    [ ! -f "{{py_widgets_out}}" ] && widgets_stale=1
    [ "{{touchy_proto}}"  -nt "{{py_touchy_out}}"  ] && touchy_stale=1  || true
    [ "{{widgets_proto}}" -nt "{{py_widgets_out}}" ] && widgets_stale=1 || true
    if [ $touchy_stale -eq 0 ] && [ $widgets_stale -eq 0 ]; then
        echo "build-proto-py: up to date"
        exit 0
    fi
    mkdir -p {{py_proto_dst}}
    {{sys_python}} -m grpc_tools.protoc \
        -Iproto \
        --python_out={{py_proto_dst}} \
        {{touchy_proto}} {{widgets_proto}}
    echo "wrote {{py_touchy_out}} {{py_widgets_out}}"

# Regenerate the embedded C bindings via nanopb iff any proto or options
# file is newer than the generated .pb.c files.
build-proto-c:
    #!/usr/bin/env bash
    set -euo pipefail
    touchy_stale=0
    widgets_stale=0
    [ ! -f "{{c_touchy_out}}"  ] && touchy_stale=1
    [ ! -f "{{c_widgets_out}}" ] && widgets_stale=1
    [ "{{touchy_proto}}"  -nt "{{c_touchy_out}}"  ] && touchy_stale=1  || true
    [ "{{touchy_opts}}"   -nt "{{c_touchy_out}}"  ] && touchy_stale=1  || true
    [ "{{widgets_proto}}" -nt "{{c_widgets_out}}" ] && widgets_stale=1 || true
    [ "{{widgets_opts}}"  -nt "{{c_widgets_out}}" ] && widgets_stale=1 || true
    if [ $touchy_stale -eq 0 ] && [ $widgets_stale -eq 0 ]; then
        echo "build-proto-c:  up to date"
        exit 0
    fi
    # nanopb's generator wants to run from a directory that contains the
    # .proto file so its --proto_path defaults line up.
    mkdir -p {{c_proto_dst}}
    cd proto && {{sys_python}} -m nanopb.generator.nanopb_generator \
        --output-dir=../{{c_proto_dst}} \
        touchy.proto widgets.proto
    echo "wrote {{c_touchy_out}} {{c_widgets_out}}"

# Compile proto/default_screen.json (the firmware's built-in fallback
# screen, shown when no host-uploaded screens are present) into a C++
# header carrying its serialised protobuf bytes. Depends on
# build-proto-py because the embed script needs touchy_pb2.
default_screen_json := "proto/default_screen.json"
default_screen_out  := "firmware/main/default_screen_pb.h"
build-default-screen: build-proto-py
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -f "{{default_screen_out}}" ] \
        && [ "{{default_screen_out}}" -nt "{{default_screen_json}}" ] \
        && [ "{{default_screen_out}}" -nt "proto/embed_screen_json.py" ] \
        && [ "{{default_screen_out}}" -nt "{{py_touchy_out}}" ] \
        && [ "{{default_screen_out}}" -nt "{{py_widgets_out}}" ]; then
        echo "build-default-screen: up to date"
        exit 0
    fi
    {{sys_python}} proto/embed_screen_json.py \
        {{default_screen_json}} {{default_screen_out}} default_screen_pb

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

# ---------------------------------------------------------------------------
# Firmware (firmware/) — ESP-IDF CMake build.
# ---------------------------------------------------------------------------

# Build the firmware. Regenerates C proto bindings first so the firmware
# always compiles against the latest schema.
firmware-build: build-proto-c build-default-screen
    cmake --build firmware/build

flash: firmware-build
    #!/usr/bin/env bash
    set -euo pipefail
    # Pick the first readable+writable ttyACM* under /host/dev/
    port=""
    for candidate in $(ls /host/dev/ttyACM* 2>/dev/null | sort); do
        if [ -r "$candidate" ] && [ -w "$candidate" ]; then
            port="$candidate"
            break
        fi
    done
    if [ -z "$port" ]; then
        echo "error: no accessible ttyACM* device found under /host/dev/" >&2
        exit 1
    fi
    echo "flashing to $port"
    # Use esptool directly from the ESP-IDF venv — avoids sourcing export.sh,
    # which breaks inside an already-activated Python environment.
    # cd into build so the relative binary paths in flash_args resolve.
    esptool_py="esptool"
    cd firmware/build
    mapfile -t flash_args < flash_args
    # flash_args has two lines: flags line, then addr:file pairs per line.
    # Flatten them into a single array of words.
    args=()
    for line in "${flash_args[@]}"; do
        read -ra words <<< "$line"
        args+=("${words[@]}")
    done
    "$esptool_py" -p "$port" -b 460800 \
        --before default-reset --after hard-reset \
        --chip esp32s3 write-flash "${args[@]}"


# ---------------------------------------------------------------------------
# Aggregate convenience targets
# ---------------------------------------------------------------------------

# Build firmware + Python wheel (proto bindings regenerated as needed).
build-all: firmware-build app-build

test-interactive: 
    cd app && poetry run touchy screens demo --listen

# Lint + test everything (currently just the host app).
test: app-lint app-test

# Remove generated artifacts. The proto outputs are rebuilt on the next
# `just build-proto*` invocation.
clean:
    rm -f {{py_proto_dst}}/touchy_pb2.py {{py_proto_dst}}/touchy_pb2.pyi
    rm -f {{py_proto_dst}}/widgets_pb2.py {{py_proto_dst}}/widgets_pb2.pyi
    rm -rf {{c_proto_dst}}
    rm -f firmware/main/default_screen_pb.h
    rm -rf app/dist app/build app/src/*.egg-info
