# Publishing the Rust crate (`touchy-pad`) like Rust developers expect

**Status:** implemented as **Stage lb22** — and **shipped**: `touchy-pad` 0.3.9 is
live on crates.io (first publish 2026-11-15). The crate reports `trustpub_only`,
so publishing runs through GitHub OIDC; `release.yml` prefers the OIDC token and
falls back to the `CARGO_REGISTRY_TOKEN` secret (the path used for that first
release). See "Publishing" below.

**Sides touched:** `rust/` (crate layout + new `proto-gen` member), `Justfile`
(`rust-gen-proto`, `build-proto`), `.github/workflows/app-ci.yml` (freshness
check), repo submodules (`tools/autopage` removed). **No** `firmware/`, **no**
`app/`, **no** `proto/*.proto` change — so **no wire-version bump**
(`Widget.Version` 30 / `ProtocolVersion` 14 / `PreferencesFile.Version` 9 all
stay put). The checked-in bindings are a re-emission of the same schema and were
verified byte-identical to what `touchy-pad/build.rs` used to produce.

## Goal

Let a Rust developer consume the library the normal ways — `cargo add
touchy-pad` (crates.io), or `touchy-pad = { git = "…" }` — without installing a
protobuf compiler and without the monorepo's `tools/` submodules getting in the
way.

## Where we started (measured, not assumed)

| Check | Result |
|---|---|
| `cargo add touchy-pad` in a fresh crate | ❌ `error: the crate touchy-pad could not be found in registry index` |
| crates.io | no such crate; the name is unclaimed |
| `cargo package` / `cargo publish --dry-run` (what `just rust-build` runs) | ✅ already passed |
| consumer build of the packaged crate | ✅ worked, but ❌ **failed without `protoc`** (`prost-build` needs the binary) |
| `touchy-pad = { git = "…/touchy-pad" }` | ❌ hard failure: cargo recursively initialises submodules and `tools/autopage`'s *nested* `autopage-recipes` submodule is pinned to a revision that no longer exists upstream |
| same with `[net] git-fetch-with-cli = true` | ❌ identical failure — there is no cargo knob to skip submodules |

So the crate was **packaged but never distributed**, and both escape hatches
(git dependency, build-from-source) had sharp edges.

## Approach

### 1. Check the prost bindings in; delete the build script

`rust/touchy-pad/src/proto/touchy.rs` is committed and `include!`d by
`src/proto.rs`. `touchy-pad` therefore has **no `build.rs`**, no `prost-build`
build-dependency and no `protoc` requirement; a consumer's graph loses
`prost-build` (and its heck/itertools/petgraph/prettyplease/syn/regex deps).

Rationale: this is exactly what the project already does for Python —
`app/pyproject.toml` gitignores `_proto/*_pb2.py` but explicitly bundles them
into the sdist/wheel, so `pip install touchy-pad` needs no protoc, and ruff
`extend-exclude`s the generated modules. Cargo ships *source*, so the only way to
hand a consumer pre-generated code is to commit it.

Generation moves into a new unpublished workspace member `rust/proto-gen`
(`publish = false`, depends on `prost-build`) which writes that file. Keeping the
generator as a separate crate — rather than a `build.rs` that skips itself — is
what actually removes `prost-build` from consumer builds.

### 2. Regenerate via Just; fail CI on drift

* `just rust-gen-proto` → `cd rust && cargo run -p proto-gen`.
* `just build-proto` now also runs it, so one command refreshes Python, C, Rust
  and the embedded default screen together (house rule: *regenerate, never
  hand-edit*).
* `app-ci.yml`'s `test-rust` re-runs the generator and fails if the committed
  file differs. That is the **mandatory** mitigation for the one real cost of
  checked-in generated code — drift. A stale binding would otherwise only show
  up as a silent wire mismatch.
* The include!-only module gets `#![allow(missing_docs)] #![allow(clippy::all)]`
  in `src/proto.rs`, mirroring the Python `extend-exclude` policy. Side benefit:
  a newer clippy had started failing `just rust-lint` on the generated
  `Layout::Trackpad` variant (`clippy::large_enum_variant`); the same allow fixes
  that, and it was pre-existing (reproduced against the old, build-script crate).

### 3. Drop the `tools/autopage` submodule

The submodule existed only for browsing reference code; no recipe used it. Its
*tree* contains a nested submodule (`autopage-recipes`) pinned at `8196e80…`,
which no longer exists in that repo (`geeksville/autopage-recipes` now only has
`a94ecfb`), and upstream `geeksville/autopage@main` still pins the dead SHA — so
bumping our pointer alone could not have fixed it. Cargo's recursive submodule
update therefore failed for **any** git consumer, and `git clone
--recurse-submodules` was equally broken.

The other four submodules are all fetchable and none has nested submodules
(`StreamController`, `plugins`, `streamcontroller-python-elgato-streamdeck` have
no `.gitmodules`; OpenDeck's is empty), and the recipes that use them
(`just streamcontroller-run`, `just opendeck-run`) keep working.

### 4. Package polish

`include` now ships `src/**`, `proto/*.proto` and `tests/**` (27 files, up from
21): the tests stop triggering cargo's six "ignoring test … is not included in
the published package" warnings, and the packaged crate stays testable.

## Alternatives considered

| Option | Why not |
|---|---|
| keep `build.rs` + `protoc-bin-vendored` | still compiles `prost-build` for every consumer; adds a platform-binary build-dep |
| keep `build.rs` + `protobuf-src` | prost's official "compile protoc from source" answer, but needs a C++ toolchain and takes minutes |
| document `protoc` as a prerequisite | a poor deal for a public library when the Python package already promises better |
| distribute via `git =` only | drags the whole `tools/` tree (StreamController + a Tauri app) into every consumer |
| remove all `tools/` submodules | `just streamcontroller-run` / `just opendeck-run` genuinely need StreamController/OpenDeck |

## Layout

```
rust/
├── Cargo.toml                 # members = […, "proto-gen"]
├── proto-gen/                 # publish = false — build tooling only
│   ├── Cargo.toml
│   └── src/main.rs
└── touchy-pad/
    ├── Cargo.toml             # no [build-dependencies]; include += tests/**
    └── src/
        ├── proto.rs           # include!("…/src/proto/touchy.rs")
        └── proto/touchy.rs    # checked in (prost output, 1768 lines)
```

`rust/touchy-pad/proto/*.proto` stay as symlinks to the shared schemas: the crate
keeps documenting its own wire format, and `cargo package` exports them into the
tarball as real files.

## Phases

1. **Submodule** — `git submodule deinit -f tools/autopage`, `git rm -f
   tools/autopage`, drop the `.gitmodules` section, delete
   `.git/modules/tools/autopage`. *(done)*
2. **Bindings** — add `rust/proto-gen`; move the `prost_build` config out of
   `touchy-pad/build.rs` (same `disable_comments(["."])`, one flat `touchy.rs`);
   generate + commit `src/proto/touchy.rs`; point `src/proto.rs` at it; delete
   `build.rs`; drop the `prost-build` build-dependency. *(done)*
3. **Glue** — `just rust-gen-proto`, wire it into `build-proto`, add the CI
   freshness check. *(done)*
4. **Validate** (below). *(done)*
5. **Publish** — manual, see the checklist. *(outstanding)*

## Validation performed

| Check | Result |
|---|---|
| generated file vs the old `build.rs` `OUT_DIR/touchy.rs` | `diff -q` → identical |
| determinism | two consecutive runs → same sha256 (`1a657468…`) |
| `just rust-test` | pass (lib, framing / loopback / image-cache / pad-events, Python-sim TCP, doctest) |
| `just rust-lint` | pass, including the generated module — it was **failing before** this change |
| `cargo package` | 27 files, 279.8 KiB; verify build OK |
| packaged manifest | `build = false` — no build script in the published crate |
| consumer build of the packaged crate with `PROTOC=/nonexistent/protoc` | ✅ exit 0 — no protoc, no build script |
| `git =` dependency (index-exported tree, since the removal is staged rather than committed) | ✅ exit 0; resolved `touchy-pad 0.3.9` at `rust/touchy-pad/Cargo.toml` |
| remaining submodule remotes | all four reachable; none has nested submodules |

## Risks / open questions

* **Drift** — the checked-in bindings can go stale. Mitigated by the CI check and
  `just build-proto`; the sanctioned failure mode is a red CI step, never a
  silent wire mismatch.
* **`protoc` is still needed to *regenerate*** bindings (so `just build-proto`,
  CI and the devcontainer keep it). That is inherent; only *consumers* are freed.
* **The submodule fix only takes effect once committed** — cargo clones `HEAD`,
  so a git consumer keeps seeing the old `.gitmodules` (with the dead nested pin)
  until the removal lands in history.
* **License text is not in the package.** The metadata carries
  `license = "GPL-3.0-or-later"` (so crates.io/docs.rs are satisfied), but no
  `LICENSE` file ships. Options: a crate-local copy, a symlink to `../../LICENSE`
  (awkward on Windows, where `core.symlinks=false` degrades it to a text file),
  or `license-file`. Left as a maintainer decision rather than guessed.
* **README links** — `rust/README.md` is what crates.io renders, so its relative
  links (`../README.md`, `docs/rust-api.md`) don't resolve there and were switched
  to absolute GitHub URLs.
* **`serial` is invisible on docs.rs** unless
  `[package.metadata.docs.rs] features = ["serial"]` is added (docs.rs's image
  does ship `libudev-dev`). Optional; skipped to keep docs builds minimal.
* **No CI publishing job yet** — a release still needs a manual `cargo publish`
  (or a `release.yml` job with a `CARGO_REGISTRY_TOKEN` secret).

## Publishing

`release.yml` publishes the crate on every published GitHub release, from a
`publish-crates-io` job that depends on the same `test-rust` gate the PR CI runs
(the gate is a reusable workflow, `.github/workflows/test-rust.yml`). The job
first asks crates.io whether that version already exists and skips the publish if
it does, so re-running a release — or publishing by hand first — is harmless.
`cargo publish` there needs no `protoc` and no build script (the bindings are
checked in).

Auth, in order of preference:

1. **crates.io trusted publishing (OIDC)** — preferred, and effectively required
   for this crate: crates.io reports `trustpub_only` for `touchy-pad`, so token
   publishing is not accepted. The job mints a short-lived token with
   `rust-lang/crates-io-auth-action@v1` (job-level `id-token: write`); if no
   trusted publisher is configured, that step fails softly and the job falls back
   to the secret below.
2. **`CARGO_REGISTRY_TOKEN` repository secret** — the fallback, and the path that
   published 0.3.9 (a trusted publisher can only be configured once the crate
   exists, so the very first release had to use a token).

### One-time setup — first release (token secret — done for 0.3.9)

Kept as the documented fallback path. A trusted publisher can only be configured
once the crate exists, so the first release went out with a token:

1. Sign in at <https://crates.io> (GitHub OAuth).
2. **Account Settings → API Tokens** (<https://crates.io/settings/tokens>) →
   *New Token*, e.g. named `touchy-pad release`:
   * scopes: **`publish-new`** (to create `touchy-pad`) **+ `publish-update`**
     (for later releases); add `yank` too if the workflow should be able to
     yank, and `change-owners` only if you want it to manage owners;
   * crates: a `publish-new` token can't be restricted to a crate that doesn't
     exist yet, so it will cover new crates — give it a **short expiry** and/or
     revoke it once the crate exists and trusted publishing is set up.
3. Copy the token (shown once) and add it in GitHub as
   **Settings → Secrets and variables → Actions → New repository secret**, named
   exactly **`CARGO_REGISTRY_TOKEN`**.
4. Cut a release as usual (`just bump-version`, then publish the GitHub release)
   — the workflow publishes the crate as part of it. Publishing by hand first
   (`cargo login` + `cargo publish --manifest-path rust/touchy-pad/Cargo.toml`)
   also works; the job then sees the version and skips.

### One-time setup — trusted publishing (no secret)

1. Do the token release above first, so the crate exists.
2. On the crate's crates.io page (or your account's trusted-publishing settings):
   add a **GitHub** trusted publisher naming this repository's owner
   (`geeksville`), its name (`touchy-pad`), the **workflow file name**
   (`release.yml`) and, if you scope the job with a GitHub *environment*, that
   environment's name. Note the coupling: renaming `release.yml`, the repo, or
   adding an environment without updating crates.io breaks the OIDC exchange.
   Field labels/current wording: <https://crates.io/docs/trusted-publishing>.
3. Optionally delete the `CARGO_REGISTRY_TOKEN` secret — the workflow uses the
   OIDC token whenever it can mint one, so the secret only matters as a fallback
   for a crate without a trusted publisher. (Keeping it is harmless.)

### Manual fallback / local check

```sh
cargo login                              # paste the API token once, locally
just bump-version                        # bumps VERSION, re-syncs rust/Cargo.toml
just build-proto                         # refresh the binding sets (needs protoc)
cargo package --allow-dirty --manifest-path rust/touchy-pad/Cargo.toml   # inspect the tarball
cargo publish --manifest-path rust/touchy-pad/Cargo.toml
```

crates.io versions are immutable, so every release needs a `VERSION` bump
(`just bump-version` also re-syncs `rust/Cargo.toml` via `just rust-build`).

