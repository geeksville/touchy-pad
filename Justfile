# Touchy-Pad task runner. Run `just` to list recipes, `just <name>` to run one.
#
# https://just.systems/

# Default recipe: show the list of available targets.
default:
    @just --list

# ---------------------------------------------------------------------------
# Developer setup
# ---------------------------------------------------------------------------

# Set up the dev environment: install all Poetry venvs, wire git hooks.
# Run once after cloning or after a devcontainer rebuild — safe to re-run.
# Also invoked automatically by devcontainer.json's postCreateCommand.
init:
    #!/usr/bin/env bash
    set -euo pipefail
    unset VIRTUAL_ENV
    _repo="$(pwd)"
    cd app && poetry install --no-interaction --extras sim
    # streamdeck-probe: Poetry 2.x ignores [tool.poetry.dependencies] path deps
    # when a [project] table is present, so force-install the editable touchy-pad
    # link via pip after the normal poetry install.
    cd "$_repo/tools/streamdeck-probe" && poetry install --no-interaction
    poetry run pip install -q -e "$_repo/app[sim]"
    cd "$_repo/app" && poetry run pre-commit install
    poetry run pre-commit install --hook-type pre-push
    
    # Install shell completions for just command
    echo "Installing just shell completions..."
    mkdir -p ~/.bash_completion.d ~/.zfunc
    just --completions bash > ~/.bash_completion.d/just
    just --completions zsh > ~/.zfunc/_just
    
    # For bash: source the completion in bashrc if not already present
    if [ -f ~/.bashrc ] && ! grep -q 'bash_completion.d/just' ~/.bashrc; then
        echo '[ -f ~/.bash_completion.d/just ] && source ~/.bash_completion.d/just' >> ~/.bashrc
    fi
    
    # For zsh: ensure completion directory is in fpath and compinit is set up
    if [ -f ~/.zshrc ]; then
        if ! grep -q 'fpath=(~/.zfunc' ~/.zshrc; then
            echo 'fpath=(~/.zfunc $fpath)' >> ~/.zshrc
        fi
        if ! grep -q 'autoload -Uz compinit' ~/.zshrc; then
            echo 'autoload -Uz compinit && compinit' >> ~/.zshrc
        fi
    fi

    echo "Setting default firmware build"
    just firmware-reconfigure
    
    echo "✓ Dev environment ready. Run 'just build-proto' to generate bindings."
    echo "✓ Shell completions installed. Restart your shell or run 'exec \$SHELL' to activate."

# Where generated proto outputs land.
#  - Python output is dropped directly into the host package so `poetry build`
#    picks it up automatically.
#  - C output goes into firmware/main/proto/ so it lives inside the component
#    tree; CMake finds it via a local relative path. Both are gitignored.
py_proto_dst := justfile_directory() + "/app/src/touchy_pad/_proto"
c_proto_dst  := justfile_directory() + "/firmware/main/proto"

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

touchy_proto     := justfile_directory() + "/proto/touchy.proto"
touchy_opts      := justfile_directory() + "/proto/touchy.options"
widgets_proto    := justfile_directory() + "/proto/widgets.proto"
widgets_opts     := justfile_directory() + "/proto/widgets.options"
prefs_proto      := justfile_directory() + "/proto/preferences.proto"
prefs_opts       := justfile_directory() + "/proto/preferences.options"
py_touchy_out    := py_proto_dst + "/touchy_pb2.py"
py_widgets_out   := py_proto_dst + "/widgets_pb2.py"
py_prefs_out     := py_proto_dst + "/preferences_pb2.py"
c_touchy_out     := c_proto_dst  + "/touchy.pb.c"
c_widgets_out    := c_proto_dst  + "/widgets.pb.c"
c_prefs_out      := c_proto_dst  + "/preferences.pb.c"

# Regenerate both Python and C protobuf bindings (if stale).
build-proto: build-proto-py build-proto-c build-default-screen

# Regenerate Python bindings into the host package iff either .proto is
# newer than its generated module (or the module doesn't exist yet).
# `poetry build` then bundles them into the wheel/sdist so PyPI users don't
# need protoc.
build-proto-py:
    #!/usr/bin/env bash
    set -euo pipefail
    # Use relative paths — Just always sets cwd to the justfile dir (repo root),
    # so this is safe and avoids Windows backslash-expansion in bash scripts.
    _py="${SYS_PYTHON:-/usr/bin/python3}"
    _py_out="app/src/touchy_pad/_proto"
    touchy_stale=0
    widgets_stale=0
    prefs_stale=0
    [ ! -f "${_py_out}/touchy_pb2.py"      ] && touchy_stale=1
    [ ! -f "${_py_out}/widgets_pb2.py"     ] && widgets_stale=1
    [ ! -f "${_py_out}/preferences_pb2.py" ] && prefs_stale=1
    [ "proto/touchy.proto"      -nt "${_py_out}/touchy_pb2.py"      ] && touchy_stale=1  || true
    [ "proto/widgets.proto"     -nt "${_py_out}/widgets_pb2.py"     ] && widgets_stale=1 || true
    [ "proto/preferences.proto" -nt "${_py_out}/preferences_pb2.py" ] && prefs_stale=1   || true
    if [ $touchy_stale -eq 0 ] && [ $widgets_stale -eq 0 ] && [ $prefs_stale -eq 0 ]; then
        echo "build-proto-py: up to date"
        exit 0
    fi
    mkdir -p "${_py_out}"
    "${_py}" -m grpc_tools.protoc \
        -Iproto \
        --python_out="${_py_out}" \
        proto/touchy.proto proto/widgets.proto proto/preferences.proto
    # protoc emits flat `import widgets_pb2` for cross-file references
    # (Stage 71: touchy.proto imports widgets.proto). Rewrite those to
    # package-relative imports so the committed package imports cleanly
    # without putting `_proto/` on sys.path.
    # proto/fix_proto_imports.py is a standalone script to avoid BSD sed
    # incompatibility (macOS sed treats -i as needing a backup suffix) and
    # Just parser issues with embedded multi-line Python strings.
    "${_py}" proto/fix_proto_imports.py \
        "${_py_out}/touchy_pb2.py" \
        "${_py_out}/widgets_pb2.py" \
        "${_py_out}/preferences_pb2.py"
    echo "wrote ${_py_out}/touchy_pb2.py ${_py_out}/widgets_pb2.py ${_py_out}/preferences_pb2.py"

# Regenerate the embedded C bindings via nanopb iff any proto or options
# file is newer than the generated .pb.c files.
build-proto-c:
    #!/usr/bin/env bash
    set -euo pipefail
    _py="${SYS_PYTHON:-/usr/bin/python3}"
    _c_out="firmware/main/proto"
    touchy_stale=0
    widgets_stale=0
    prefs_stale=0
    [ ! -f "${_c_out}/touchy.pb.c"        ] && touchy_stale=1
    [ ! -f "${_c_out}/widgets.pb.c"       ] && widgets_stale=1
    [ ! -f "${_c_out}/preferences.pb.c"   ] && prefs_stale=1
    [ "proto/touchy.proto"        -nt "${_c_out}/touchy.pb.c"      ] && touchy_stale=1  || true
    [ "proto/touchy.options"      -nt "${_c_out}/touchy.pb.c"      ] && touchy_stale=1  || true
    [ "proto/widgets.proto"       -nt "${_c_out}/widgets.pb.c"     ] && widgets_stale=1 || true
    [ "proto/widgets.options"     -nt "${_c_out}/widgets.pb.c"     ] && widgets_stale=1 || true
    [ "proto/preferences.proto"   -nt "${_c_out}/preferences.pb.c" ] && prefs_stale=1   || true
    [ "proto/preferences.options" -nt "${_c_out}/preferences.pb.c" ] && prefs_stale=1   || true
    if [ $touchy_stale -eq 0 ] && [ $widgets_stale -eq 0 ] && [ $prefs_stale -eq 0 ]; then
        echo "build-proto-c:  up to date"
        exit 0
    fi
    # nanopb's generator wants to run from a directory that contains the
    # .proto file so its --proto_path defaults line up.
    mkdir -p "${_c_out}"
    cd proto && "${_py}" -m nanopb.generator.nanopb_generator \
        --output-dir="../${_c_out}" \
        touchy.proto widgets.proto preferences.proto
    echo "wrote ${_c_out}/touchy.pb.c ${_c_out}/widgets.pb.c ${_c_out}/preferences.pb.c"

# Regenerate proto/default_screen.json from the Python DSL
# (touchy_pad.api.screens.build_default_screen) — the single source of
# truth for the firmware's built-in fallback chrome. Depends on
# build-proto-py because the generator imports the host package (which
# pulls in the generated _proto bindings).
gen-default-screen: build-proto-py
    #!/usr/bin/env bash
    set -euo pipefail
    _py="${SYS_PYTHON:-/usr/bin/python3}"
    _json="proto/default_screen.json"
    _py_out="app/src/touchy_pad/_proto"
    if [ -f "${_json}" ] \
        && [ "${_json}" -nt "proto/gen_default_screen.py" ] \
        && [ "${_json}" -nt "app/src/touchy_pad/api/screens.py" ] \
        && [ "${_json}" -nt "${_py_out}/widgets_pb2.py" ]; then
        echo "gen-default-screen: up to date"
        exit 0
    fi
    "${_py}" proto/gen_default_screen.py "${_json}"

# Compile proto/default_screen.json (the firmware's built-in fallback
# screen, shown when no host-uploaded screens are present) into a C++
# header carrying its serialised protobuf bytes. Depends on
# gen-default-screen (which regenerates the JSON from the DSL) and thus
# transitively on build-proto-py.
default_screen_json := justfile_directory() + "/proto/default_screen.json"
default_screen_out  := justfile_directory() + "/firmware/main/default_screen_pb.h"
build-default-screen: gen-default-screen
    #!/usr/bin/env bash
    set -euo pipefail
    _py="${SYS_PYTHON:-/usr/bin/python3}"
    _out="firmware/main/default_screen_pb.h"
    _json="proto/default_screen.json"
    _py_out="app/src/touchy_pad/_proto"
    if [ -f "${_out}" ] \
        && [ "${_out}" -nt "${_json}" ] \
        && [ "${_out}" -nt "proto/embed_screen_json.py" ] \
        && [ "${_out}" -nt "${_py_out}/touchy_pb2.py" ] \
        && [ "${_out}" -nt "${_py_out}/widgets_pb2.py" ]; then
        echo "build-default-screen: up to date"
        exit 0
    fi
    "${_py}" proto/embed_screen_json.py \
        "${_json}" "${_out}" default_screen_pb

# ---------------------------------------------------------------------------
# Host app (app/) — Poetry-driven, but the proto module is a build-time
# dependency so every recipe depends on build-proto-py to keep the package
# self-consistent.
# ---------------------------------------------------------------------------

# Install dependencies into the Poetry-managed venv.
app-install:
    env -u VIRTUAL_ENV poetry install --no-interaction --directory app

# Run the test suite. Ensures the generated proto module is present first.
app-test: build-proto-py
    env -u VIRTUAL_ENV poetry run --directory app pytest

# Run the linter.
app-lint: build-proto-py
    env -u VIRTUAL_ENV poetry run --directory app ruff format src/touchy_pad tests
    cd app && env -u VIRTUAL_ENV poetry lock
    env -u VIRTUAL_ENV poetry run --directory app ruff check --fix src/touchy_pad tests

# Build the public-API HTML docs into docs/python-api/ (commit-friendly).
# Requires the optional `docs` Poetry group: `poetry install --with docs`.
build-docs: build-proto-py
    env -u VIRTUAL_ENV poetry run --directory app mkdocs build --site-dir ../docs/python-api

# Build wheel + sdist into app/dist/. Regenerates proto first so the
# wheel always contains an up-to-date touchy_pb2.py.
app-build: build-proto-py
    cd app && env -u VIRTUAL_ENV poetry build

# Run the touchy CLI inside the Poetry venv. Forward extra args:
#   just app-run -- version
# On segfault: PYTHONFAULTHANDLER prints the Python traceback; catchsegv
# (when available) prints the C-level backtrace via libSegFault.so.
app-run *ARGS: build-proto-py
    #!/usr/bin/env bash
    if command -v catchsegv &>/dev/null; then
        PYTHONFAULTHANDLER=1 catchsegv env -u VIRTUAL_ENV poetry run --directory app touchy {{ARGS}}
    else
        PYTHONFAULTHANDLER=1 env -u VIRTUAL_ENV poetry run --directory app touchy {{ARGS}}
    fi

# Change touchpad image to a custom graphic
app-touchpad-image *ARGS:
    just app-run {{ARGS}} touchpad image https://www.geeksville.com/robots.png 

# Change touchpad image to the bundled cute cat gif
app-touchpad-gif *ARGS:
    just app-run {{ARGS}} touchpad gif 

# Decode a coredump backtrace from the firmware logs. Forward the raw hex PCs as args:
app-backtrace *ARGS:
    #!/usr/bin/env bash
    set -euo pipefail
    # activate_idf_v6.0.1.sh's is_sourced() checks ${0##*/} against "bash"|"sh"|etc.
    # Just's temp-script has a random name, so sourcing fails. Re-exec under a new
    # bash that passes "bash" as $0 so is_sourced() returns true. Use a marker env
    # var (not IDF_PATH) to break the re-exec loop.
    if [ -z "${TOUCHY_IDF_SOURCED:-}" ]; then
        exec bash -c 'source ~/.espressif/tools/activate_idf_v6.0.1.sh && TOUCHY_IDF_SOURCED=1 exec bash "$1"' bash "$0"
    fi
    xtensa-esp32s3-elf-addr2line -pfiaC -e firmware/build/touchy_pad_v2.elf {{ARGS}}

# ---------------------------------------------------------------------------
# Rust library + demo (rust/) — pure-Rust async client.
# ---------------------------------------------------------------------------

# Sync the [workspace.package] version in rust/Cargo.toml from the top-level
# VERSION file. All individual crates inherit it via `version.workspace = true`.
# Run this before cutting a release:
#   just _rust-sync-version && just rust-build
_rust-sync-version:
    #!/usr/bin/env bash
    set -euo pipefail
    ver=$(head -1 VERSION | tr -d '[:space:]')
    # perl -i works identically on Linux and macOS (no BSD sed -i '' dance needed).
    PERL_BADLANG=0 perl -i -pe "s/^version = \"[^\"]+\"/version = \"$ver\"/" rust/Cargo.toml
    echo "rust workspace version → $ver"

# Build the Rust workspace (library + demo binary).
rust-build: _rust-sync-version
    cd rust && cargo build --workspace
    cd rust/touchy-pad && cargo publish --dry-run --allow-dirty

# Run the Rust test suite.
rust-test:
    cd rust && cargo test --workspace

# Format-check and clippy-lint the Rust workspace.
rust-lint:
    #!/usr/bin/env bash
    set -euo pipefail
    cd rust
    cargo fmt --all -- --check
    cargo clippy --workspace --all-targets -- -D warnings

# Auto-format Rust sources.
rust-fmt:
    cd rust && cargo fmt --all

# Build rustdoc HTML for the touchy-pad crate.
rust-doc:
    cd rust && cargo doc --workspace --no-deps

# Run the touchy-demo binary. Forward extra args:
#   just rust-run -- listen
rust-run *ARGS:
    cd rust && cargo run -p touchy-demo -- {{ARGS}}

# Build the OpenDeck device plugin (debug binary).
opendeck-build-debug: _rust-sync-version
    cd rust && cargo build -p touchy-opendeck

# Build the OpenDeck device plugin (release binary).
opendeck-build-release: _rust-sync-version
    cd rust && cargo build -p touchy-opendeck --release

# Package the .sdPlugin folder (with the host-triple binary) into a
# zip ready to install from OpenDeck's "Install plugin from file…"
# dialog. Writes to rust/target/com.geeksville.touchypad.sdPlugin.zip.
opendeck-package: opendeck-build-release
    #!/usr/bin/env bash
    set -euo pipefail
    cd rust
    cargo build -p touchy-opendeck --release
    triple="$(rustc -vV | awk '/^host:/ {print $2}')"
    bin="target/release/touchy-opendeck"
    case "$triple" in
        *windows*) ext=".exe" ;;
        *)         ext="" ;;
    esac
    out="touchy-opendeck/com.geeksville.touchypad.sdPlugin"
    mkdir -p "$out/$triple/bin"
    cp "${bin}${ext}" "$out/$triple/bin/touchy-opendeck${ext}"
    rm -f target/com.geeksville.touchypad.sdPlugin.zip
    (cd touchy-opendeck && python3 -m zipfile -c ../target/com.geeksville.touchypad.sdPlugin.zip com.geeksville.touchypad.sdPlugin)
    echo "wrote rust/target/com.geeksville.touchypad.sdPlugin.zip"

# ---------------------------------------------------------------------------
# Versioning
# ---------------------------------------------------------------------------

# Increment the patch component of the semver in VERSION, bump the build
# number, commit the change, tag it "vX.Y.Z", and push both the commit and
# the tag to origin.  Requires a clean git index on the branch to be pushed.
# Optionally accepts a version argument (e.g., `just bump-version 0.2.0`).
bump-version VERSION="":
    #!/usr/bin/env bash
    set -euo pipefail
    ver=$(sed -n '1p' VERSION)
    build=$(sed -n '2p' VERSION)
    if [ -n "{{VERSION}}" ]; then
        new_ver="{{VERSION}}"
    else
        IFS='.' read -r major minor patch <<< "$ver"
        new_ver="$major.$minor.$((patch + 1))"
    fi
    new_build=$((build + 1))
    printf '%s\n%s\n' "$new_ver" "$new_build" > VERSION
    # Keep app/pyproject.toml in sync — the regex matches only the bare
    # semver on the [tool.poetry] version line, not dependency version specs.
    sed -i "s/^version = \"[0-9][0-9.]*\"/version = \"${new_ver}\"/" app/pyproject.toml
    just rust-build # to update rust/Cargo.toml from the new VERSION
    echo "Bumped: $ver → $new_ver  (build $new_build)"
    git add VERSION app/pyproject.toml rust/Cargo.toml rust/Cargo.lock
    git commit -m "chore: bump version to v${new_ver}"
    git tag "v${new_ver}"
    git push
    git push origin "v${new_ver}"
    echo "✓ Tagged and pushed v${new_ver}"

# ---------------------------------------------------------------------------
# Firmware (firmware/) — ESP-IDF CMake build.
# ---------------------------------------------------------------------------

# Build the firmware. Regenerates C proto bindings first so the firmware
# always compiles against the latest schema.
firmware-build: build-proto-c build-default-screen
    #!/usr/bin/env bash
    # The IDF activate script detects sourcing via ${0##*/}; when Just runs a
    # recipe the temp-script name doesn't match "bash", so sourcing would fail.
    # Run a fresh bash -c so $0 is "bash" and is_sourced() returns true.
    # Try the devcontainer wrapper first; fall back to the standard export.sh
    # used in CI and manual installs (no error if neither — idf.py may already
    # be on PATH from an outer shell).
    exec bash -c '
        if [ -f "$HOME/.espressif/tools/activate_idf_v6.0.1.sh" ]; then
            source "$HOME/.espressif/tools/activate_idf_v6.0.1.sh" 2>/dev/null
        elif [ -f "$HOME/esp/esp-idf/export.sh" ]; then
            source "$HOME/esp/esp-idf/export.sh"
        fi
        idf.py -C firmware build
    '

# Regenerate the per-board sdkconfig from defaults.  Accepts an optional board
# name; if omitted, honours the $BOARD env var (set by CI) and falls back to
# the CMakeLists.txt default (jc4827w543).
#   just firmware-reconfigure                    # use default / $BOARD
#   just firmware-reconfigure waveshare_s3_lcd_7b
#
# The ESP-IDF target chip (esp32 / esp32s3) is read from
# firmware/boards/<board>/target so each board is self-describing; classic
# ESP32 boards like esp32_2432s028rv3 build for `esp32`, the rest for `esp32s3`.
firmware-reconfigure board="":
    #!/usr/bin/env bash
    # {{board}} is expanded by Just; if empty, honour $BOARD env var or fall
    # back to the CMakeLists.txt default.
    _just_board='{{board}}'
    export _BOARD="${_just_board:-${BOARD:-jc4827w543}}"
    # Read the chip target from the board's `target` file (default esp32s3).
    _target_file="firmware/boards/${_BOARD}/target"
    if [ -f "${_target_file}" ]; then
        _IDF_TARGET="$(tr -d '[:space:]' < "${_target_file}")"
    else
        _IDF_TARGET="esp32s3"
    fi
    export _IDF_TARGET
    exec bash -c '
        if [ -f "$HOME/.espressif/tools/activate_idf_v6.0.1.sh" ]; then
            source "$HOME/.espressif/tools/activate_idf_v6.0.1.sh" 2>/dev/null
        elif [ -f "$HOME/esp/esp-idf/export.sh" ]; then
            source "$HOME/esp/esp-idf/export.sh"
        else
            echo "error: cannot find ESP-IDF activation script" >&2
            echo "  tried: ~/.espressif/tools/activate_idf_v6.0.1.sh" >&2
            echo "  tried: ~/esp/esp-idf/export.sh" >&2
            exit 1
        fi
        idf.py -C firmware -DBOARD="${_BOARD}" set-target "${_IDF_TARGET}"
        idf.py -C firmware -DBOARD="${_BOARD}" reconfigure
    '

# Erase the flash filesystem
flash-erase: 
    #!/usr/bin/env bash
    set -euo pipefail
    # activate_idf_v6.0.1.sh's is_sourced() checks ${0##*/} against "bash"|"sh"|etc.
    # Just's temp-script has a random name, so sourcing fails. Re-exec under a new
    # bash that passes "bash" as $0 so is_sourced() returns true. Use a marker env
    # var (not IDF_PATH) to break the re-exec loop.
    if [ -z "${TOUCHY_IDF_SOURCED:-}" ]; then
        exec bash -c 'source ~/.espressif/tools/activate_idf_v6.0.1.sh && TOUCHY_IDF_SOURCED=1 exec bash "$1"' bash "$0"
    fi
    # Pick the first readable+writable ttyACM*/ttyUSB* under /host/dev/.
    # Native-USB boards enumerate as ttyACM*; UART-bridge boards (CH340 on
    # esp32_2432s028rv3) enumerate as ttyUSB*.
    port=""
    for candidate in $(ls /host/dev/ttyACM* /host/dev/ttyUSB* 2>/dev/null | sort); do
        if [ -r "$candidate" ] && [ -w "$candidate" ]; then
            port="$candidate"
            break
        fi
    done
    if [ -z "$port" ]; then
        echo "error: no accessible ttyACM*/ttyUSB* device found under /host/dev/" >&2
        exit 1
    fi    
    esptool -p "$port" -b 460800 --chip esp32s3 erase_region 0x310000 0xC0000

flash: firmware-build
    #!/usr/bin/env bash
    set -euo pipefail
    # activate_idf_v6.0.1.sh's is_sourced() checks ${0##*/} against "bash"|"sh"|etc.
    # Just's temp-script has a random name, so sourcing fails. Re-exec under a new
    # bash that passes "bash" as $0 so is_sourced() returns true. Use a marker env
    # var (not IDF_PATH) to break the re-exec loop.
    if [ -z "${TOUCHY_IDF_SOURCED:-}" ]; then
        exec bash -c 'source ~/.espressif/tools/activate_idf_v6.0.1.sh && TOUCHY_IDF_SOURCED=1 exec bash "$1"' bash "$0"
    fi
    # Pick the first readable+writable ttyACM*/ttyUSB* under /host/dev/.
    # Native-USB boards enumerate as ttyACM*; UART-bridge boards (CH340 on
    # esp32_2432s028rv3) enumerate as ttyUSB*.
    port=""
    for candidate in $(ls /host/dev/ttyACM* /host/dev/ttyUSB* 2>/dev/null | sort); do
        if [ -r "$candidate" ] && [ -w "$candidate" ]; then
            port="$candidate"
            break
        fi
    done
    if [ -z "$port" ]; then
        echo "error: no accessible ttyACM*/ttyUSB* device found under /host/dev/" >&2
        exit 1
    fi
    echo "flashing to $port"
    esptool_py="esptool"
    cd {{justfile_directory()}}/firmware/build
    # Read the chip target from the build cache so this works for both
    # esp32s3 (native-USB boards) and esp32 (CYD / UART-bridge boards).
    chip=$(grep -m1 '^IDF_TARGET:STRING=' CMakeCache.txt | cut -d= -f2)
    chip="${chip:-esp32s3}"
    mapfile -t flash_args < flash_args
    # flash_args has two lines: flags line, then addr:file pairs per line.
    # Flatten them into a single array of words.
    args=()
    for line in "${flash_args[@]}"; do
        read -ra words <<< "$line"
        args+=("${words[@]}")
    done
    # We used to pass in the following options...
    # --before default-reset --after hard-reset 
    "$esptool_py" -p "$port" -b 460800 \
        --chip "$chip" write-flash "${args[@]}"

# Produce a single merged firmware binary (bootloader + partition table +
# app, all at their flash offsets, padded with 0xFF) at
# firmware/build/touchy_pad_merged.bin. This is what `touchy update`
# downloads from a GitHub release and flashes to a fresh board at 0x0.
merge-bin: firmware-build
    #!/usr/bin/env bash
    set -euo pipefail
    # Same re-exec trick as flash: pass "bash" as $0 so is_sourced() returns true.
    if [ -z "${TOUCHY_IDF_SOURCED:-}" ]; then
        exec bash -c 'source ~/.espressif/tools/activate_idf_v6.0.1.sh && TOUCHY_IDF_SOURCED=1 exec bash "$1"' bash "$0"
    fi
    out="touchy_pad_merged.bin"
    esptool_py="$(command -v esptool || command -v esptool.py)"
    if [ -z "${esptool_py}" ]; then
        echo "error: esptool not found on PATH" >&2
        exit 1
    fi
    cd {{justfile_directory()}}/firmware/build
    chip=$(grep -m1 '^IDF_TARGET:STRING=' CMakeCache.txt | cut -d= -f2)
    chip="${chip:-esp32s3}"
    mapfile -t flash_args < flash_args
    args=()
    for line in "${flash_args[@]}"; do
        read -ra words <<< "$line"
        args+=("${words[@]}")
    done
    "${esptool_py}" --chip "$chip" merge-bin -o "${out}" "${args[@]}"
    echo "wrote {{justfile_directory()}}/firmware/build/${out}"

# End-user flash flow: build → merge → flash the merged image at 0x0.
# Mirrors what the `touchy update` CLI does after downloading a release
# asset, so local hardware bring-up can exercise the exact same path.
flash-merged: merge-bin
    #!/usr/bin/env bash
    set -euo pipefail
    if [ -z "${IDF_PATH:-}" ]; then
        exec bash -c 'source ~/.espressif/tools/activate_idf_v6.0.1.sh && exec bash "$1"' -- "$0"
    fi
    port=""
    for candidate in $(ls /host/dev/ttyACM* /host/dev/ttyUSB* 2>/dev/null | sort); do
        if [ -r "$candidate" ] && [ -w "$candidate" ]; then
            port="$candidate"
            break
        fi
    done
    if [ -z "$port" ]; then
        echo "error: no accessible ttyACM*/ttyUSB* device found under /host/dev/" >&2
        exit 1
    fi
    chip=$(grep -m1 '^IDF_TARGET:STRING=' {{justfile_directory()}}/firmware/build/CMakeCache.txt | cut -d= -f2)
    chip="${chip:-esp32s3}"
    echo "flashing merged image to $port (chip: $chip)"
    esptool -p "$port" -b 460800 \
        --before default-reset --after hard-reset \
        --chip "$chip" write-flash 0x0 {{justfile_directory()}}/firmware/build/touchy_pad_merged.bin


# ---------------------------------------------------------------------------
# Aggregate convenience targets
# ---------------------------------------------------------------------------

# Build everything
build-all: firmware-build app-build rust-build

test-interactive:
    env -u VIRTUAL_ENV poetry run --directory app touchy screens demo --listen

# Lint + test everything (currently just the host app).
test: app-lint app-test

# ---------------------------------------------------------------------------
# streamdeck-probe
# ---------------------------------------------------------------------------

# Run streamdeck-probe via its Poetry venv. Extra args are forwarded:
#   just streamdeck-probe --sim --sim-headless
streamdeck-probe *ARGS:
    #!/usr/bin/env bash
    set -euo pipefail
    # Ensure the editable touchy-pad link is present (Poetry 2.x drops path
    # deps from [tool.poetry.dependencies] when a [project] table exists).
    cd tools/streamdeck-probe
    poetry run pip install -q -e ../../app[sim]
    poetry run streamdeck-probe {{ARGS}}

# ---------------------------------------------------------------------------
# StreamController
# ---------------------------------------------------------------------------

sc_dir  := justfile_directory() + "/tools/StreamController"
sc_venv := sc_dir + "/.venv"
sc_pip  := sc_venv + "/bin/pip"
sc_py   := sc_venv + "/bin/python3"

# Create the StreamController venv and install requirements if needed,
# then launch StreamController via the touchy_bootstrap shim so any
# attached TouchyPads (or, with --sim, an in-process simulated one)
# show up in StreamController's device list. The esp32ulp cross-linker
# is stripped from PATH so meson can detect the native host linker when
# building dbus-python.
#
# StreamController is tracked as a git submodule; this recipe ensures
# it's initialized and up-to-date on the pr-touchypad branch before
# running.
#
# Flags forwarded as env vars to touchy_bootstrap:
#   --sim          : spin up an in-process sim device (TOUCHY_SIM=1)
#   --sim-headless : with --sim, skip the PySide6 SimWindow
streamcontroller-run *ARGS:
    #!/usr/bin/env bash
    set -euo pipefail
    # Ensure the StreamController submodule is initialized and updated
    # to the latest pr-touchypad branch.
    git submodule update --init --remote tools/StreamController
    if [ ! -f "{{sc_pip}}" ]; then
        echo "Creating StreamController venv…"
        python3 -m venv {{sc_venv}}
    fi
    stamp="{{sc_venv}}/.requirements_installed"
    if [ ! -f "$stamp" ] || [ "{{sc_dir}}/requirements.txt" -nt "$stamp" ]; then
        echo "Installing StreamController requirements…"
        {{sc_pip}} install --upgrade pip
        {{sc_pip}} install -r {{sc_dir}}/requirements.txt
        touch "$stamp"
    fi
    # Always (re-)install the local app/ in editable mode so the live source
    # tree is used instead of whatever PyPI version requirements.txt pulled in.
    # Include the [sim] extra so PySide6 is available when --sim is passed.
    {{sc_pip}} install -q -e {{justfile_directory()}}/app[sim]
    # Parse our own flags out of ARGS so the rest can be forwarded to main.py.
    export TOUCHY_SIM=0
    export TOUCHY_SIM_HEADLESS=0
    forward_args=()
    for arg in {{ARGS}}; do
        case "$arg" in
            --sim) TOUCHY_SIM=1 ;;
            --sim-headless) TOUCHY_SIM=1; TOUCHY_SIM_HEADLESS=1 ;;
            *) forward_args+=("$arg") ;;
        esac
    done
    cd {{sc_dir}} && PATH="$CLEAN_PATH" {{sc_py}} touchy_bootstrap.py "${forward_args[@]}"

# Run OpenDeck in Tauri dev mode (hot-reloading frontend + live Rust backend).
# Requires Rust and Deno to be installed (see .devcontainer/Containerfile).
opendeck-run: opendeck-build-debug
    #!/usr/bin/env bash
    set -euo pipefail
    export PATH="$HOME/.cargo/bin:$HOME/.deno/bin:$PATH"
    git submodule update --init tools/OpenDeck
    cd "$(git rev-parse --show-toplevel)/tools/OpenDeck"
    if [ ! -d node_modules ] && [ ! -d .deno ]; then
        echo "Running deno install…"
        deno install
    fi
    # In devcontainers the host /dev/ is bind-mounted at /host/dev/.
    # Symlink any hidraw devices visible on the host but absent from the
    # container's /dev/ so that hidapi can enumerate Stream Deck devices.
    for _d in /host/dev/hidraw*; do
        [ -e "$_d" ] || continue
        _name=$(basename "$_d")
        [ -e "/dev/$_name" ] || sudo ln -sf "$_d" "/dev/$_name" 2>/dev/null || true
    done
    exec dbus-run-session -- deno task tauri dev

# Configure opendeck to use my 'live' dev build of the
opendeck-dev-start: 
    mkdir -p ~/.config/opendeck/plugins
    ln -sfn {{justfile_directory()}}/rust/touchy-opendeck/com.geeksville.touchypad.sdPlugin \
            ~/.config/opendeck/plugins/com.geeksville.touchypad.sdPlugin

# Remove the development version of my plugin from opendeck
opendeck-dev-stop:
    rm -f ~/.config/opendeck/plugins/com.geeksville.touchypad.sdPlugin

# Watch the opendeck plugin log
opendeck-log:
    tail -F ~/.local/share/opendeck/logs/plugins/com.geeksville.touchypad.sdPlugin.log

# Remove generated artifacts. The proto outputs are rebuilt on the next
# `just build-proto*` invocation.
clean:
    rm -f {{py_proto_dst}}/touchy_pb2.py {{py_proto_dst}}/touchy_pb2.pyi
    rm -f {{py_proto_dst}}/widgets_pb2.py {{py_proto_dst}}/widgets_pb2.pyi
    rm -rf {{c_proto_dst}}
    rm -f {{default_screen_out}}
    rm -rf {{justfile_directory()}}/app/dist {{justfile_directory()}}/app/build {{justfile_directory()}}/app/src/*.egg-info
