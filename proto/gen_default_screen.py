#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate ``proto/default_screen.json`` from the Python DSL.

The firmware ships a built-in fallback screen (shown when no
host-uploaded screens are present on the device filesystem). Rather than
hand-maintain that layout twice, it is generated from the single source
of truth in :func:`touchy_pad.api.screens.build_setup_screen` — a
self-contained "run ``touchy init``" hint plus an inlined trackpad (no
``widget_ref`` into ``F:host/uscr/``, which doesn't exist on a virgin
device, and no prev/next chrome). This script serialises that screen to
protobuf JSON; ``proto/embed_screen_json.py`` then turns the JSON into
the embedded C header ``firmware/main/default_screen_pb.h``.

Usage::

    gen_default_screen.py [OUTPUT.json]

Defaults to ``proto/default_screen.json``. Runnable from the repo root
without installing the app first — it puts ``app/src`` (and the generated
``_proto`` bindings) on ``sys.path``.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "output",
        nargs="?",
        type=Path,
        default=None,
        help="Where to write the JSON (default: proto/default_screen.json)",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    out = args.output or (repo_root / "proto" / "default_screen.json")

    # Make the host package importable straight from the source tree.
    sys.path.insert(0, str(repo_root / "app" / "src"))

    from google.protobuf import json_format  # noqa: E402

    from touchy_pad.api.screens import build_setup_screen  # noqa: E402

    screen = build_setup_screen()
    msg = screen.to_proto()
    body = json_format.MessageToJson(msg, indent=2)

    # NOTE: keep the file pure JSON — both consumers
    # (proto/embed_screen_json.py and the simulator's _load_default_screen)
    # call json_format.Parse, which rejects `//`-style comments. The
    # "this file is generated" provenance lives in the Justfile recipe and
    # docs, not in the file itself.
    out.write_text(body + "\n", encoding="utf-8")
    print(f"wrote {out} (screen {screen.name!r})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
