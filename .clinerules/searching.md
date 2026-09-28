# Code Search

**Default tool: `semble`, accessed through its MCP server.** Use the MCP tools
`semble__search` and `semble__find_related` — they are auto-approved (no
confirmation prompt) and share the exact index the CLI uses. Only fall back to
the `poetry run semble ...` CLI when the MCP tools are not available to you
(some clients/agents do not expose MCP) — see *CLI fallback* below. Never use
`uvx` or a bare `pip install` (see *Build / test / run* above).

The server is configured as `poetry -C /workspaces/touchy-pad/app run semble` in
`.cline/cline_mcp_settings.json`, with `search`/`find_related` auto-approved, so
the MCP call is the cheap path. `semble` is a dev dependency of the `app/`
Poetry project (`app/pyproject.toml`), so one index covers the whole repo.

> **Gotcha:** because of the `-C /workspaces/touchy-pad/app`, the *process* cwd is
> `app/`, so **every `repo` path is relative to `app/`** — there is no bare
> `firmware/main` here (`repo="."` means the Python project, not the repo root).
> Prefix the non-Python trees with `../`: `../firmware/main`,
> `../rust/touchy-pad/src`, `../proto`, `../docs`.

Use it to find code by describing what it does, or by naming a symbol/identifier,
instead of grep. `semble__search` parameters:

| Parameter | Meaning |
|---|---|
| `query` | What the code does, or a symbol/identifier name |
| `repo` | A path or git URL to index, **relative to `app/`** (see the gotcha above). `.`/`src` (Python package), `tests` (pytest), `../firmware/main` (C++), `../rust/touchy-pad/src`, `../proto`, `../docs` |
| `top_k` | Number of results; default 5, widen with 10+ |
| `content` | `code` (default), `docs` (prose), `config` (TOML/YAML/Kconfig/`.proto`), `all` |
| `max_snippet_lines` | Lines of source per hit; default 10 (signature + first body lines), `0` for path/line only, `null` for the full chunk |

```text
semble__search(query="where host events are dispatched", repo=".")                 # describe behaviour
semble__search(query="screen_save", repo="src")                                   # name a symbol
semble__search(query="trackpad swipe detection", repo="../firmware/main", top_k=10)  # widen results
semble__search(query="frame decoder", repo="src", max_snippet_lines=5)             # shorter output
semble__search(query="how stages are documented", repo="../docs", content="docs")  # prose
semble__search(query="wire version CURRENT", repo="../proto", content="config")    # config/proto
semble__search(query="backlight pwm", repo="../firmware/main", content="all")      # everything
```

The index is built on first run (and cached for subsequent runs) and invalidated
automatically when files change.

Use `semble__find_related` to discover code similar to a known location — pass
the `file_path` and `line` from a prior `semble__search` result (its other
parameters mirror `semble__search`):

```text
semble__find_related(file_path="touchy_pad/api/device.py", line=590, repo="src")
```

**Prefer scoping to the subtree you care about.** The first run indexes whatever
tree you point at; scoping keeps it fast and keeps the big, irrelevant trees
(`../firmware/build/`, `../firmware/managed_components/`, `../rust/target/`,
`../build/`, `../images/`, `../app/dist/`, `../tools/StreamController/`) out of
the index. Never widen to the whole repo (`repo=".."`) for a routine question.
`file_path` in the JSON results is relative to the `repo` path you passed, which
also makes the hits shorter and directly readable.

Generated files **do** show up in hits — recognise and skip them:
`firmware/main/proto/*.pb.{c,h}` (nanopb output, `just build-proto-c`),
`app/src/touchy_pad/_proto/*_pb2.py` (`just build-proto-py`), and
`docs/python-api/**` (built HTML from `just build-docs`). The real source of
truth for the wire format is `proto/*.proto`.

## CLI fallback

When the MCP server is not reachable, the CLI is the same dev dependency and the
same index — only the invocation differs (shell flags instead of structured
arguments):

```bash
# Run it from app/ (the Poetry project root) — many recipes use --directory app.
poetry run semble search "where host events are dispatched"        # describe behaviour
poetry run semble search "screen_save"                             # name a symbol
poetry run semble search "trackpad swipe" src                      # scope to the package
poetry run semble search "fixture setup" tests                     # scope to the tests
poetry run semble search "backlight level" ../firmware/main        # scope to the firmware
poetry run semble search "frame decoder" ../rust/touchy-pad/src    # scope to the Rust crate
poetry run semble search "trackpad swipe" src --top-k 10           # widen results
poetry run semble search "safe formatter" src --max-snippet-lines 10  # shorter output
poetry run semble search "how stages are documented" ../docs --content docs  # prose
poetry run semble search "wire version" ../proto --content config  # config/proto
poetry run semble search "backlight pwm" ../firmware/main --content all  # everything
poetry run semble find-related src/touchy_pad/api/device.py 590
```

`path` defaults to the *current* directory — and `poetry run --directory app`
(what the CLI fallback above assumes) makes that `app/`, so `../firmware/main`
is the normal way to reach the firmware. `poetry -C app run semble …` from the
repo root behaves identically (**the cwd is still `app/`**, so the `../` prefixes
stay). The `./my-project` argument in semble's upstream docs is a placeholder —
never copy it into a command here. `--top-k N` widens results;
`--max-snippet-lines N` shortens output.

If `semble` is missing it is a dev dependency of `app/`: run `just init` (or
`poetry -C app install`, which installs the `dev` group by default). Do not
reach for `uvx` or a bare `pip install` — this project is Poetry-only (see
AGENTS.md → *Build / test / run*).

## Choosing a tool (read this before reaching for `grep`)

| You need… | Use |
|---|---|
| "where is X handled / what does this do" | `semble__search(query="<description>", repo=".")` |
| a Python symbol by name (`screen_save`, `class Touchy`) | `semble__search(query="<symbol>", repo="src")` |
| firmware C++ flow (`TrackpadWidget::_swipe_process`) | `semble__search(query="<symbol>", repo="../firmware/main")` |
| **every** occurrence of a literal string repo-wide (rename/migration sweep) | `grep -rn` (bounded — see *Rules of thumb* below) |
| a structural outline of one already-known file (list its `def`s) | read the file, or `grep -n '^\s*def ' <file>` |
| a literal/regex sweep semble keeps missing | `grep -rn` or the `search_codebase` tool |

Rules of thumb:

1. Start with `semble__search` for anything semantic or symbol-shaped.
2. Navigate straight to the returned `file:line` — don't re-search or re-grep for the same content.
3. `grep` is for the two carve-outs above *only*: an exhaustive literal sweep, or outlining a single file you already know.
4. When in doubt, `semble` first — it's ranked and quieter than a repo-wide
   `grep` that wades through `firmware/build/`, `rust/target/` and generated
   bindings.
5. Keep sweeps out of generated + vendored trees: `firmware/build/`,
   `firmware/managed_components/`, `firmware/main/proto/`, `rust/target/`,
   `build/`, `images/`, `app/dist/`, `docs/python-api/`,
   `tools/StreamController/`, `.venv/` — and ignore generated hits
   (`*_pb2.py`, `*.pb.{c,h}`, embedded `default_screen_pb.h`).
6. If the MCP tools are unavailable, use the CLI fallback above — not grep.

## Workflow

1. `semble__search` first — for anything semantic or symbol-shaped.
2. Use `content="docs"` for prose (e.g. `docs/` — `design.md` is the stage
   history), `content="config"` for TOML/YAML/Kconfig/`.proto`, `content="all"`
   for everything. Default is `code`.
3. The code lives in three languages, so pick the right root — remembering the
   `app/` cwd: `src`/`tests` (Python host), `../firmware/main` (C++ firmware),
   `../rust/touchy-pad/src` (Rust crate), `../proto` (the wire format shared by
   all three).
4. Navigate directly to the returned `file:line` — do not re-search or `grep` for the same content.
5. `semble__find_related(file_path=..., line=...)` to find sibling implementations.
6. `grep` only for an exhaustive literal sweep (e.g. every caller of a renamed
   function) or to outline a single already-known file — never for semantic/symbol lookups.

