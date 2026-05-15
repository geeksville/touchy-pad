# Touchy-Pad task runner. Run `just` to list recipes, `just <name>` to run one.
#
# https://just.systems/

# Default recipe: show the list of available targets.
default:
    @just --list

# Where generated proto outputs land. /tmp keeps them out of the source tree
# until we wire them into the firmware/host build systems properly.
proto_out := "/tmp/touchy-proto"

# The system Python (NOT the ESP-IDF venv). The ESP-IDF activate script
# overrides `python3` on PATH inside this devcontainer, but the host-side
# protobuf and nanopb packages were pip-installed to /usr/bin/python3.
sys_python := "/usr/bin/python3"

# Compile proto/touchy.proto into:
#   - Python bindings   (touchy_pb2.py, for the host CLI/library)
#   - C bindings        (touchy.pb.c / touchy.pb.h, for the firmware via nanopb)
# Both land under {{proto_out}} for now; later recipes will copy them into
# the actual host/firmware build trees.
build-proto:
    mkdir -p {{proto_out}}/python {{proto_out}}/c
    # --- Python via grpcio-tools (vendors its own protoc) ---------------
    {{sys_python}} -m grpc_tools.protoc \
        -Iproto \
        --python_out={{proto_out}}/python \
        proto/touchy.proto
    # --- C via nanopb ---------------------------------------------------
    # nanopb's generator wants to run from a directory that contains the
    # .proto file so its --proto_path defaults line up.
    cd proto && {{sys_python}} -m nanopb.generator.nanopb_generator \
        --output-dir={{proto_out}}/c \
        touchy.proto
    @echo ""
    @echo "Generated:"
    @ls -la {{proto_out}}/python {{proto_out}}/c
