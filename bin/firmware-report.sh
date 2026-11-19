#!/usr/bin/env bash
# Print a flash / RAM usage report for the currently configured board+target.
#
#   bin/firmware-report.sh
#
# Builds the firmware (so the numbers match the current sources) and runs
# ESP-IDF's size report, then prints an app-partition summary line with the
# bytes used, the percentage full and the bytes free.
#
# The board + chip are whatever the existing build config
# (firmware/build/CMakeCache.txt) already has, so run
# `just firmware-reconfigure [board]` first to switch target. The
# `just firmware-report` recipe is a thin wrapper around this script.
#
# Environment:
#   TOUCHY_IDF_VERSION   ESP-IDF activate-script version (default: v6.1)
#
# Bash 3.2 compatible (macOS): no Bash 4-only builtins.

set -euo pipefail

_script_path="${BASH_SOURCE[0]}"
repo_root="$(cd -P "$(dirname "$_script_path")/.." && pwd)"

# Locate the ESP-IDF activate/export script, if one is installed; otherwise the
# inner shell falls back to whatever toolchain (idf.py) is already on PATH.
# Mirrors bin/flash.sh so both work on macOS and in the devcontainer.
_version="${TOUCHY_IDF_VERSION:-v6.1}"
_wrapper="${HOME}/.espressif/tools/activate_idf_${_version}.sh"
_export_sh="${HOME}/esp/esp-idf/export.sh"
_activate=""
if [ -f "$_wrapper" ]; then
	_activate="$_wrapper"
elif [ -f "$_export_sh" ]; then
	_activate="$_export_sh"
fi

log="$(mktemp)"
trap 'rm -f "$log"' EXIT

# `idf.py size` builds first, then reports. Run it under a bash whose $0 is
# "bash" (so IDF's is_sourced() check accepts the activate script) *and in that
# same shell*: IDF exposes idf.py as a shell function (plus an alias), which does
# not survive a re-exec. Tee the output so we can both show it and parse
# check_sizes.py's app-partition line.
set +e
bash -c '
	_activate=$1; _repo=$2; _log=$3
	if [ -n "$_activate" ]; then
		. "$_activate" || {
			echo "error: ESP-IDF activation failed: $_activate" >&2
			echo "  If it mentioned a missing python module (e.g. rich), the venv is" >&2
			echo "  incomplete. Repair it and retry:" >&2
			echo "    ( cd ~/esp/esp-idf && ./install.sh esp32,esp32s3,esp32p4 )" >&2
			echo "  (in the devcontainer: re-run the container setup)." >&2
			exit 1
		}
	fi
	if ! command -v idf.py >/dev/null 2>&1; then
		echo "error: idf.py not found on PATH." >&2
		echo "  Install ESP-IDF, or set TOUCHY_IDF_VERSION so the activate script is found." >&2
		exit 1
	fi
	cd "$_repo" || exit 1
	idf.py -C firmware size 2>&1 | tee "$_log"
	exit "${PIPESTATUS[0]}"
' bash "$_activate" "$repo_root" "$log"
rc=$?
set -e

# e.g. "touchy_pad_v2.bin binary size 0x1b6c80 bytes. Smallest app partition is
#       0x300000 bytes. 0x149380 bytes (43%) free."
line="$(grep -m1 'Smallest app partition' "$log" || true)"
used="$(printf '%s' "$line" | sed -n 's/.*binary size \(0x[0-9a-fA-F]*\) bytes.*/\1/p')"
tot="$(printf '%s' "$line" | sed -n 's/.*Smallest app partition is \(0x[0-9a-fA-F]*\) bytes.*/\1/p')"
if [ "$rc" -eq 0 ] && [ -n "$used" ] && [ -n "$tot" ]; then
	usedd=$((used))
	totd=$((tot))
	pct=$((usedd * 100 / totd))
	printf '\nApp image: %d of %d bytes used — %d%% full (%d bytes free)\n' \
		"$usedd" "$totd" "$pct" "$((totd - usedd))"
fi
exit "$rc"
