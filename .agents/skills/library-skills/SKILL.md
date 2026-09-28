---
name: library-skills
description: Use Library Skills to discover, install, refresh, repair, check, and manage agent skills from installed packages.
---

# Library Skills

Use this skill when a project might benefit from agent skills bundled by its installed packages, or when existing Library Skills-managed symlinks are stale, broken, orphaned, or need to be checked.

Run commands from the project root.

Agents bundle their own skills by including an `.agents/skills` directory. More details in [Library Skills](https://library-skills.io).

This project uses **Poetry**, so run the tool through the project virtualenv as
`poetry run library-skills` (never `uvx`/`pip install` — see `AGENTS.md` →
*Build / test / run*). `library-skills` is installed as a dev dependency, so
`poetry install --with dev` provides it.

## First-Time Setup

- Make sure project dependencies are installed first with `poetry install --with dev`.
- Run `poetry run library-skills` to discover skills bundled by the installed packages and install selected skills interactively.
- Use `poetry run library-skills --all` only when all newly discovered skills should be installed without selecting individual skills.
- Use `poetry run library-skills --tool-skill` to copy this Library Skills tool skill into the project so future agents know how to discover, install, update, repair, and check skills.

## Commands

- Run `poetry run library-skills` to discover package-provided skills, install selected new skills, and reconcile existing managed symlinks.
- Run `poetry run library-skills list` to inspect discovered and installed skills.
- Run `poetry run library-skills list --json` for machine-readable installed status.
- Run `poetry run library-skills scan --json` for discovery-only automation.
- Run `poetry run library-skills --check` to validate managed skill symlink state without changing files.
- Run `poetry run library-skills --yes` to repair stale managed symlinks and remove orphaned managed symlinks non-interactively.
- Add `--claude` when `.claude/skills` should also be managed.
- Add `--skill NAME` to install a specific discovered skill by name.

## Safety

- Prefer rerunning `poetry run library-skills` over editing managed symlinks manually.
- If installed skill symlinks are broken, dependencies may not be installed yet. Run the project's normal install command first (`poetry install --with dev`), then rerun `poetry run library-skills`.
- Do not delete or overwrite hand-authored skill directories.
- Library Skills only removes managed symlinks. It should not remove copied or hand-authored skill directories.
