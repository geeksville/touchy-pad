// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad on-device filesystem abstraction (stage 51).
//
// Two concrete filesystems are exposed to host code under drive-letter
// prefixes:
//
//   F:   `FlashFs` — esp_littlefs on the on-board NOR partition.
//                    Persistent across reboots. Mounted at /littlefs;
//                    LVGL's POSIX FS bridge (LV_USE_FS_POSIX) maps the
//                    "F:" drive directly onto that mount point.
//
//   R:   `RamFs`   — PSRAM-backed hash map. Survives only until reboot;
//                    intended for images the host pushes on connect to
//                    avoid wearing the flash. Registered as a custom
//                    LVGL FS driver (`lv_fs_drv_register`) so widgets
//                    can `lv_image_set_src("R:host/foo.bin")`.
//
// Wire paths are always prefixed (e.g. `"F:host/screens/home.pb"`).
// Internal FS-relative paths (passed to the `Fs::*` virtual methods)
// have the drive letter / colon stripped and no leading slash —
// e.g. `"host/screens/home.pb"`.
//
// Streaming writes follow an open / append* / close protocol so the
// host can transfer files larger than a single USB bulk frame. Only
// one in-flight transaction is supported at a time across both drives;
// the registry-level `fs_open_write` enforces that.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

class Fs {
public:
    virtual ~Fs() = default;

    // The single ASCII drive letter ('F', 'R', ...) under which this
    // filesystem is registered.
    virtual char letter() const = 0;

    // One-time setup. Safe to call multiple times. Returns true on
    // success.
    virtual bool begin() = 0;

    // Read an entire file as a freshly-allocated byte array. Caller
    // must `delete[]` the returned pointer when done. Sets *len_out to
    // the file size in bytes. Returns nullptr on error.
    virtual uint8_t *readBinary(const std::string &path, size_t *len_out) = 0;

    // Delete a single file. Returns true if the file was removed (or
    // didn't exist). Does not remove directories.
    virtual bool remove(const std::string &path) = 0;

    // Recursively remove a directory tree (every file and sub-
    // directory under `path`), plus the directory itself. An empty
    // `path` wipes the root of this filesystem. Returns true on
    // success.
    virtual bool removeTree(const std::string &path) = 0;

    // Iterate over entries directly inside `dirname`. The callback is
    // invoked once per child with the *relative* entry name (no path
    // separators) and a flag indicating whether the entry is a
    // directory. Returning false from the callback stops iteration
    // early.
    using ListCb = std::function<bool(const std::string &name, bool is_dir)>;
    virtual bool list(const std::string &dirname, const ListCb &cb) = 0;

    // ---- Streaming writes -------------------------------------------
    //
    // openWrite: begin a new transaction. Returns an opaque non-zero
    // handle on success, or 0 on failure (already an open transaction,
    // OOM, parent directory creation failed, ...). Subsequent
    // appendWrite/closeWrite calls must echo back the same handle.
    //
    // closeWrite(handle, commit=true): atomically rename the staged
    // file into place and free the staging buffer/temp file.
    // closeWrite(handle, commit=false): discard the staging data.
    // Either way the handle is invalidated.
    virtual uint32_t openWrite(const std::string &path)             = 0;
    virtual bool     appendWrite(uint32_t handle, const uint8_t *data, size_t len) = 0;
    virtual bool     closeWrite(uint32_t handle, bool commit)       = 0;

    // ---- Convenience helpers ----------------------------------------
    //
    // Write a whole buffer atomically via open/append/close. Returns
    // true on success.
    bool writeFile(const std::string &path, const uint8_t *data, size_t len);

    // Read an entire file as a UTF-8 string. Returns "" on error.
    std::string readText(const std::string &path);
};

// ---------------------------------------------------------------------------
// Convenience: pull in the concrete filesystem types so a single
// `#include "fs/fs.h"` gives callers access to FlashFs and RamFs.
// (flash_fs.h / ram_fs.h re-include this header; #pragma once makes
// that cycle safe.)
// ---------------------------------------------------------------------------
#include "flash_fs.h"
#include "ram_fs.h"

// ---------------------------------------------------------------------------
// Drive-letter routing
// ---------------------------------------------------------------------------

// Parse a wire path like "F:host/foo.bin" into ('F', "host/foo.bin").
// Returns false if `full` lacks a valid <letter>: prefix.
bool fs_parse_drive(const std::string &full, char *drive_out, std::string *rest_out);

// Look up the filesystem registered under `letter`, or nullptr.
Fs *fs_for_drive(char letter);

// Convenience: parse `full` and return both the Fs and the FS-relative
// path. Returns nullptr (and leaves *rest_out untouched) if the path
// lacks a drive prefix or the drive is unknown.
Fs *fs_resolve(const std::string &full, std::string *rest_out);

// ---------------------------------------------------------------------------
// Cross-FS write-transaction registry.
// ---------------------------------------------------------------------------
//
// Only one in-flight write transaction is supported across the whole
// device at any time. These wrappers parse the drive letter, route to
// the right Fs, and remember which Fs owns the active handle.

uint32_t fs_open_write(const std::string &full_path);
bool     fs_append_write(uint32_t handle, const uint8_t *data, size_t len);
bool     fs_close_write(uint32_t handle, bool commit);

// Cancel any currently-open transaction (used when the host
// disconnects mid-stream). Safe to call when none is open.
void     fs_abort_open_transaction();

// ---------------------------------------------------------------------------
// One-shot bring-up. Mounts both filesystems and registers the RamFs
// LVGL driver.
// ---------------------------------------------------------------------------
void fs_init();
