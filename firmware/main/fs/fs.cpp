// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fs base-class helpers, drive-letter routing, and one-shot init.

#include "fs.h"
#include "tc_tag.h"

#include "esp_log.h"

#include <string>

static const char *TAG = TOUCHY_TAG("fs");

// ---------------------------------------------------------------------------
// Shared path helper
// ---------------------------------------------------------------------------

namespace {

// Strip leading '/' (defensive — wire paths don't include one but
// internal callers occasionally do).
std::string strip_leading_slash(const std::string &p)
{
    size_t start = 0;
    while (start < p.size() && p[start] == '/') start++;
    return p.substr(start);
}

}  // namespace

// ---------------------------------------------------------------------------
// Fs base-class convenience helpers
// ---------------------------------------------------------------------------

bool Fs::writeFile(const std::string &path, const uint8_t *data, size_t len)
{
    uint32_t h = openWrite(path);
    if (!h) return false;
    bool ok = len ? appendWrite(h, data, len) : true;
    if (!closeWrite(h, ok)) ok = false;
    return ok;
}

std::string Fs::readText(const std::string &path)
{
    size_t len = 0;
    uint8_t *buf = readBinary(path, &len);
    if (!buf) return {};
    std::string out(reinterpret_cast<const char *>(buf), len);
    delete[] buf;
    return out;
}

// ---------------------------------------------------------------------------
// Drive-letter routing
// ---------------------------------------------------------------------------

bool fs_parse_drive(const std::string &full, char *drive_out, std::string *rest_out)
{
    if (full.size() < 2 || full[1] != ':') return false;
    char c = full[0];
    // Only accept alphabetic drive letters so a stray colon in a path
    // doesn't look like a drive prefix.
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
    if (drive_out) *drive_out = (char)(c & ~0x20);   // upper-case
    if (rest_out)  *rest_out  = strip_leading_slash(full.substr(2));
    return true;
}

Fs *fs_for_drive(char letter)
{
    switch (letter & ~0x20) {
        case 'F': return &FlashFs::instance();
        case 'R': return &RamFs::instance();
        default:  return nullptr;
    }
}

Fs *fs_resolve(const std::string &full, std::string *rest_out)
{
    char drive = 0;
    std::string rest;
    if (!fs_parse_drive(full, &drive, &rest)) return nullptr;
    Fs *fs = fs_for_drive(drive);
    if (!fs) return nullptr;
    if (rest_out) *rest_out = rest;
    return fs;
}

const uint8_t *fs_peek(const std::string &full_path, size_t *len_out)
{
    std::string rest;
    Fs *fs = fs_resolve(full_path, &rest);
    if (!fs) {
        if (len_out) *len_out = 0;
        return nullptr;
    }
    return fs->peek(rest, len_out);
}

// ---------------------------------------------------------------------------
// Cross-FS write-transaction registry. Tracks which Fs owns the
// current handle so subsequent FileWrite/FileClose commands can be
// routed without re-parsing the path.
// ---------------------------------------------------------------------------

namespace {
Fs       *g_active_write_fs     = nullptr;
uint32_t  g_active_write_handle = 0;
}

uint32_t fs_open_write(const std::string &full_path)
{
    if (g_active_write_fs) {
        // A stale transaction from a dropped connection. The host is starting
        // a new write, so the old one is definitively abandoned — abort it.
        ESP_LOGW(TAG, "fs_open_write: stale transaction detected, aborting before opening %s",
                 full_path.c_str());
        fs_abort_open_transaction();
    }
    std::string rest;
    Fs *fs = fs_resolve(full_path, &rest);
    if (!fs) {
        ESP_LOGE(TAG, "fs_open_write: bad path %s", full_path.c_str());
        return 0;
    }
    uint32_t h = fs->openWrite(rest);
    if (h) {
        g_active_write_fs     = fs;
        g_active_write_handle = h;
    }
    return h;
}

bool fs_append_write(uint32_t handle, const uint8_t *data, size_t len)
{
    if (!g_active_write_fs) return false;
    return g_active_write_fs->appendWrite(handle, data, len);
}

bool fs_close_write(uint32_t handle, bool commit)
{
    if (!g_active_write_fs) return false;
    bool ok = g_active_write_fs->closeWrite(handle, commit);
    g_active_write_fs     = nullptr;
    g_active_write_handle = 0;
    return ok;
}

void fs_abort_open_transaction()
{
    if (!g_active_write_fs || !g_active_write_handle) return;
    g_active_write_fs->closeWrite(g_active_write_handle, false);
    g_active_write_fs     = nullptr;
    g_active_write_handle = 0;
}

// ---------------------------------------------------------------------------
// One-shot bring-up
// ---------------------------------------------------------------------------

void fs_init()
{
    FlashFs::instance().begin();
    RamFs::instance().begin();
}

void fs_register_lvgl_drivers()
{
    // lv_init() clears LVGL's FS driver linked list, so we must
    // (re)register after display_init() has run lv_init().
    RamFs::instance().registerLvglDriver();
}
