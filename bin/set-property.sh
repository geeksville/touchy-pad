#!/usr/bin/env bash
# Set a widget's `text` property on a networked Touchy-Pad via the JSON
# variant of the HTTP command API (Stage lb13).
#
# Usage: bin/set-property.sh URL WIDGET VALUE...
#   URL      device base URL, e.g. http://192.168.1.42 or https://touchypad.local
#   WIDGET   target Widget.id (e.g. "welcome")
#   VALUE... the new text (remaining args are joined with spaces)
#
# Example:
#   bin/set-property.sh http://192.168.1.42 welcome "You got an email this is a test message..."
#
# This POSTs canonical protobuf-JSON — the same shape `touchy --debug`
# logs — so no protobuf tooling is needed, just curl.
set -euo pipefail

if [ "$#" -lt 3 ]; then
    echo "usage: $0 URL WIDGET VALUE..." >&2
    exit 2
fi

base_url="${1%/}"   # strip trailing slash if any
widget="$2"
shift 2
value="$*"

# JSON-escape the value (quotes + backslashes) so arbitrary text is safe.
esc_value=$(printf '%s' "$value" | sed 's/\\/\\\\/g; s/"/\\"/g')

curl -sS -X POST \
    -H 'Content-Type: application/json' \
    --data "{\"setProperty\":{\"widgetId\":\"${widget}\",\"propertyName\":\"text\",\"stringValue\":\"${esc_value}\"}}" \
    "${base_url}/touchy/api/v1/command"
echo
