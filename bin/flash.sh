#!/usr/bin/env bash
# Portable flashing helper for a source checkout.
#
#   bin/flash.sh write    # flash firmware/build/flash_args
#   bin/flash.sh erase    # erase the 4M-layout LittleFS storage partition
#   bin/flash.sh merge    # merge the built images into one binary
#   bin/flash.sh merged   # flash the merged binary at offset 0x0
#
# The `just flash`, `just flash-erase`, `just merge-bin` and
# `just flash-merged` recipes are thin wrappers around this script.
#
# End users should use `touchy update` instead: it downloads a release
# image and needs neither this checkout nor ESP-IDF.
#
# Environment:
#   TOUCHY_PORT          serial port to use (skips discovery; validated)
#   TOUCHY_IDF_VERSION   ESP-IDF activate-script version (default: v6.1)
#   TOUCHY_DEV_ROOTS     colon-separated directories to search for ports
#                        instead of the defaults (/dev on macOS;
#                        /host/dev:/dev elsewhere). Mostly for tests and
#                        unusual container mounts.
#
# Bash 3.2 compatible: no BASH 4-only array builtins or associative arrays.

set -euo pipefail

_script_path="${BASH_SOURCE[0]}"
repo_root="$(cd -P "$(dirname "$_script_path")/.." && pwd)"
build_dir="${repo_root}/firmware/build"

_usage() {
	echo "usage: bin/flash.sh {write|erase|merge|merged}" >&2
}

_fail() {
	echo "error: $*" >&2
	exit 1
}

# Source the ESP-IDF environment once, if one is installed. Falls back to
# an already-active toolchain (esptool on PATH). The activate/export scripts
# run under a bash whose $0 is "bash" because IDF's is_sourced() checks it.
_idf_activate() {
	if [ -n "${TOUCHY_IDF_SOURCED:-}" ]; then
		return 0
	fi

	local version="${TOUCHY_IDF_VERSION:-v6.1}"
	local wrapper="${HOME}/.espressif/tools/activate_idf_${version}.sh"
	local export_sh="${HOME}/esp/esp-idf/export.sh"
	local activate=""

	if [ -f "$wrapper" ]; then
		activate="$wrapper"
	elif [ -f "$export_sh" ]; then
		activate="$export_sh"
	else
		return 0
	fi

	TOUCHY_IDF_SOURCED=1 exec bash -c '
        _activate=$1
        shift
        if ! . "$_activate"; then
            echo "error: ESP-IDF activation failed: $_activate" >&2
            echo "  If it mentioned a missing python module (e.g. rich), the venv is" >&2
            echo "  incomplete. Repair it and retry:" >&2
            echo "    ( cd ~/esp/esp-idf && ./install.sh esp32,esp32s3,esp32p4 )" >&2
            echo "  (in the devcontainer: re-run the container setup)." >&2
            exit 1
        fi
        exec bash "$@"
    ' bash "$activate" "$0" "$@"
}

_need_esptool() {
	esptool_bin="$(command -v esptool || command -v esptool.py || true)"
	if [ -z "$esptool_bin" ]; then
		echo "error: esptool not found on PATH." >&2
		echo "  Install ESP-IDF (firmware/README.md) or: python -m pip install esptool" >&2
		exit 1
	fi
}

_chip() {
	chip="esp32s3"
	if [ -f "${build_dir}/CMakeCache.txt" ]; then
		cached="$(grep -m1 '^IDF_TARGET:STRING=' "${build_dir}/CMakeCache.txt" | cut -d= -f2 || true)"
		chip="${cached:-esp32s3}"
	fi
}

# Flatten firmware/build/flash_args into the global `flash_args` array.
# The first line(s) hold esptool flags; each data line is "<offset> <path>".
# The path stays one word so build paths with spaces survive.
_read_flash_args() {
	local args_file="${build_dir}/flash_args"
	if [ ! -f "$args_file" ]; then
		echo "error: ${args_file} is missing." >&2
		echo "  Run: just firmware-build" >&2
		exit 1
	fi

	flash_args=()
	local line offset path
	while IFS= read -r line || [ -n "$line" ]; do
		[ -z "$line" ] && continue
		case "$line" in
		0x*)
			offset="${line%% *}"
			path="${line#* }"
			if [ "$path" = "$line" ]; then
				_fail "malformed line in flash_args: $line"
			fi
			flash_args+=("$offset" "$path")
			;;
		*)
			local -a words=()
			read -ra words <<<"$line"
			flash_args+=("${words[@]}")
			;;
		esac
	done <"$args_file"

	if [ "${#flash_args[@]}" -eq 0 ]; then
		_fail "flash_args is empty; run: just firmware-build"
	fi
}

# Print the one serial port to use. Exits with a readable error when the
# choice is ambiguous, so we never guess before writing hardware.
_find_port() {
	local explicit="${TOUCHY_PORT:-}"
	if [ -n "$explicit" ]; then
		if [ ! -e "$explicit" ] || [ ! -r "$explicit" ] || [ ! -w "$explicit" ]; then
			_fail "TOUCHY_PORT=${explicit} is not a readable+writable device."
		fi
		printf '%s\n' "$explicit"
		return 0
	fi

	local roots
	local -a patterns=()
	case "$(uname -s)" in
	Darwin)
		roots="${TOUCHY_DEV_ROOTS:-/dev}"
		patterns=("cu.usbmodem*" "cu.usbserial*" "cu.wchusbserial*" "cu.SLAB_USBtoUART*")
		;;
	*)
		roots="${TOUCHY_DEV_ROOTS:-/host/dev:/dev}"
		patterns=("ttyACM*" "ttyUSB*")
		;;
	esac

	local remaining="$roots" root match pattern key
	local candidates=""
	while [ -n "$remaining" ]; do
		root="${remaining%%:*}"
		case "$remaining" in
		*:*) remaining="${remaining#*:}" ;;
		*) remaining="" ;;
		esac
		[ -d "$root" ] || continue
		for pattern in "${patterns[@]}"; do
			for match in "$root"/$pattern; do
				[ -e "$match" ] || continue
				[ -r "$match" ] && [ -w "$match" ] || continue
				key="$(cd -P "${match%/*}" && pwd)/${match##*/}"
				case "|${candidates}|" in
				*"|${key}|"*) continue ;;
				esac
				candidates="${candidates:+${candidates}|}${key}"
			done
		done
	done

	if [ -z "$candidates" ]; then
		echo "error: no accessible USB serial port found." >&2
		echo "  Searched: ${roots}" >&2
		case "$(uname -s)" in
		Darwin)
			echo "  macOS USB serial ports look like /dev/cu.usbmodem* or /dev/cu.usbserial*." >&2
			;;
		*)
			echo "  Linux ports are /host/dev/ttyACM*, /host/dev/ttyUSB*, /dev/ttyACM* or /dev/ttyUSB*." >&2
			;;
		esac
		echo "  Use a USB data cable; a charge-only cable never creates a serial port." >&2
		echo "  If the port is missing, hold BOOT, tap RST, release BOOT, then retry." >&2
		echo "  Or set TOUCHY_PORT=/path/to/port." >&2
		exit 1
	fi

	local rest="$candidates" candidate count=0
	while [ -n "$rest" ]; do
		candidate="${rest%%|*}"
		case "$rest" in
		*"|"*) rest="${rest#*|}" ;;
		*) rest="" ;;
		esac
		count=$((count + 1))
	done

	if [ "$count" -gt 1 ]; then
		echo "error: more than one USB serial port found. Set TOUCHY_PORT and retry:" >&2
		rest="$candidates"
		while [ -n "$rest" ]; do
			candidate="${rest%%|*}"
			case "$rest" in
			*"|"*) rest="${rest#*|}" ;;
			*) rest="" ;;
			esac
			echo "  ${candidate}" >&2
		done
		exit 1
	fi

	printf '%s\n' "$candidates"
}

_write_args() {
	_read_flash_args
	_chip
	local port
	port="$(_find_port)"
	echo "flashing to ${port} (chip: ${chip})"
	cd "$build_dir"
	"$esptool_bin" -p "$port" -b 460800 \
		--chip "$chip" write-flash "${flash_args[@]}"
}

_erase_args() {
	_chip
	local port
	port="$(_find_port)"
	echo "erasing storage on ${port} (chip: ${chip})"
	# 0x310000 + 0xC0000 is the `storage` (LittleFS) partition in the
	# 4M layout. The 16M layout puts storage at 0x410000; do not use this
	# recipe on a 16M build.
	"$esptool_bin" -p "$port" -b 460800 \
		--chip "$chip" erase_region 0x310000 0xC0000
}

_merge() {
	_read_flash_args
	_chip
	cd "$build_dir"
	local out="touchy_pad_merged.bin"
	"$esptool_bin" --chip "$chip" merge-bin -o "$out" "${flash_args[@]}"
	echo "wrote ${build_dir}/${out}"
}

_flash_merged() {
	local image="${build_dir}/touchy_pad_merged.bin"
	if [ ! -f "$image" ]; then
		echo "error: ${image} is missing." >&2
		echo "  Run: just merge-bin" >&2
		exit 1
	fi
	_chip
	local port
	port="$(_find_port)"
	echo "flashing merged image to ${port} (chip: ${chip})"
	"$esptool_bin" -p "$port" -b 460800 \
		--before default-reset --after hard-reset \
		--chip "$chip" write-flash 0x0 "$image"
}

action="${1:-}"
case "$action" in
write | erase | merge | merged) ;;
*)
	_usage
	exit 2
	;;
esac

if [ "$#" -ne 1 ]; then
	_fail "unexpected argument: $2"
fi

_idf_activate "$action"
_need_esptool

case "$action" in
write) _write_args ;;
erase) _erase_args ;;
merge) _merge ;;
merged) _flash_merged ;;
esac
