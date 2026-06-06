// SPDX-License-Identifier: GPL-3.0-or-later
//
// FlashFs implementation — LittleFS / VFS on the NOR flash partition.

#include "flash_fs.h"

#include "esp_littlefs.h"
#include "esp_log.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <new>
#include <string>

static const char *TAG = "fs.flash";

// LittleFS mount + partition labels (match firmware/partitions/<SIZE>.csv).
static constexpr const char *FLASH_MOUNT     = "/littlefs";
static constexpr const char *FLASH_PARTITION = "storage";

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

// Strip leading '/' (wire paths don't include one but defensive).
std::string strip_leading_slash(const std::string &p)
{
    size_t start = 0;
    while (start < p.size() && p[start] == '/') start++;
    return p.substr(start);
}

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

// ---- Per-transaction state -----------------------------------------------

struct FlashWriteTxn {
    uint32_t    handle = 0;
    std::string final_path;   // FS-relative, no /littlefs prefix
    std::string tmp_path;     // FS-relative
    FILE       *fp = nullptr;
};

FlashWriteTxn g_txn;
uint32_t      g_next_handle = 0;

}  // namespace

// ---------------------------------------------------------------------------
// FlashFs singleton
// ---------------------------------------------------------------------------

FlashFs &FlashFs::instance()
{
    static FlashFs s;
    return s;
}

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

bool FlashFs::usage(size_t *total, size_t *used) const
{
    if (!_mounted) return false;
    size_t t = 0, u = 0;
    if (esp_littlefs_info(FLASH_PARTITION, &t, &u) != ESP_OK) return false;
    if (total) *total = t;
    if (used) *used = u;
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

// ---------------------------------------------------------------------------
// Streaming write
//
// Strategy: write the streamed bytes into `<path>.tmp.<handle>` and on
// commit `rename(tmp, final)`. On abort or boot-time sweep the tmp
// file is unlinked.
// ---------------------------------------------------------------------------

uint32_t FlashFs::openWrite(const std::string &name)
{
    if (g_txn.handle != 0) {
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
    g_txn = { handle, rel, tmp_rel, fp };
    return handle;
}

bool FlashFs::appendWrite(uint32_t handle, const uint8_t *data, size_t len)
{
    if (handle == 0 || g_txn.handle != handle) return false;
    if (len == 0) return true;
    size_t w = fwrite(data, 1, len, g_txn.fp);
    if (w != len) {
        ESP_LOGE(TAG, "fwrite short on %s: %u/%u",
                 g_txn.tmp_path.c_str(), (unsigned)w, (unsigned)len);
        return false;
    }
    return true;
}

bool FlashFs::closeWrite(uint32_t handle, bool commit)
{
    if (handle == 0 || g_txn.handle != handle) return false;
    bool ok = (fclose(g_txn.fp) == 0);
    g_txn.fp = nullptr;
    const std::string tmp_full   = flash_full(g_txn.tmp_path);
    const std::string final_full = flash_full(g_txn.final_path);
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
    g_txn = {};
    return ok;
}
