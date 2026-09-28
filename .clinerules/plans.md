# Plans

Implementation and design plans live in `docs/plans/*.md` and are tracked in git.
(Note the plural `docs/` — this is the project's *documentation* tree, not the
mkdocs site: `app/mkdocs.yml` builds the published Python API docs from
`app/docs/`, so plans written here are never published.)

## Rules

- When I create or edit a plan, I MUST save it as `docs/plans/<topic>.md`
  (kebab-case, e.g. `trackpad-gestures.md`). Create the directory if it does
  not exist yet.
- Always persist plans to that directory so they can eventually be committed —
  never leave a plan only in chat or in a temporary location.
- One plan per file. Update the existing file for the topic instead of creating
  a near-duplicate; use dated filenames only when an explicit supersession is
  intended.
- Plans are implementation-oriented. Each should include: goal/summary,
  approach, library/technology choices with rationale, file & component layout,
  a phased implementation sequence, testing strategy, and risks/open questions.
- Touchy-Pad specifics to cover in a plan: **which side(s)** it touches
  (`firmware/main/`, `app/src/touchy_pad/`, `rust/`, `proto/`), and whether it
  moves a wire version (`Widget.Version`, `SysBoardInfoResponse.ProtocolVersion`,
  `PreferencesFile.Version`) or is additive — plus the `just` recipes
  (`just build-proto`, `just app-test`, `just firmware-build`) that validate it.
- When a plan drives active work, reference it from the memory bank
  (`activeContext.md`).
- Do not delete a plan; mark it superseded and link the replacement.

## Existing plans

None yet (as of 2026-09-28) — `docs/plans/` is created by the first plan. Docs
that are *not* plans but are easy to confuse with them:

| Doc | What it is |
|-----|------------|
| `docs/design.md` | Authoritative stage-by-stage history — "what stage are we on". Update it when a stage finishes. |
| `docs/TODO.md` | Loose feature backlog / wish list (checkboxes), not a design. |
| `docs/open-issues.md` | Known bugs and rough edges. |
| `docs/*.md` | Shipped user/developer docs (`host-api.md`, `ui.md`, `simulator.md`, …). |
| `.clinerules/memory-bank/` | Cline's cross-session context — not project documentation. |

When a plan is added, list it here with a one-line summary and cross-reference
it from `activeContext.md`; when it is superseded, keep the file and link the
replacement rather than deleting it.