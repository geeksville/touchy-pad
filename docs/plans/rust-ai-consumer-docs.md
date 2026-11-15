# Documentation aimed at AI consumers of the Rust crate

**Status:** implemented as **Stage lb23**. Sides touched: `rust/touchy-pad/`
(examples, crate docs, `# Errors` sections, `Cargo.toml`), repo root
(`CHANGELOG.md`), docs. **No wire change** — `proto/*.proto` is untouched, so
`Widget.Version` 30 / `ProtocolVersion` 14 / `PreferencesFile.Version` 9 stay put
and the checked-in bindings are unchanged.

## Goal

Rust has **no AI-specific documentation standard**. What it has is a served,
machine-consumable documentation surface (rustdoc on docs.rs, plus rustdoc JSON)
and a set of conventions (the Rust API Guidelines) that agents already ingest.
So the goal is not a new format — it is to put usage recommendations **where
consumers can actually retrieve them**, and to make the crate legible to both a
human and an agent that has only `cargo add` + docs.rs.

## What exists (verified, `docs/plans/crates-io-publishing.md` context)

* **Cross-language conventions** — `llms.txt` (a *proposal*, v2 by Jeremy
  Howard, explicitly not a standard; web-oriented: index file + `llms-full.txt`
  + `.md` page twins) and `AGENTS.md`. The repo already has both, and
  `llms.txt:425` has a "Rust API (async, no libusb)" section.
* **The Rust-specific machine-readable surface** — docs.rs has published
  **rustdoc JSON** for every crate/version since 2025-05-23
  (`https://docs.rs/crate/touchy-pad/latest/json`, `.gz`, pinned
  `json/<format-version>`, per-target). Free with publishing, nothing to
  configure.
* **Nothing serves `llms.txt` for a crate** — crates.io serves the rendered
  README, docs.rs serves rustdoc. An agent that only reads those never sees the
  root `llms.txt`, so the crate-doc copy of the same knowledge is what counts.

## Approach

1. **`examples/` in the crate** — the idiomatic Rust "usage recommendation"
   channel: compiled by `cargo test`/`clippy --all-targets`, shipped in the
   tarball, and read by agents that fetch the source. Today the only worked
   examples live in `touchy-demo`, a *separate `publish = false` crate* that is
   invisible from the touchy-pad crate page.
2. **A "For AI agents" block in the crate docs** (`src/lib.rs`) — the five facts
   that prevent wrong code, stated where crates.io/docs.rs readers see them, plus
   a pointer to the repo `llms.txt` for depth.
3. **`[package.metadata.docs.rs] features = ["serial"]`** — without it docs.rs
   builds with no features and the whole serial transport is absent from the
   rendered API, so no agent will ever suggest it.
4. **`#[doc(alias = "…")]`** on the entry points — the only Rust-native
   retrieval aid (rustdoc search aliases; also surfaces in rustdoc JSON).
5. **`# Errors` sections** (C-FAILURE) on every public fallible API, with
   `#![warn(clippy::missing_errors_doc)]` so it cannot silently rot.
6. **`CHANGELOG.md`** (C-RELNOTES) — the migration signal a crate currently
   doesn't emit at all; `docs/design.md` is the deep stage history, not
   release notes.

## Decisions and trade-offs

* **`doc(cfg)` / `cfg_attr(docsrs, …)` deliberately not used.** Feature labels
  in rustdoc need a `docsrs` cfg plus a `[lints.rust] unexpected_cfgs`
  declaration to stay warning-free on stable; prose in the module docs gives the
  same information without the toolchain noise.
* **`missing_panics_doc` not enabled** (only `missing_errors_doc`). Documenting
  panics honestly would mean auditing every `expect()` first; noted as a
  possible follow-up rather than smuggled in.
* **`examples/**` added to `include`** so the examples travel in the `.crate`
  (they need dev-dependencies, which ship in the manifest anyway).
* **CHANGELOG starts at 0.3.5.** Entries are reconstructed from the tagged
  commit ranges; older releases only have `docs/design.md` + git tags, and
  inventing summaries for them would be worse than saying so.

## Layout

```
CHANGELOG.md                       # Keep-a-Changelog style, newest first
rust/touchy-pad/
├── Cargo.toml                     # include += examples/**; [package.metadata.docs.rs]
├── examples/
│   ├── discover.rs                # enumerate USB / UART / simulator, print board info
│   ├── screen.rs                  # build a screen from prost structs, show it
│   ├── events.rs                  # stream touch events (events() is one-shot)
│   └── image_cache.rs             # upload once, point buttons at cached T: paths
└── src/
    ├── lib.rs                     # "For AI agents" block, doc aliases, lint enables
    ├── client.rs                  # module-level error model + per-RPC # Errors
    ├── pad.rs                     # # Errors on the high-level API
    └── …                          # + # Errors in images / image_cache / discover / transport*
```

## Validation performed

* `cargo build --examples` + `cargo clippy --all-targets -- -D warnings` clean
  (the examples are compiled and linted, so they cannot rot).
* `cargo test --workspace` + `cargo fmt --check` green via `just rust-test` /
  `just rust-lint`.
* `cargo package` shows the examples in the tarball; `cargo doc` builds without
  warnings, and `missing_errors_doc` is clean (proving every public fallible API
  is documented).
* `just build-proto` → the checked-in bindings are unchanged (no wire change).

## Follow-ups / open questions

* `missing_panics_doc`; `doc(cfg)` feature badges if the `docsrs` cfg plumbing is
  ever added.
* The "For AI agents" block quotes the current wire versions; it must be updated
  when a `Widget.Version` / `ProtocolVersion` / prefs bump lands (same rule as
  the root `llms.txt`).
* A crate-level `llms.txt` shipped inside the tarball is possible via `include`
  but nothing consumes it automatically — the crate-doc block is the version that
  actually reaches consumers.
* Long term, the cleanest AI-facing artifact is the docs.rs **rustdoc JSON**;
  nothing to do beyond publishing.
