// SPDX-License-Identifier: Apache-2.0
//
// Touchy-Pad on-device filesystem wrapper (stage 14).
//
// Wraps esp_littlefs / VFS so the rest of the firmware doesn't need to
// know which underlying FS is in use. Mounts the `storage` partition at
// /littlefs and pre-creates the two well-known top-level directories:
//
//   /prefs     — per-device preferences and state (not yet populated)
//   /from_host — file tree pushed by the host PC via FileSave/FileReset
//
// All `name` arguments are FS-root-relative paths *without* the mount
// prefix — e.g. `"from_host/screens/home.xml"`. Any missing parent
// directories along the path are created automatically.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

class Fs {
public:
    // Mount the storage partition at /littlefs, formatting on first boot
    // and creating /prefs and /from_host if missing. Also registers an
    // LVGL filesystem driver under identifier letter 'F' that maps
    // `"F:path/to/foo"` to `/littlefs/from_host/path/to/foo` (stage 15),
    // so XML/image loaders can resolve host-uploaded assets directly.
    // Returns true on success. Subsequent calls are no-ops.
    bool begin();

    // Read an entire file as a UTF-8 string. Returns empty string on error.
    std::string readText(const std::string &name);

    // Read an entire file as a freshly-allocated byte array. Caller must
    // `delete[]` the returned pointer when done. Sets *len_out to the file
    // size in bytes. Returns nullptr on error.
    uint8_t *readBinary(const std::string &name, size_t *len_out);

    // Write (or replace) a file with the given byte payload. Creates any
    // missing parent directories. Returns true on success.
    bool writeFile(const std::string &name, const uint8_t *data, size_t len);

    // Delete a single file. Returns true if the file was removed (or did
    // not exist). Directories are *not* deleted by this call.
    bool remove(const std::string &name);

    // Recursively remove a directory tree (every file and sub-directory
    // under `name`). The directory itself is removed too. Returns true
    // on success.
    bool removeTree(const std::string &name);

    // Iterate over entries directly inside `dirname`. The callback is
    // invoked once per child with the *relative* entry name (no path
    // separators) and a flag indicating whether the entry is a directory.
    // Returning false from the callback stops iteration early. Returns
    // true if the directory was opened successfully.
    using ListCb = std::function<bool(const std::string &name, bool is_dir)>;
    bool list(const std::string &dirname, const ListCb &cb);

    // Singleton accessor — the firmware has a single global filesystem.
    static Fs &instance();

private:
    bool _mounted = false;
};
