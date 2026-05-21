#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile a touchy.Screen JSON description into an embedded C header.

Called from the Justfile to turn ``proto/default_screen.json`` into
``firmware/main/default_screen_pb.h`` — a small header carrying the
serialised protobuf bytes as a ``constexpr`` array. The firmware uses
this as a built-in fallback when no host-uploaded screens are present
on the device filesystem.

Usage::

    embed_screen_json.py INPUT.json OUTPUT.h SYMBOL_NAME

Standard protobuf JSON conventions are used (camelCase field names,
unknown fields raise). The schema is read via ``touchy_pb2`` which
must be importable — typically because ``just build-proto-py`` has
already generated it under ``app/src/touchy_pad/_proto/``.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="JSON description of a touchy.Screen")
    parser.add_argument("output", type=Path, help="C++ header to write")
    parser.add_argument("symbol", help="C identifier prefix for the array")
    args = parser.parse_args()

    # The host-side Python protobuf bindings live under the app package;
    # add them to sys.path so this script is runnable from the repo root
    # without installing the app first.
    repo_root = Path(__file__).resolve().parent.parent
    sys.path.insert(0, str(repo_root / "app" / "src" / "touchy_pad" / "_proto"))

    import touchy_pb2  # noqa: E402, F401 — registers shared messages
    import widgets_pb2  # noqa: E402
    from google.protobuf import json_format  # noqa: E402

    raw = args.input.read_text(encoding="utf-8")
    screen = json_format.Parse(raw, widgets_pb2.Screen())
    encoded = screen.SerializeToString()

    # Emit a tiny header with the bytes as a constexpr array. We use
    # std::byte rather than uint8_t so callers must pass it through
    # reinterpret_cast<const uint8_t*>(...) — the cast is a one-liner
    # at the consumer and we avoid pulling <cstdint> into the header.
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "//",
        f"// Generated from {args.input.name} by proto/embed_screen_json.py.",
        "// Do not edit by hand; re-run `just build-default-screen`.",
        "",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
    ]
    sym = args.symbol
    lines.append(f"inline constexpr unsigned char {sym}_data[] = {{")
    width = 12
    for i in range(0, len(encoded), width):
        chunk = encoded[i : i + width]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    lines.append("};")
    lines.append("")
    lines.append(f"inline constexpr std::size_t {sym}_len = sizeof({sym}_data);")
    lines.append("")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {args.output} ({len(encoded)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
