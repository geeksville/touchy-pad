// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad on-device filesystem implementation (stage 51).

#include "fs.h"

#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "lvgl.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

static const char *TAG = "fs";

// LittleFS mount + partition labels (match boards/<board>/partitions.csv).
static constexpr const char *FLASH_MOUNT     = "/littlefs";
static constexpr const char *FLASH_PARTITION = "storage";

// ---------------------------------------------------------------------------
// Path helpers shared by both Fs implementations
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
// Fs base-class helpers
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

// ===========================================================================
// FlashFs — littlefs / VFS
// ===========================================================================

FlashFs &FlashFs::instance()
{
    static FlashFs s;
    return s;
}

namespace {

std::string flash_full(const std::string &name)
{
    return std::string(FLASH_MOUNT) + "/" + strip_leading_slash(name);
}

// Create every intermediate directory along `full` (before the final
// component). Existing directories are silently accepted.
bool ensure_parents(const std::string &full)
{
    for (size_t i = 1; i < full.size(); i++) {
        if (full[i] != '/') continue;
        std::string segment = full.substr(0, i);
        if (mkdir(segment.c_str(), 0755) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "mkdir(%s) failed: %s", segment.c_str(), strerror(errno));
            return false;
        }
    }
    return true;
}

// Sweep any leftover `*.tmp.NNNN` files from previous crashed write
// transactions. Called once at boot.
void sweep_tmp(const std::string &name)
{
    // Walk via opendir so we can recurse without rebuilding a separate
    // generic walker; this only runs once at boot.
    std::string full = flash_full(name);
    DIR *dir = opendir(full.c_str());
    if (!dir) return;
    while (struct dirent *ent = readdir(dir)) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        std::string child = name.empty()
                              ? std::string(ent->d_name)
                              : (name + "/" + ent->d_name);
        if (ent->d_type == DT_DIR) {
            sweep_tmp(child);
        } else if (strstr(ent->d_name, ".tmp.")) {
            std::string p = flash_full(child);
            if (unlink(p.c_str()) == 0) {
                ESP_LOGI(TAG, "swept stale temp file %s", p.c_str());
            }
        }
    }
    closedir(dir);
}

}  // namespace

bool FlashFs::begin()
{
    if (_mounted) return true;

    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path              = FLASH_MOUNT;
    conf.partition_label        = FLASH_PARTITION;
    conf.format_if_mount_failed = true;
    conf.dont_mount             = false;

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return false;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(FLASH_PARTITION, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "mounted %s on %s: %u/%u bytes used",
                 FLASH_PARTITION, FLASH_MOUNT, (unsigned)used, (unsigned)total);
    }

    // Pre-create well-known top-level directories so callers don't
    // have to worry about ensure_parents() for the common case.
    for (const char *d : { "/littlefs/prefs", "/littlefs/host" }) {
        if (mkdir(d, 0755) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "mkdir(%s) failed: %s", d, strerror(errno));
        }
    }

    sweep_tmp("");
    _mounted = true;
    return true;
}

uint8_t *FlashFs::readBinary(const std::string &name, size_t *len_out)
{
    if (len_out) *len_out = 0;
    const std::string p = flash_full(name);

    struct stat st;
    if (stat(p.c_str(), &st) != 0) {
        ESP_LOGW(TAG, "stat(%s) failed: %s", p.c_str(), strerror(errno));
        return nullptr;
    }

    FILE *f = fopen(p.c_str(), "rb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed: %s", p.c_str(), strerror(errno));
        return nullptr;
    }

    const size_t len = static_cast<size_t>(st.st_size);
    auto *buf = new (std::nothrow) uint8_t[len];
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "out of memory reading %s (%u bytes)", p.c_str(), (unsigned)len);
        return nullptr;
    }
    const size_t got = fread(buf, 1, len, f);
    fclose(f);
    if (got != len) {
        ESP_LOGE(TAG, "short read on %s: %u/%u", p.c_str(), (unsigned)got, (unsigned)len);
        delete[] buf;
        return nullptr;
    }

    ESP_LOGI(TAG, "read %s (%u bytes)", p.c_str(), (unsigned)len);
    if (len_out) *len_out = len;
    return buf;
}

bool FlashFs::remove(const std::string &name)
{
    const std::string p = flash_full(name);
    if (unlink(p.c_str()) == 0) {
        ESP_LOGI(TAG, "removed %s", p.c_str());
        return true;
    }
    if (errno == ENOENT) return true;
    ESP_LOGE(TAG, "unlink(%s) failed: %s", p.c_str(), strerror(errno));
    return false;
}

bool FlashFs::removeTree(const std::string &name)
{
    const std::string p = flash_full(name);

    DIR *dir = opendir(p.c_str());
    if (!dir) {
        if (errno == ENOENT) return true;            // already gone
        if (unlink(p.c_str()) == 0 || errno == ENOENT) return true;
        ESP_LOGE(TAG, "removeTree(%s): %s", p.c_str(), strerror(errno));
        return false;
    }

    bool ok = true;
    while (struct dirent *ent = readdir(dir)) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        std::string child = name.empty()
                              ? std::string(ent->d_name)
                              : (name + "/" + ent->d_name);
        if (!removeTree(child)) ok = false;
    }
    closedir(dir);

    // An empty `name` means the root of the FS — never `rmdir` that.
    if (!name.empty()) {
        if (rmdir(p.c_str()) != 0 && errno != ENOENT) {
            ESP_LOGE(TAG, "rmdir(%s) failed: %s", p.c_str(), strerror(errno));
            ok = false;
        } else {
            ESP_LOGI(TAG, "removed dir %s", p.c_str());
        }
    }
    return ok;
}

bool FlashFs::list(const std::string &dirname, const ListCb &cb)
{
    const std::string p = flash_full(dirname);
    DIR *dir = opendir(p.c_str());
    if (!dir) {
        ESP_LOGW(TAG, "opendir(%s) failed: %s", p.c_str(), strerror(errno));
        return false;
    }
    while (struct dirent *ent = readdir(dir)) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        const bool is_dir = (ent->d_type == DT_DIR);
        if (!cb(ent->d_name, is_dir)) break;
    }
    closedir(dir);
    return true;
}

// ---- FlashFs streaming write ----------------------------------------------
//
// Strategy: write the streamed bytes into `<path>.tmp.<handle>` and on
// commit `rename(tmp, final)`. On abort or boot-time sweep the tmp
// file is unlinked.

namespace {

struct FlashWriteTxn {
    uint32_t    handle = 0;
    std::string final_path;   // FS-relative, no /littlefs prefix
    std::string tmp_path;     // FS-relative
    FILE       *fp = nullptr;
};

FlashWriteTxn g_flash_txn;
uint32_t      g_next_handle = 0;

}  // namespace

uint32_t FlashFs::openWrite(const std::string &name)
{
    if (g_flash_txn.handle != 0) {
        ESP_LOGE(TAG, "openWrite(%s): another flash transaction is in flight",
                 name.c_str());
        return 0;
    }
    const std::string rel = strip_leading_slash(name);
    if (rel.empty()) {
        ESP_LOGE(TAG, "openWrite: empty path");
        return 0;
    }
    const std::string full_final = flash_full(rel);
    if (!ensure_parents(full_final)) return 0;

    if (++g_next_handle == 0) g_next_handle = 1;
    const uint32_t handle = g_next_handle;

    char suffix[32];
    snprintf(suffix, sizeof(suffix), ".tmp.%u", (unsigned)handle);
    const std::string tmp_rel  = rel + suffix;
    const std::string tmp_full = flash_full(tmp_rel);

    FILE *fp = fopen(tmp_full.c_str(), "wb");
    if (!fp) {
        ESP_LOGE(TAG, "fopen(%s, w) failed: %s", tmp_full.c_str(), strerror(errno));
        return 0;
    }
    g_flash_txn = { handle, rel, tmp_rel, fp };
    return handle;
}

bool FlashFs::appendWrite(uint32_t handle, const uint8_t *data, size_t len)
{
    if (handle == 0 || g_flash_txn.handle != handle) return false;
    if (len == 0) return true;
    size_t w = fwrite(data, 1, len, g_flash_txn.fp);
    if (w != len) {
        ESP_LOGE(TAG, "fwrite short on %s: %u/%u",
                 g_flash_txn.tmp_path.c_str(), (unsigned)w, (unsigned)len);
        return false;
    }
    return true;
}

bool FlashFs::closeWrite(uint32_t handle, bool commit)
{
    if (handle == 0 || g_flash_txn.handle != handle) return false;
    bool ok = (fclose(g_flash_txn.fp) == 0);
    g_flash_txn.fp = nullptr;
    const std::string tmp_full   = flash_full(g_flash_txn.tmp_path);
    const std::string final_full = flash_full(g_flash_txn.final_path);
    if (commit && ok) {
        // POSIX rename overwrites the destination atomically.
        if (::rename(tmp_full.c_str(), final_full.c_str()) != 0) {
            ESP_LOGE(TAG, "rename(%s -> %s) failed: %s",
                     tmp_full.c_str(), final_full.c_str(), strerror(errno));
            unlink(tmp_full.c_str());
            ok = false;
        } else {
            ESP_LOGI(TAG, "committed %s", final_full.c_str());
        }
    } else {
        unlink(tmp_full.c_str());
        ESP_LOGI(TAG, "aborted %s", tmp_full.c_str());
    }
    g_flash_txn = {};
    return ok;
}

// ===========================================================================
// RamFs — PSRAM-backed hashmap, used for transient image assets
// ===========================================================================

namespace {

struct RamEntry {
    // The actual file bytes. Allocated via `heap_caps_malloc(MALLOC_CAP_SPIRAM)`
    // so the RAM filesystem doesn't compete with internal SRAM for
    // (potentially large) image buffers.
    uint8_t *data = nullptr;
    size_t   len  = 0;
};

struct RamWriteTxn {
    uint32_t              handle = 0;
    std::string           path;
    std::vector<uint8_t>  staging;   // built up across appendWrite calls
};

// Internal-only state. Kept in an anonymous namespace so its layout
// doesn't bleed into other translation units; access is exclusively
// through the RamFs accessors.
std::unordered_map<std::string, RamEntry> g_ram_files;
RamWriteTxn                                g_ram_txn;
uint32_t                                   g_ram_next_handle = 0;

void ram_free_entry(RamEntry &e)
{
    if (e.data) {
        heap_caps_free(e.data);
        e.data = nullptr;
    }
    e.len = 0;
}

}  // namespace

RamFs &RamFs::instance()
{
    static RamFs s;
    return s;
}

bool RamFs::begin()
{
    if (_ready) return true;
    _ready = true;
    ESP_LOGI(TAG, "RamFs ready");
    return true;
}

const uint8_t *RamFs::peek(const std::string &path, size_t *len_out) const
{
    const std::string rel = strip_leading_slash(path);
    auto it = g_ram_files.find(rel);
    if (it == g_ram_files.end()) {
        if (len_out) *len_out = 0;
        return nullptr;
    }
    if (len_out) *len_out = it->second.len;
    return it->second.data;
}

uint8_t *RamFs::readBinary(const std::string &name, size_t *len_out)
{
    if (len_out) *len_out = 0;
    size_t n = 0;
    const uint8_t *src = peek(name, &n);
    if (!src) return nullptr;
    auto *buf = new (std::nothrow) uint8_t[n];
    if (!buf) return nullptr;
    memcpy(buf, src, n);
    if (len_out) *len_out = n;
    return buf;
}

bool RamFs::remove(const std::string &name)
{
    const std::string rel = strip_leading_slash(name);
    auto it = g_ram_files.find(rel);
    if (it == g_ram_files.end()) return true;
    ram_free_entry(it->second);
    g_ram_files.erase(it);
    ESP_LOGI(TAG, "removed R:%s", rel.c_str());
    return true;
}

bool RamFs::removeTree(const std::string &name)
{
    const std::string rel = strip_leading_slash(name);
    // Empty prefix = wipe everything.
    if (rel.empty()) {
        for (auto &kv : g_ram_files) ram_free_entry(kv.second);
        g_ram_files.clear();
        ESP_LOGI(TAG, "RamFs wiped (all files removed)");
        return true;
    }
    const std::string prefix = rel + "/";
    for (auto it = g_ram_files.begin(); it != g_ram_files.end(); ) {
        // Match either the exact path or any descendant.
        if (it->first == rel ||
            it->first.compare(0, prefix.size(), prefix) == 0) {
            ram_free_entry(it->second);
            it = g_ram_files.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

bool RamFs::list(const std::string &dirname, const ListCb &cb)
{
    const std::string rel = strip_leading_slash(dirname);
    const std::string prefix = rel.empty() ? std::string() : (rel + "/");
    // Track which immediate sub-directories we've already reported,
    // since the map is flat and may have several entries under the
    // same logical directory.
    std::unordered_map<std::string, bool> seen_dirs;
    for (const auto &kv : g_ram_files) {
        const std::string &full = kv.first;
        if (prefix.empty()
            ? false
            : full.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        const std::string rest = full.substr(prefix.size());
        if (rest.empty()) continue;
        size_t slash = rest.find('/');
        if (slash == std::string::npos) {
            if (!cb(rest, false)) return true;
        } else {
            std::string sub = rest.substr(0, slash);
            if (seen_dirs.insert({sub, true}).second) {
                if (!cb(sub, true)) return true;
            }
        }
    }
    return true;
}

uint32_t RamFs::openWrite(const std::string &name)
{
    if (g_ram_txn.handle != 0) {
        ESP_LOGE(TAG, "openWrite(R:%s): another RAM txn in flight", name.c_str());
        return 0;
    }
    const std::string rel = strip_leading_slash(name);
    if (rel.empty()) return 0;
    if (++g_ram_next_handle == 0) g_ram_next_handle = 1;
    g_ram_txn.handle = g_ram_next_handle;
    g_ram_txn.path   = rel;
    g_ram_txn.staging.clear();
    return g_ram_txn.handle;
}

bool RamFs::appendWrite(uint32_t handle, const uint8_t *data, size_t len)
{
    if (handle == 0 || g_ram_txn.handle != handle) return false;
    if (len == 0) return true;
    g_ram_txn.staging.insert(g_ram_txn.staging.end(), data, data + len);
    return true;
}

bool RamFs::closeWrite(uint32_t handle, bool commit)
{
    if (handle == 0 || g_ram_txn.handle != handle) return false;
    bool ok = true;
    if (commit) {
        // Replace any existing entry.
        auto it = g_ram_files.find(g_ram_txn.path);
        if (it != g_ram_files.end()) ram_free_entry(it->second);
        RamEntry entry;
        entry.len = g_ram_txn.staging.size();
        if (entry.len) {
            // Prefer PSRAM (large allocations would otherwise starve the
            // internal heap LVGL needs for draw buffers / display).
            entry.data = static_cast<uint8_t *>(
                heap_caps_malloc(entry.len, MALLOC_CAP_SPIRAM));
            if (!entry.data) {
                // Fall back to whatever heap is left.
                entry.data = static_cast<uint8_t *>(malloc(entry.len));
            }
            if (!entry.data) {
                ESP_LOGE(TAG, "RamFs OOM committing %s (%u bytes)",
                         g_ram_txn.path.c_str(), (unsigned)entry.len);
                ok = false;
            } else {
                memcpy(entry.data, g_ram_txn.staging.data(), entry.len);
            }
        }
        if (ok) {
            g_ram_files[g_ram_txn.path] = entry;
            ESP_LOGI(TAG, "committed R:%s (%u bytes)",
                     g_ram_txn.path.c_str(), (unsigned)entry.len);
        }
    } else {
        ESP_LOGI(TAG, "aborted R:%s", g_ram_txn.path.c_str());
    }
    g_ram_txn = {};
    return ok;
}

// ===========================================================================
// Drive-letter routing
// ===========================================================================

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

// ---------------------------------------------------------------------------
// Cross-FS write-transaction registry. Tracks which Fs owns the current
// handle so subsequent FileWrite/FileClose commands can be routed
// without re-parsing the path.
// ---------------------------------------------------------------------------

namespace {
Fs *g_active_write_fs = nullptr;
}

uint32_t fs_open_write(const std::string &full_path)
{
    if (g_active_write_fs) {
        ESP_LOGE(TAG, "fs_open_write: another transaction is already open");
        return 0;
    }
    std::string rest;
    Fs *fs = fs_resolve(full_path, &rest);
    if (!fs) {
        ESP_LOGE(TAG, "fs_open_write: bad path %s", full_path.c_str());
        return 0;
    }
    uint32_t h = fs->openWrite(rest);
    if (h) g_active_write_fs = fs;
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
    g_active_write_fs = nullptr;
    return ok;
}

void fs_abort_open_transaction()
{
    if (!g_active_write_fs) return;
    // The active fs owns its own handle bookkeeping; passing the value
    // it stored is impossible from here without a getter. Force-abort
    // by tearing each known fs's transaction down regardless.
    if (g_flash_txn.handle) FlashFs::instance().closeWrite(g_flash_txn.handle, false);
    if (g_ram_txn.handle)   RamFs::instance().closeWrite(g_ram_txn.handle, false);
    g_active_write_fs = nullptr;
}

// ===========================================================================
// LVGL FS driver for the "R:" RAM drive
// ===========================================================================
//
// Read-only — host code writes via the normal openWrite/appendWrite/
// closeWrite path. LVGL only ever needs read access for image / font
// loading.

namespace {

struct RamLvFile {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
};

void *ramlv_open(lv_fs_drv_t *, const char *path, lv_fs_mode_t mode)
{
    if ((mode & LV_FS_MODE_WR) != 0) return nullptr;  // read-only
    if (!path) return nullptr;
    // LVGL hands us the path with the leading '/' it inserts itself in
    // some code paths; strip it.
    const char *p = path;
    while (*p == '/') p++;
    size_t len = 0;
    const uint8_t *bytes = RamFs::instance().peek(p, &len);
    if (!bytes) return nullptr;
    auto *fh = new (std::nothrow) RamLvFile{bytes, len, 0};
    return fh;
}

lv_fs_res_t ramlv_close(lv_fs_drv_t *, void *file_p)
{
    delete static_cast<RamLvFile *>(file_p);
    return LV_FS_RES_OK;
}

lv_fs_res_t ramlv_read(lv_fs_drv_t *, void *file_p, void *buf,
                       uint32_t btr, uint32_t *br)
{
    auto *fh = static_cast<RamLvFile *>(file_p);
    size_t left = fh->len - fh->pos;
    size_t got  = btr < left ? btr : left;
    memcpy(buf, fh->data + fh->pos, got);
    fh->pos += got;
    if (br) *br = (uint32_t)got;
    return LV_FS_RES_OK;
}

lv_fs_res_t ramlv_seek(lv_fs_drv_t *, void *file_p, uint32_t pos,
                       lv_fs_whence_t whence)
{
    auto *fh = static_cast<RamLvFile *>(file_p);
    size_t newpos = 0;
    switch (whence) {
        case LV_FS_SEEK_SET: newpos = pos; break;
        case LV_FS_SEEK_CUR: newpos = fh->pos + pos; break;
        case LV_FS_SEEK_END: newpos = fh->len + pos; break;
        default: return LV_FS_RES_INV_PARAM;
    }
    if (newpos > fh->len) newpos = fh->len;
    fh->pos = newpos;
    return LV_FS_RES_OK;
}

lv_fs_res_t ramlv_tell(lv_fs_drv_t *, void *file_p, uint32_t *pos_p)
{
    auto *fh = static_cast<RamLvFile *>(file_p);
    if (pos_p) *pos_p = (uint32_t)fh->pos;
    return LV_FS_RES_OK;
}

lv_fs_drv_t g_ramlv_drv;

void ramfs_register_lvgl()
{
    lv_fs_drv_init(&g_ramlv_drv);
    g_ramlv_drv.letter   = 'R';
    g_ramlv_drv.open_cb  = ramlv_open;
    g_ramlv_drv.close_cb = ramlv_close;
    g_ramlv_drv.read_cb  = ramlv_read;
    g_ramlv_drv.seek_cb  = ramlv_seek;
    g_ramlv_drv.tell_cb  = ramlv_tell;
    lv_fs_drv_register(&g_ramlv_drv);
    ESP_LOGI(TAG, "registered LVGL 'R:' filesystem driver");
}

}  // namespace

// ===========================================================================
// One-shot init
// ===========================================================================

void fs_init()
{
    FlashFs::instance().begin();
    RamFs::instance().begin();
    ramfs_register_lvgl();
}
