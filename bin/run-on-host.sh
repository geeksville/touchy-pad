#!/bin/bash
# Script to run commands on the host from within a devcontainer
# 
# This script breaks container isolation by using podman to spawn a container
# that shares the host's namespaces (PID, network, IPC, UTS) and mounts the
# host's root filesystem and binaries.
#
# Usage: run-on-host.sh <command> [args...]
# Example: run-on-host.sh uname -a
#          run-on-host.sh cat /etc/hostname
#          run-on-host.sh rpm -qa | grep kernel
#
# Works with: Most CLI tools, rpm, dnf, system utilities
# Limitations: D-Bus/GUI tools (kdotool, etc.) may not work due to socket
#              permission isolation in nested containers. For those, install
#              them directly in the devcontainer instead.
#
# Note: Commands are executed using the host's binaries and libraries from
#       /usr, /bin, /lib, etc., running as host user (UID 1000).

set -euo pipefail

# Check if command is provided
if [ $# -eq 0 ]; then
    echo "Usage: run-on-host.sh <command> [args...]" >&2
    echo "Example: run-on-host.sh ls -la /home" >&2
    exit 1
fi

# The command to run is all arguments
COMMAND="$@"

# Use podman to run commands with access to host binaries
# We mount key host directories to use the host's binaries and libraries:
# - /usr: Host binaries and libraries
# - /lib64: Additional host libraries (on x86_64 systems)
# - /etc: Host system configuration
# - /var/home: Host home directories
#
# We also pass through important environment variables for GUI apps

# Conditionally mount the XAUTHORITY file — it typically lives in the
# devcontainer (/tmp/.docker.xauth) and must be visible inside the
# spawned container for X11 auth to work.
XAUTH_MOUNT=()
if [ -n "${XAUTHORITY:-}" ] && [ -f "${XAUTHORITY}" ]; then
    XAUTH_MOUNT=(-v "${XAUTHORITY}:${XAUTHORITY}:ro")
fi

# The host runs a systemd user session (and, on Wayland, the compositor)
# whose sockets live under /run/user/1000 — already bind-mounted into the
# container. The devcontainer doesn't export the matching env vars, so
# derive them here. Without DBUS_SESSION_BUS_ADDRESS, dbus-aware tools
# (kdotool, etc.) fall back to `dbus-launch`, which tries X11 and fails
# with "Authorization required, but no authorization protocol specified".
XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=${XDG_RUNTIME_DIR}/bus}"
# Pick up the Wayland display socket if present (host is Wayland).
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -S "${XDG_RUNTIME_DIR}/wayland-0" ]; then
    WAYLAND_DISPLAY="wayland-0"
fi

podman run \
    --rm \
    --privileged \
    --pid=host \
    --network=host \
    --ipc=host \
    --uts=host \
    --userns=host \
    --user=1000:1000 \
    -v /usr:/usr:ro \
    -v /lib:/lib:ro \
    -v /lib64:/lib64:ro \
    -v /bin:/bin:ro \
    -v /sbin:/sbin:ro \
    -v /etc:/etc:ro \
    -v /var/home:/var/home:ro \
    -v /run/user/1000:/run/user/1000:rw \
    -v /run/dbus:/run/dbus:ro \
    -v /tmp/.X11-unix:/tmp/.X11-unix:ro \
    "${XAUTH_MOUNT[@]}" \
    -e DISPLAY="${DISPLAY:-}" \
    -e XAUTHORITY="${XAUTHORITY:-}" \
    -e XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR}" \
    -e DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS}" \
    -e WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-}" \
    -e HOME="${HOME:-/root}" \
    fedora:latest \
    sh -c "$COMMAND"
