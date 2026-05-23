// SPDX-License-Identifier: GPL-3.0-or-later
//
// RamFs — PSRAM-backed hash map filesystem (drive letter 'R:').
// Volatile — contents are lost on reboot. Intended for assets the host
// pushes on every connect (e.g. StreamDeck-style key icons) to avoid
// wearing the flash. Registered as a read-only LVGL FS driver so
// widgets can use "R:host/..." paths directly.

#pragma once

#include "fs.h"

class RamFs : public Fs {
public:
    static RamFs &instance();
    char letter() const override { return 'R'; }
    bool begin() override;

    uint8_t *readBinary(const std::string &path, size_t *len_out) override;
    bool     remove(const std::string &path) override;
    bool     removeTree(const std::string &path) override;
    bool     list(const std::string &dirname, const ListCb &cb) override;
    uint32_t openWrite(const std::string &path) override;
    bool     appendWrite(uint32_t handle, const uint8_t *data, size_t len) override;
    bool     closeWrite(uint32_t handle, bool commit) override;

    // Direct read of an existing file's bytes — used by the LVGL FS
    // driver to avoid a copy. Returns the in-place pointer (still
    // owned by RamFs) plus its length, or nullptr if not present.
    const uint8_t *peek(const std::string &path, size_t *len_out) const;

private:
    RamFs() = default;
    bool _ready = false;
};
