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
    parser.add_argument("output", type=Path, help="C++ header to write")
    parser.add_argument(
        "entry",
        nargs="+",
        metavar="INPUT:SYMBOL",
        help="One or more 'input.json:symbol_prefix' pairs to embed. Each "
        "emits a `<symbol>_data[]` / `<symbol>_len` pair into the single "
        "output header.",
    )
    args = parser.parse_args()

    entries: list[tuple[Path, str]] = []
    for raw in args.entry:
        in_str, sep, sym = raw.rpartition(":")
        if not sep or not in_str or not sym:
            parser.error(f"malformed entry {raw!r}; expected INPUT.json:SYMBOL")
        entries.append((Path(in_str), sym))

    # The host-side Python protobuf bindings live under the app package;
    # add its source root to sys.path so this script is runnable from the
    # repo root without installing the app first. Import the bindings via
    # the package (not flat) so their package-relative cross-imports work.
    repo_root = Path(__file__).resolve().parent.parent
    sys.path.insert(0, str(repo_root / "app" / "src"))

    from google.protobuf import json_format  # noqa: E402

    from touchy_pad._proto import (
        touchy_pb2,  # noqa: E402, F401 — registers shared messages
        widgets_pb2,  # noqa: E402
    )

    # Emit a tiny header with the bytes as a constexpr array. We use
    # std::byte rather than uint8_t so callers must pass it through
    # reinterpret_cast<const uint8_t*>(...) — the cast is a one-liner
    # at the consumer and we avoid pulling <cstdint> into the header.
    names = ", ".join(p.name for p, _ in entries)
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "//",
        f"// Generated from {names} by proto/embed_screen_json.py.",
        "// Do not edit by hand; re-run `just build-default-screen`.",
        "",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
    ]

    total = 0
    for input_path, sym in entries:
        raw = input_path.read_text(encoding="utf-8")
        screen = json_format.Parse(raw, widgets_pb2.Screen())
        encoded = screen.SerializeToString()
        total += len(encoded)
        lines.append(f"// {input_path.name} ({len(encoded)} bytes)")
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
    print(f"wrote {args.output} ({len(entries)} screen(s), {total} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
