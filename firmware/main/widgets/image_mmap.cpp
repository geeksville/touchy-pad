// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 52 \u2014 see image_mmap.h.

#include "image_mmap.h"
#include "tc_tag.h"

#include "fs/fs.h"

#include "esp_log.h"

#include <cstdio>
#include <cstring>

static const char *TAG = TOUCHY_TAG("image.mmap");

// Buffer used to format the per-call rejection reason string. Static
// is fine here because the LVGL task runs single-threaded and the
// caller consumes the string immediately (it logs it and falls back).
static char g_reason_buf[80];

bool try_mmap_image(const char *wire_path,
                    lv_image_dsc_t *dsc_out,
                    const char **reason_out)
{
    if (reason_out) *reason_out = "";
    if (!wire_path || !wire_path[0] || !dsc_out) {
        if (reason_out) *reason_out = "no path";
        return false;
    }

    // Step 1: can the FS hand us a stable in-memory pointer? Only
    // RamFs ('R:') can today; FlashFs ('F:') returns nullptr because
    // LittleFS pages aren't contiguous in CPU-addressable RAM.
    size_t len = 0;
    const uint8_t *bytes = fs_peek(wire_path, &len);
    if (!bytes) {
        if (reason_out) *reason_out = "not in mmappable FS (use R: drive)";
        return false;
    }
    if (len < sizeof(lv_image_header_t)) {
        if (reason_out) *reason_out = "file too short for LVGL bin header";
        return false;
    }

    // Step 2: copy the on-disk LVGL header out of the buffer. The
    // header layout matches `lv_image_header_t` exactly (that's the
    // whole point of the LVGL `.bin` format), but we still copy via
    // memcpy to avoid relying on the source being naturally aligned
    // for the bitfield reads.
    lv_image_header_t hdr;
    memcpy(&hdr, bytes, sizeof(hdr));

    // Step 3: format must EXACTLY match the display native; LVGL has
    // no zero-copy converter, and the whole point of the fast path is
    // to skip the convert/copy pass. Stage 53 will start emitting
    // plain RGB565 on the host so most images become eligible.
    const lv_color_format_t native = (lv_color_format_t)LV_COLOR_FORMAT_NATIVE;
    if ((lv_color_format_t)hdr.cf != native) {
        snprintf(g_reason_buf, sizeof(g_reason_buf),
                 "cf 0x%02x != display native 0x%02x",
                 (unsigned)hdr.cf, (unsigned)native);
        if (reason_out) *reason_out = g_reason_buf;
        return false;
    }

    // Step 4: build the descriptor. `data` aliases into the FS-owned
    // buffer; the caller owns the `lv_image_dsc_t` itself.
    memset(dsc_out, 0, sizeof(*dsc_out));
    dsc_out->header    = hdr;
    dsc_out->data      = bytes + sizeof(lv_image_header_t);
    dsc_out->data_size = len - sizeof(lv_image_header_t);
    ESP_LOGI(TAG, "mmap image %s: %ux%u cf=0x%02x data_size=%u",
             wire_path, (unsigned)hdr.w, (unsigned)hdr.h,
             (unsigned)hdr.cf, (unsigned)dsc_out->data_size);
    return true;
}
