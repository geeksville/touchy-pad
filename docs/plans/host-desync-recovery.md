# Host-side recovery from a desynced reply FIFO (stale `NOT_FOUND` read by `FileOpenWrite`)

**Status: DONE — host library (Python + Rust), no wire change.** Reported by
voicebot (an app consuming the published `touchy-pad` PyPI package) against a
`jc4827w543` pad on protocol 14. Mirrors the firmware fix in
`docs/plans/fs-write-transaction-recovery.md` (Stage lb20) — that one made the
device *self-heal*; this one stops the host from **creating** the leaked
transaction in the first place, and stops it mis-attributing the desync.

## Goal / summary

A host that is hard-killed mid-RPC (SIGKILL, crash, debugger stop) leaves its
reply unread in the device's bulk-IN FIFO. The wire protocol has no command id —
a `Response` is matched to its `Command` purely by FIFO order — so **every later
reply is one command behind** (`docs/open-issues.md`).

Two symptoms follow, both from the same desync:

1. **`FileOpenWrite` reads a leftover `NOT_FOUND`.** `TouchyClient.file_save()`
   raises `TouchyError: device returned NOT_FOUND (3)` even though the device
   *did* open the handle. Because `file_open_write()` was called **outside**
   `file_save()`'s `try/finally`, no `FileClose(commit=False)` is sent and the
   device is left with a live write transaction — one leak per failed attempt
   (`fs_open_write: stale transaction detected …` on the next session).
   Any app that gives up after one attempt shows nothing on the panel.
2. **`SysBoardInfoGet` reads a leftover `OK`.** A foreign `OK` with no
   `sys_board_info` payload parses as the *default* message
   (`protocol_version == 0`, 0×0 panel, empty name), which `touchy_open()`
   accepted as a real board — a phantom 0×0 board.

Both are invisible to applications after this plan: the offending RPC is
retried once after flushing the leftover frame(s).

## Root cause (verified)

| Where | Problem |
|---|---|
| `app/src/touchy_pad/api/client.py` `file_save()` | opens the handle outside the cleanup `try`; on a misread reply there is no handle to close, so nothing is sent and the device keeps the transaction |
| `app/src/touchy_pad/api/client.py` `file_open_write()` | trusts `reply.file_open_write.handle` even when the reply carries no payload → returns handle `0` on a desync |
| `app/src/touchy_pad/api/device.py` `touchy_open()` | `if device_version:` skips every check when the (default) reply says `protocol_version == 0` |
| `rust/touchy-pad/src/pad.rs` `file_write_raw()` | same unguarded `file_open_write()` before its abort path |
| `TouchyClient` | no lock around the whole open→write→close, so two threads can abort each other's transaction (the Rust crate already has `write_lock`) |

Why retrying is safe and unambiguous:

* The firmware answers `FileOpenWrite` with `IO_ERROR` when it really fails
  (`firmware/main/api/host_api.cpp`) and always carries a handle on success — it
  **never** emits `NOT_FOUND` there. So `NOT_FOUND` (or an `OK` with no handle)
  on this RPC is *always* another command's reply.
* The first attempt wrote nothing (it failed at open), and the retry's open()
  makes the firmware abort whatever transaction the lost open created
  (`fs_open_write()` aborts a stale transaction first — Stage lb20). One retry,
  bounded.

## Approach

**Python (`app/`)**

1. `TouchyClient.file_save()` splits into a retry wrapper + `_save_bytes()`
   (one open→write→close). On a `RESULT_NOT_FOUND` — or the new
   `_DesyncedReply` — it logs a warning, `_flush_stale_frames()`s the orphaned
   reply and retries **once**. `IO_ERROR` and every other code propagate
   immediately.
2. `TouchyClient.file_open_write()` validates the reply
   (`HasField("file_open_write")` + non-zero handle) and raises `_DesyncedReply`
   (a `TouchyError` with `code == 0`), mirroring the Rust client's
   `TouchyError::Proto`. Never returns a bogus handle `0`.
3. `TouchyClient._flush_stale_frames()` now holds `_rpc_lock` so the background
   event poller can't have its in-flight reply stolen by a flush.
4. `TouchyClient.file_save()` holds a new `_write_lock` for the whole sequence
   (and the retry) — the Python analogue of the Rust `write_lock`, so two
   threads can't abort each other's transaction.
5. `touchy_open()` reads board info through a new `_read_board_info()` helper: a
   `NOT_FOUND` or a default (`protocol_version == 0`) reply is flushed and
   retried once; a second one raises a clear "reply FIFO is desynchronised"
   error. A valid version with a 0×0 display is still allowed (headless board).

**Rust (`rust/touchy-pad/`)**

6. `Client::flush_stale_frames()` (crate-internal) — short-timeout reads until
   the FIFO is empty, under `rpc_lock`.
7. `Touchy::file_write_raw()` retries once on
   `TouchyError::Device { code: NOT_FOUND }` from `file_open_write`, after
   flushing — so the retry reads its *own* reply, not the orphaned one.

Not changed (deliberately): the firmware's stale-transaction abort is correct;
`NOT_FOUND` stays a legitimate answer for `EventConsume` (empty) and
`SetPreferences` (unknown screen) and is not turned into a generic retry signal;
`FileWrite` is not retried in place (a lost reply may mean the bytes were
appended — the whole-sequence retry after `FileClose(commit=False)` covers it).

## Files

| File | Change |
|---|---|
| `app/src/touchy_pad/api/client.py` | `_DesyncedReply`; `_write_lock`; `file_save` retry + `_save_bytes`; `file_open_write` payload check; `_flush_stale_frames` takes `_rpc_lock` |
| `app/src/touchy_pad/api/device.py` | `_read_board_info()`; `touchy_open()` uses it; imports `TouchyError` |
| `rust/touchy-pad/src/client.rs` | `Client::flush_stale_frames()`, `FLUSH_MAX_FRAMES`/`FLUSH_TIMEOUT` |
| `rust/touchy-pad/src/pad.rs` | `file_write_raw()` retry-on-`NOT_FOUND`; `#[cfg(test)] mod tests` |
| `app/tests/test_client.py` | `StaleThenLoopback` + 3 tests |
| `app/tests/test_api.py` | `DesyncFirstTransport` / `_ForeignOkForever` + 2 `touchy_open` tests |
| `CHANGELOG.md`, `docs/open-issues.md` | user-facing entries |

No `proto/*.proto` change → no `Widget.Version` / `ProtocolVersion` /
`PreferencesFile.Version` bump; no firmware, simulator or wire change.

## Phases

1. Python: `file_open_write` validation + `file_save` retry (`_save_bytes`), the
   write lock, `_flush_stale_frames` locking.
2. Python: `_read_board_info` in `touchy_open`.
3. Rust: `flush_stale_frames` + the `file_write_raw` retry.
4. Tests + docs; `just app-test`, `just app-lint`, `just rust-test`,
   `just rust-lint`.

## Testing strategy

* **Unit-level, no hardware** (the level the reporter recommended — the
  simulator never emits `NOT_FOUND` from `file_open_write`, so the sim cannot
  reproduce this):
  * `test_file_save_recovers_from_a_stale_not_found` — a leftover `NOT_FOUND`
    frame is read first; the save retries and commits its *own* handle.
  * `test_file_save_recovers_from_a_stale_ok_without_payload` — an `OK` with no
    handle is a desync too.
  * `test_file_save_does_not_retry_a_real_open_failure` — `IO_ERROR` surfaces at
    once, one open only (the fix has to stay narrow).
  * `test_touchy_open_retries_a_default_board_info_reply` — a foreign `OK` is
    flushed and the real board returned.
  * `test_touchy_open_rejects_repeated_default_board_info` — a permanently
    desynced FIFO fails with a clear message instead of a phantom 0×0 board.
  * Rust `pad::tests::file_write_raw_retries_a_stale_not_found_open` and
    `…_does_not_retry_a_real_open_failure` (mock transport, no poller).
* **Hardware (acceptance criteria from the report, still to run):** 20
  `kill -9`/restart cycles, each followed by a `screen_save()` + `screen_load()`
  that must succeed with zero `device returned NOT_FOUND (3)` from
  `FileOpenWrite`; handle numbers in `fs_open_write: stale transaction
  detected` must stop climbing.

## Risks / open questions

* The retry wraps the whole `_save_bytes()` sequence, so a `NOT_FOUND` arriving
  during `file_write`/`file_close` also retries. That is safe (the `finally`
  aborts with `commit=False` first, so the partial file is discarded and the
  re-upload is not a duplicate), and matches the reporter's patch.
* `_flush_stale_frames()` still stops at the first 40 ms timeout, so a *very*
  late leftover can survive one open. The RPC-level retries make that harmless;
  tightening the flush further would add latency to every `TouchyClient.open()`
  and was left out.
* The Rust crate's `Touchy::open()` still tolerates a desynced board-info reply
  (it logs and continues, and does not flush at open). The `file_write_raw`
  retry covers the reported failure; hardening Rust's open the way Python's was
  is a natural follow-up.
* Optional firmware defence-in-depth (age out an abandoned write transaction
  after N seconds) was not implemented — the host fix + the Stage-lb20 abort are
  sufficient.

