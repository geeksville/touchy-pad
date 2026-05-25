// SPDX-License-Identifier: GPL-3.0-or-later
//
// RamFs implementation — PSRAM-backed hash map + LVGL 'R:' driver.

#include "ram_fs.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#if CONFIG_SPIRAM
#include "esp_cache.h"
#endif

#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

static const char *TAG = "fs.ram";

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// Strip leading '/' (defensive).
std::string strip_leading_slash(const std::string &p)
{
    size_t start = 0;
    while (start < p.size() && p[start] == '/') start++;
    return p.substr(start);
}

// ---- RamFs storage -------------------------------------------------------

struct RamEntry {
    // Bytes allocated via heap_caps_malloc(MALLOC_CAP_SPIRAM) so large
    // image buffers don't compete with internal SRAM for LVGL draw
    // memory.
    uint8_t *data = nullptr;
    size_t   len  = 0;
};

struct RamWriteTxn {
    uint32_t              handle = 0;
    std::string           path;
    std::vector<uint8_t>  staging;   // built up across appendWrite calls
};

std::unordered_map<std::string, RamEntry> g_files;
RamWriteTxn                                g_txn;
uint32_t                                   g_next_handle = 0;

void free_entry(RamEntry &e)
{
    if (e.data) {
        heap_caps_free(e.data);
        e.data = nullptr;
    }
    e.len = 0;
}

// ---- LVGL 'R:' driver ----------------------------------------------------
//
// Read-only — host code writes via openWrite/appendWrite/closeWrite.
// LVGL only ever needs read access for image / font loading.

struct RamLvFile {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
};

void *ramlv_open(lv_fs_drv_t *, const char *path, lv_fs_mode_t mode)
{
    if ((mode & LV_FS_MODE_WR) != 0) return nullptr;  // read-only
    if (!path) return nullptr;
    // LVGL may hand us the path with a leading '/' it inserts itself;
    // strip it.
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

lv_fs_drv_t g_drv;

void register_lvgl_driver()
{
    lv_fs_drv_init(&g_drv);
    g_drv.letter   = 'R';
    g_drv.open_cb  = ramlv_open;
    g_drv.close_cb = ramlv_close;
    g_drv.read_cb  = ramlv_read;
    g_drv.seek_cb  = ramlv_seek;
    g_drv.tell_cb  = ramlv_tell;
    lv_fs_drv_register(&g_drv);
    ESP_LOGI(TAG, "registered LVGL 'R:' filesystem driver");
}

}  // namespace

// ---------------------------------------------------------------------------
// RamFs singleton
// ---------------------------------------------------------------------------

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

void RamFs::registerLvglDriver()
{
    register_lvgl_driver();
}

const uint8_t *RamFs::peek(const std::string &path, size_t *len_out) const
{
    const std::string rel = strip_leading_slash(path);
    auto it = g_files.find(rel);
    if (it == g_files.end()) {
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
    auto it = g_files.find(rel);
    if (it == g_files.end()) return true;
    free_entry(it->second);
    g_files.erase(it);
    ESP_LOGI(TAG, "removed R:%s", rel.c_str());
    return true;
}

bool RamFs::removeTree(const std::string &name)
{
    const std::string rel = strip_leading_slash(name);
    // Empty prefix = wipe everything.
    if (rel.empty()) {
        for (auto &kv : g_files) free_entry(kv.second);
        g_files.clear();
        ESP_LOGI(TAG, "RamFs wiped (all files removed)");
        return true;
    }
    const std::string prefix = rel + "/";
    for (auto it = g_files.begin(); it != g_files.end(); ) {
        if (it->first == rel ||
            it->first.compare(0, prefix.size(), prefix) == 0) {
            free_entry(it->second);
            it = g_files.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

bool RamFs::list(const std::string &dirname, const ListCb &cb)
{
    const std::string rel    = strip_leading_slash(dirname);
    const std::string prefix = rel.empty() ? std::string() : (rel + "/");
    // Track which immediate sub-directories we've already reported
    // since the map is flat and may have several entries under the
    // same logical directory.
    std::unordered_map<std::string, bool> seen_dirs;
    for (const auto &kv : g_files) {
        const std::string &full = kv.first;
        if (!prefix.empty() &&
            full.compare(0, prefix.size(), prefix) != 0) {
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
    if (g_txn.handle != 0) {
        ESP_LOGE(TAG, "openWrite(R:%s): another RAM txn in flight", name.c_str());
        return 0;
    }
    const std::string rel = strip_leading_slash(name);
    if (rel.empty()) return 0;
    if (++g_next_handle == 0) g_next_handle = 1;
    g_txn.handle = g_next_handle;
    g_txn.path   = rel;
    g_txn.staging.clear();
    return g_txn.handle;
}

bool RamFs::appendWrite(uint32_t handle, const uint8_t *data, size_t len)
{
    if (handle == 0 || g_txn.handle != handle) return false;
    if (len == 0) return true;
    g_txn.staging.insert(g_txn.staging.end(), data, data + len);
    return true;
}

bool RamFs::closeWrite(uint32_t handle, bool commit)
{
    if (handle == 0 || g_txn.handle != handle) return false;
    bool ok = true;
    if (commit) {
        // Replace any existing entry.
        auto it = g_files.find(g_txn.path);
        if (it != g_files.end()) free_entry(it->second);
        RamEntry entry;
        entry.len = g_txn.staging.size();
        if (entry.len) {
            // Prefer PSRAM for large allocations.
            entry.data = static_cast<uint8_t *>(
                heap_caps_malloc(entry.len, MALLOC_CAP_SPIRAM));
            if (!entry.data) {
                // Fall back to whatever heap is left.
                entry.data = static_cast<uint8_t *>(malloc(entry.len));
            }
            if (!entry.data) {
                ESP_LOGE(TAG, "RamFs OOM committing %s (%u bytes)",
                         g_txn.path.c_str(), (unsigned)entry.len);
                ok = false;
            } else {
                memcpy(entry.data, g_txn.staging.data(), entry.len);
#if CONFIG_SPIRAM
                // Stage 55 / Stage 52 belt-and-braces: when the entry
                // lives in PSRAM and any future consumer reads it via
                // DMA (LCD frame-buffer, JPEG decoder, etc.) the data
                // cache must be flushed back to physical PSRAM before
                // that DMA peripheral can see the freshly-written
                // bytes. Today's consumers are CPU-only (LVGL image
                // decode via fread or mmap), which the data cache
                // serves transparently, so this msync is technically
                // unnecessary for them; flushing unconditionally
                // costs a few µs and keeps the invariant simple as
                // we add DMA-based decoders later.
                esp_cache_msync(entry.data, entry.len,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                ESP_CACHE_MSYNC_FLAG_UNALIGNED);
#endif
            }
        }
        if (ok) {
            g_files[g_txn.path] = entry;
            ESP_LOGI(TAG, "committed R:%s (%u bytes)",
                     g_txn.path.c_str(), (unsigned)entry.len);
        }
    } else {
        ESP_LOGI(TAG, "aborted R:%s", g_txn.path.c_str());
    }
    g_txn = {};
    return ok;
}
