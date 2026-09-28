# Terminal safety

I cannot type into a shell that is waiting for input, so **any command line that
leaves the shell at a secondary prompt hangs forever**. The classic failure: a
composite one-liner that mixes `&&`, backgrounding (`&`) and quoted strings gets
one quote mis-escaped, and bash drops to its `dquote>` / `quote>` / `>` prompt.
The intended command never even starts — so don't "wait and retry", fix the
invocation.

Rules:

- **Keep each command a single, simple statement.** Do not chain backgrounding
  (`nohup ... &`, `& echo $!`) onto a long `&&` chain. If you need a long
  command, write it to a temp script file first (via the editor/heredoc) and run
  the script — the quoting is then fixed and there is nothing left to parse
  interactively.
- **Avoid `&`-backgrounding inside a composite line.** To run something long,
  either run it in the foreground with a bounded `timeout <seconds> <cmd>`, or
  redirect to a log and poll the log. Never background and then leave the line
  open with a trailing quote/prompt construct.
- **Prefer a heredoc for multi-line scripts**, and always terminate it correctly:
  `python - <<'EOF' ... EOF`, `bash <<'EOF' ... EOF`. A missing/extra `EOF` or a
  stray quote also yields the `dquote>`/`heredoc>` hang.
- **If a command seems to hang, verify before retrying.** Check whether the
  process actually started (`ps aux | grep -c '[p]ytest'`) and whether its log
  exists/grew. If neither happened, the shell hung on *parsing*, not work — do
  not re-run the same malformed line.
- **Double-check quoting of `!`/`$!`/`"`** before sending: history expansion and
  escaping can survive one layer and break the next. A short shell script file
  sidesteps all of it.

## What blocks in *this* repo (read before running anything long)

- **`git`** — always `--no-pager` (`git --no-pager log|diff|show|branch -v`);
  this repo's logs/diffs are long enough to page (see `collaboration.md`).
- **`just firmware-build`** (and therefore `just flash`, `just merge-bin`,
  `just build-all`) is an ESP-IDF build: several minutes, thousands of lines of
  output. Run it with a bounded `timeout <seconds> …` and/or redirect to a log
  and poll the log — never leave the line open.
- **Never run bare `idf.py`.** It needs the IDF environment that
  `just firmware-build` sources for you, and `just flash` picks its own port
  (first accessible `/host/dev/tty{ACM,USB}*`, else it errors out) — so there is
  never a reason to hand-write an `esptool`/`idf.py` call that waits for a
  device selection. See AGENTS.md → *Build / test / run*.
- **`touchy update`** (and `just app-run -- update …`) calls `click.confirm`
  and has **no** `--yes`/`--force` flag, so it hangs the moment it wants
  confirmation — if it must be scripted, pipe into it (`yes | …`), otherwise
  hand it to the user. (The `just flash` / `just flash-merged` recipes are
  different: they pick their own port and never confirm — they are just slow,
  per the firmware-build bullet above.)
- **Long-lived foreground commands**: `just test-interactive` /
  `just app-run -- --listen …` (streams host events until Ctrl-C),
  `touchy --sim-gui` (opens a Qt window), `just build-docs` /
  `mkdocs serve`, and `just streamcontroller-run` (builds a venv, then runs a
  GUI app). Use the headless equivalent when the GUI is not the point
  (`just app-run -- --sim-headless …`) and a bounded `timeout` otherwise.
- **`poetry`** — pass `--no-interaction` (the Justfile and `just init` do), and
  prefer the recipes (`just app-test`, `just app-lint`, `just app-run -- …`)
  over ad-hoc `poetry run` invocations. Note the Poetry project root is `app/`,
  so from the repo root the form is `poetry run --directory app …`.
- **Device I/O is not guaranteed**: most `just` recipes need `/host/dev/tty*`
  or libusb. If a device is absent, prefer `--sim-headless` (see
  `docs/simulator.md`) over retrying against hardware.
