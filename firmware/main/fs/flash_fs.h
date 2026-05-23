// SPDX-License-Identifier: GPL-3.0-or-later
//
// FlashFs — LittleFS-on-NOR filesystem (drive letter 'F:').
// Persistent across reboots. Mounted at /littlefs; LVGL's POSIX FS
// bridge (LV_USE_FS_POSIX) maps the "F:" drive directly onto that
// mount point.

#pragma once

#include "fs.h"

class FlashFs : public Fs {
public:
    static FlashFs &instance();
    char letter() const override { return 'F'; }
    bool begin() override;

    uint8_t *readBinary(const std::string &path, size_t *len_out) override;
    bool     remove(const std::string &path) override;
    bool     removeTree(const std::string &path) override;
    bool     list(const std::string &dirname, const ListCb &cb) override;
    uint32_t openWrite(const std::string &path) override;
    bool     appendWrite(uint32_t handle, const uint8_t *data, size_t len) override;
    bool     closeWrite(uint32_t handle, bool commit) override;

private:
    FlashFs() = default;
    bool _mounted = false;
};
