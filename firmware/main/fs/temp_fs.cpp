// SPDX-License-Identifier: GPL-3.0-or-later
//
// TempFs implementation — 'T:' drive routing + LVGL 'T:' driver.
// See temp_fs.h.

#include "temp_fs.h"
#include "tc_tag.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include <cstring>
#include <new>
#include <string>

static const char *TAG = TOUCHY_TAG("fs.temp");

// ---------------------------------------------------------------------------
// Backing-store selection
// ---------------------------------------------------------------------------

namespace {

// Decide the backing FS once. PSRAM present → RamFs (volatile, no flash
// wear); otherwise → FlashFs (the internal-RAM RamFs fallback is too
// small for image assets).
struct TempChoice {
    Fs  *fs       = nullptr;
    bool is_flash = false;
};

const TempChoice &temp_choice()
{
    static TempChoice choice = [] {
        TempChoice c;
        const size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        if (psram > 0) {
            c.fs       = &RamFs::instance();
            c.is_flash = false;
            ESP_LOGI(TAG, "'T:' drive → RamFs (PSRAM, %u bytes)", (unsigned)psram);
        } else {
            c.fs       = &FlashFs::instance();
            c.is_flash = true;
            ESP_LOGI(TAG, "'T:' drive → FlashFs (no PSRAM)");
        }
        return c;
    }();
    return choice;
}

}  // namespace

Fs *temp_fs_backing()
{
    return temp_choice().fs;
}

bool temp_is_flash()
{
    return temp_choice().is_flash;
}

// ---------------------------------------------------------------------------
// LVGL 'T:' driver
// ---------------------------------------------------------------------------
//
// Read-only and buffered: on open we read the whole file out of the
// backing FS into a heap copy and serve reads from it. This works
// uniformly whether the backing FS is RamFs (which could mmap) or
// FlashFs (which cannot) — transient image assets are small, so the
// extra copy is cheap and keeps a single code path. (The mmap fast
// path in widget_builders.cpp still kicks in separately for RamFs-
// backed 'T:' assets via fs_peek before LVGL is ever asked to open.)

namespace {

struct TempLvFile {
    uint8_t *data;   // heap copy owned by this handle
    size_t   len;
    size_t   pos;
};

void *templv_open(lv_fs_drv_t *, const char *path, lv_fs_mode_t mode)
{
    if ((mode & LV_FS_MODE_WR) != 0) return nullptr;  // read-only
    if (!path) return nullptr;
    const char *p = path;
    while (*p == '/') p++;  // LVGL may prepend a '/'
    size_t len = 0;
    uint8_t *bytes = temp_fs_backing()->readBinary(p, &len);
    if (!bytes) return nullptr;
    auto *fh = new (std::nothrow) TempLvFile{bytes, len, 0};
    if (!fh) {
        delete[] bytes;
        return nullptr;
    }
    return fh;
}

lv_fs_res_t templv_close(lv_fs_drv_t *, void *file_p)
{
    auto *fh = static_cast<TempLvFile *>(file_p);
    delete[] fh->data;
    delete fh;
    return LV_FS_RES_OK;
}

lv_fs_res_t templv_read(lv_fs_drv_t *, void *file_p, void *buf,
                        uint32_t btr, uint32_t *br)
{
    auto *fh = static_cast<TempLvFile *>(file_p);
    size_t left = fh->len - fh->pos;
    size_t got  = btr < left ? btr : left;
    memcpy(buf, fh->data + fh->pos, got);
    fh->pos += got;
    if (br) *br = (uint32_t)got;
    return LV_FS_RES_OK;
}

lv_fs_res_t templv_seek(lv_fs_drv_t *, void *file_p, uint32_t pos,
                        lv_fs_whence_t whence)
{
    auto *fh = static_cast<TempLvFile *>(file_p);
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

lv_fs_res_t templv_tell(lv_fs_drv_t *, void *file_p, uint32_t *pos_p)
{
    auto *fh = static_cast<TempLvFile *>(file_p);
    if (pos_p) *pos_p = (uint32_t)fh->pos;
    return LV_FS_RES_OK;
}

lv_fs_drv_t g_drv;

}  // namespace

void temp_fs_register_lvgl_driver()
{
    lv_fs_drv_init(&g_drv);
    g_drv.letter   = 'T';
    g_drv.open_cb  = templv_open;
    g_drv.close_cb = templv_close;
    g_drv.read_cb  = templv_read;
    g_drv.seek_cb  = templv_seek;
    g_drv.tell_cb  = templv_tell;
    lv_fs_drv_register(&g_drv);
    ESP_LOGI(TAG, "registered LVGL 'T:' filesystem driver");
}
