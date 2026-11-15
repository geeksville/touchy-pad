# Flash write transaction wedged after a handle mismatch

**Status:** open — found during a fork-review (jellydn), reproduced by reading the
code. Firmware-only fix, **being implemented upstream by the maintainer** (no PR
requested from the reporter).

## Goal / summary

A `FileCloseCmd` carrying the *wrong* handle leaves the device unable to write
any `F:` file (`IO_ERROR`) until it is reset. The reporter worked around it
host-side by retrying the same save on `R:` — we should fix the firmware
instead, because the device is left in a permanently wedged state.

## Root cause (verified)

`firmware/main/fs/fs.cpp`:

| Line | Code | Effect |
|---|---|---|
| 161-165 | `fs_close_write()` logs `handle mismatch` but still delegates `closeWrite(handle, commit)` with the **caller's** bogus handle | `FlashFs::closeWrite()` (flash_fs.cpp:316) returns `false` immediately on `handle != g_txn.handle`, **before** `fclose()` / `g_txn = {}` |
| 167-169 | then unconditionally clears `g_active_write_fs/handle/path` | the global registry now believes there is no transaction |
| ~116-124 | `fs_open_write()` only aborts a stale transaction when `g_active_write_fs` is non-null | it is null, so the abort does not run |
| 271-275 | `FlashFs::openWrite()` refuses while `g_txn.handle != 0` | **every** subsequent `F:` open returns 0 → `IO_ERROR` forever |

Also leaked: the stale `g_txn.fp` (an open `FILE*`) and its `<path>.tmp.<handle>`
file, which the boot-time sweep is the only thing that reclaims.

The mismatch itself comes from a client closing with a stale/replayed handle
(e.g. a second session re-using a handle from a failed open). Writes are
transport-independent, so any link can trigger it.

## Approach

1. **Give `Fs` an unconditional abort.** Add
   `virtual void abortWrite() {}` to `firmware/main/fs/fs.h`; implement it in
   `flash_fs.cpp` (if `g_txn.handle`, `fclose(g_txn.fp)` if set, `unlink` the tmp
   file, `g_txn = {}`) and `ram_fs.cpp` (free the buffer, clear `g_txn`).
   Default no-op keeps any future FS source-compatible.
2. **Call it on mismatch.** In `fs_close_write()`, when `handle !=
   g_active_write_handle`, `g_active_write_fs->abortWrite()` instead of (or
   before) delegating with the bad handle, then clear the registry as today and
   still return `false` (the caller *did* send a bad handle). This is the same
   policy as `fs_abort_open_transaction()`, which already uses the *active*
   handle rather than the caller's.
3. **Belt and braces at open time.** In `fs_open_write()`, after resolving the
   `Fs`, also call `abortWrite()` when it reports a leftover transaction, so a
   device that was wedged by an older build (or by any future path that clears
   the registry without aborting) recovers on the next write instead of needing
   a reset. Keep the existing `g_active_write_fs` stale-abort log line.
4. No wire change, no proto change, no host-side change. The `R:` fallback
   should then be unnecessary (we can decline it upstream, or keep it as a
   diagnostic only).

## Files

| File | Change |
|---|---|
| `firmware/main/fs/fs.h` | `virtual void abortWrite() {}` |
| `firmware/main/fs/fs.cpp` | mismatch path aborts the underlying txn; open path recovers a leftover txn |
| `firmware/main/fs/flash_fs.h`, `flash_fs.cpp` | `abortWrite()` override |
| `firmware/main/fs/ram_fs.h`, `ram_fs.cpp` | `abortWrite()` override |

## Phases

1. `abortWrite()` + the two call sites (the fix).
2. Verification + a note in `docs/open-issues.md` if any related entry exists.

## Testing strategy

No firmware unit tests. Verification:

* `just firmware-build` (default board `jc4827w543`); the CYD/RAM-only boards
  share `Fs` but exercise `RamFs`, so build one of those too
  (`just firmware-reconfigure esp32_2432s028rv3 && just firmware-build`).
* Hardware/scripted check with a raw client (or a patched `TouchyClient`):
  1. `FileOpenWrite F:host/probe.txt` → get handle *h*.
  2. `FileClose h+1` (deliberately wrong) → expect `IO_ERROR`.
  3. Immediately `FileOpenWrite F:host/probe.txt` again → must **succeed**
     (pre-fix: `IO_ERROR` until reset).
  4. Commit a body and read it back to prove the bytes landed.
* Repeat against `R:` to confirm `RamFs::abortWrite()`.
* Optional host-side pytest: none of this is reachable from the simulator,
  which has no `IO_ERROR`/handle model — note that in the plan outcome rather
  than adding a fake test.

## Risks / open questions

* `fs_close_write()` currently returns `false` on mismatch; keep that (a bad
  handle should not look successful) and let the client retry the whole
  open/write/close cycle.
* Should the device *also* be tolerant of a mismatch by committing with the
  active handle? No — that would silently commit a file the caller did not
  intend. Abort is the safer semantics.
* The boot-time `.tmp.*` sweep should still be verified to clean up a tmp file
  left by a power loss mid-write.