// SPDX-License-Identifier: Apache-2.0
//
// Touchy-Pad on-device filesystem wrapper — see fs.h.

#include "fs.h"

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

static const char *TAG = "fs";

// Mount point and partition label must match the entry in
// boards/<board>/partitions.csv (subtype = littlefs).
static constexpr const char *MOUNT       = "/littlefs";
static constexpr const char *PARTITION   = "storage";

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

// Strip leading '/' so we can blindly join MOUNT + "/" + name.
static std::string normalise(const std::string &name)
{
    size_t start = 0;
    while (start < name.size() && name[start] == '/') start++;
    return name.substr(start);
}

static std::string full_path(const std::string &name)
{
    return std::string(MOUNT) + "/" + normalise(name);
}

// Create every intermediate directory in `full` *before* the final
// component. Existing directories are silently accepted.
static bool ensure_parents(const std::string &full)
{
    // Walk forward, splitting on '/'. Skip the very first byte (the leading
    // '/' of the mount point) so we don't try to mkdir "/".
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

// ---------------------------------------------------------------------------
// LVGL filesystem driver (letter 'F')
// ---------------------------------------------------------------------------
//
// LVGL paths like "F:screens/home.xml" are rewritten to
// /littlefs/from_host/screens/home.xml and serviced via the POSIX VFS that
// esp_littlefs already plugs into newlib. The driver is registered once
// from Fs::begin(). All file/dir handles flow straight through to libc.

static const char *FROM_HOST = "/littlefs/from_host/";

static std::string lv_resolve(const char *path)
{
    if (!path) return FROM_HOST;
    while (*path == '/') path++;  // tolerate "F:/foo" as well as "F:foo"
    return std::string(FROM_HOST) + path;
}

static lv_fs_res_t errno_to_lv()
{
    switch (errno) {
    case 0:      return LV_FS_RES_OK;
    case ENOENT: return LV_FS_RES_NOT_EX;
    case EACCES: return LV_FS_RES_DENIED;
    case ENOSPC: return LV_FS_RES_FULL;
    default:     return LV_FS_RES_FS_ERR;
    }
}

static void *lv_open_cb(lv_fs_drv_t *, const char *path, lv_fs_mode_t mode)
{
    const char *m = "rb";
    if ((mode & LV_FS_MODE_WR) && (mode & LV_FS_MODE_RD)) m = "rb+";
    else if (mode & LV_FS_MODE_WR)                       m = "wb";
    const std::string full = lv_resolve(path);
    FILE *f = fopen(full.c_str(), m);
    if (!f) {
        ESP_LOGW(TAG, "lv_open(%s, %s) failed: %s", full.c_str(), m, strerror(errno));
    }
    return f;
}

static lv_fs_res_t lv_close_cb(lv_fs_drv_t *, void *f)
{
    return fclose((FILE *)f) == 0 ? LV_FS_RES_OK : errno_to_lv();
}

static lv_fs_res_t lv_read_cb(lv_fs_drv_t *, void *f, void *buf, uint32_t btr, uint32_t *br)
{
    size_t got = fread(buf, 1, btr, (FILE *)f);
    if (br) *br = (uint32_t)got;
    return ferror((FILE *)f) ? errno_to_lv() : LV_FS_RES_OK;
}

static lv_fs_res_t lv_write_cb(lv_fs_drv_t *, void *f, const void *buf, uint32_t btw, uint32_t *bw)
{
    size_t wrote = fwrite(buf, 1, btw, (FILE *)f);
    if (bw) *bw = (uint32_t)wrote;
    return wrote == btw ? LV_FS_RES_OK : errno_to_lv();
}

static lv_fs_res_t lv_seek_cb(lv_fs_drv_t *, void *f, uint32_t pos, lv_fs_whence_t whence)
{
    int w = (whence == LV_FS_SEEK_CUR) ? SEEK_CUR
          : (whence == LV_FS_SEEK_END) ? SEEK_END
          :                              SEEK_SET;
    return fseek((FILE *)f, (long)pos, w) == 0 ? LV_FS_RES_OK : errno_to_lv();
}

static lv_fs_res_t lv_tell_cb(lv_fs_drv_t *, void *f, uint32_t *pos_p)
{
    long p = ftell((FILE *)f);
    if (p < 0) return errno_to_lv();
    if (pos_p) *pos_p = (uint32_t)p;
    return LV_FS_RES_OK;
}

static void *lv_dir_open_cb(lv_fs_drv_t *, const char *path)
{
    const std::string full = lv_resolve(path);
    DIR *d = opendir(full.c_str());
    if (!d) ESP_LOGW(TAG, "lv_diropen(%s) failed: %s", full.c_str(), strerror(errno));
    return d;
}

static lv_fs_res_t lv_dir_read_cb(lv_fs_drv_t *, void *rd, char *fn, uint32_t fn_len)
{
    // LVGL convention: prefix directory names with '/'.
    while (struct dirent *ent = readdir((DIR *)rd)) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (ent->d_type == DT_DIR) snprintf(fn, fn_len, "/%s", ent->d_name);
        else                       snprintf(fn, fn_len, "%s",  ent->d_name);
        return LV_FS_RES_OK;
    }
    if (fn_len > 0) fn[0] = '\0';
    return LV_FS_RES_OK;
}

static lv_fs_res_t lv_dir_close_cb(lv_fs_drv_t *, void *rd)
{
    return closedir((DIR *)rd) == 0 ? LV_FS_RES_OK : errno_to_lv();
}

static void register_lvgl_driver()
{
    static lv_fs_drv_t drv;  // must outlive the driver registration
    lv_fs_drv_init(&drv);
    drv.letter       = 'F';
    drv.cache_size   = 0;
    drv.open_cb      = lv_open_cb;
    drv.close_cb     = lv_close_cb;
    drv.read_cb      = lv_read_cb;
    drv.write_cb     = lv_write_cb;
    drv.seek_cb      = lv_seek_cb;
    drv.tell_cb      = lv_tell_cb;
    drv.dir_open_cb  = lv_dir_open_cb;
    drv.dir_read_cb  = lv_dir_read_cb;
    drv.dir_close_cb = lv_dir_close_cb;
    lv_fs_drv_register(&drv);
    ESP_LOGI(TAG, "registered LVGL filesystem driver 'F:' -> %s", FROM_HOST);
}

// ---------------------------------------------------------------------------
// Fs implementation
// ---------------------------------------------------------------------------

Fs &Fs::instance()
{
    static Fs s_fs;
    return s_fs;
}

bool Fs::begin()
{
    if (_mounted) return true;

    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path              = MOUNT;
    conf.partition_label        = PARTITION;
    conf.format_if_mount_failed = true;
    conf.dont_mount             = false;

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return false;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(PARTITION, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "mounted %s on %s: %u/%u bytes used",
                 PARTITION, MOUNT, (unsigned)used, (unsigned)total);
    }

    // Pre-create well-known top-level directories so callers don't have to.
    for (const char *d : { "/littlefs/prefs", "/littlefs/from_host" }) {
        if (mkdir(d, 0755) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "mkdir(%s) failed: %s", d, strerror(errno));
        }
    }

    // Stage 15: hook the LVGL filesystem abstraction up to /littlefs/from_host
    // so XML loaders can use "F:..." paths without leaking the mount prefix.
    register_lvgl_driver();

    _mounted = true;
    return true;
}

std::string Fs::readText(const std::string &name)
{
    size_t len = 0;
    uint8_t *buf = readBinary(name, &len);
    if (!buf) return {};
    std::string out(reinterpret_cast<const char *>(buf), len);
    delete[] buf;
    return out;
}

uint8_t *Fs::readBinary(const std::string &name, size_t *len_out)
{
    if (len_out) *len_out = 0;
    const std::string p = full_path(name);

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

bool Fs::writeFile(const std::string &name, const uint8_t *data, size_t len)
{
    const std::string p = full_path(name);
    if (!ensure_parents(p)) return false;

    FILE *f = fopen(p.c_str(), "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s, w) failed: %s", p.c_str(), strerror(errno));
        return false;
    }
    const size_t wrote = len ? fwrite(data, 1, len, f) : 0;
    fclose(f);
    if (wrote != len) {
        ESP_LOGE(TAG, "short write on %s: %u/%u", p.c_str(), (unsigned)wrote, (unsigned)len);
        return false;
    }

    ESP_LOGI(TAG, "wrote %s (%u bytes)", p.c_str(), (unsigned)len);
    return true;
}

bool Fs::remove(const std::string &name)
{
    const std::string p = full_path(name);
    if (unlink(p.c_str()) == 0) {
        ESP_LOGI(TAG, "removed %s", p.c_str());
        return true;
    }
    if (errno == ENOENT) return true;
    ESP_LOGE(TAG, "unlink(%s) failed: %s", p.c_str(), strerror(errno));
    return false;
}

bool Fs::removeTree(const std::string &name)
{
    const std::string p = full_path(name);

    DIR *dir = opendir(p.c_str());
    if (!dir) {
        if (errno == ENOENT) return true;            // already gone
        // Not a directory — try a plain unlink.
        if (unlink(p.c_str()) == 0 || errno == ENOENT) return true;
        ESP_LOGE(TAG, "removeTree(%s): %s", p.c_str(), strerror(errno));
        return false;
    }

    bool ok = true;
    while (struct dirent *ent = readdir(dir)) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        std::string child = name + "/" + ent->d_name;
        if (!removeTree(child)) ok = false;
    }
    closedir(dir);

    if (rmdir(p.c_str()) != 0 && errno != ENOENT) {
        ESP_LOGE(TAG, "rmdir(%s) failed: %s", p.c_str(), strerror(errno));
        ok = false;
    } else {
        ESP_LOGI(TAG, "removed dir %s", p.c_str());
    }
    return ok;
}

bool Fs::list(const std::string &dirname, const ListCb &cb)
{
    const std::string p = full_path(dirname);
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
