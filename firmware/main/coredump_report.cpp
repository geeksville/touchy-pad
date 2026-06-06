// SPDX-License-Identifier: GPL-3.0-or-later
//
// See coredump_report.h.

#include "coredump_report.h"
#include "tc_tag.h"

#include "sdkconfig.h"

#include "esp_log.h"

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH

#include "esp_core_dump.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char *TAG = TOUCHY_TAG("coredump");

void coredump_report_check_and_log(void)
{
    // Is there a valid image in the coredump partition?
    esp_err_t err = esp_core_dump_image_check();
    if (err != ESP_OK) {
        // ESP_ERR_NOT_FOUND / ESP_ERR_INVALID_CRC etc — nothing usable.
        // Stay quiet on the common "no dump" case; only note real damage.
        if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_INVALID_SIZE) {
            ESP_LOGW(TAG, "coredump image present but unusable (%s)",
                     esp_err_to_name(err));
            // Erase so a corrupt image doesn't nag every boot.
            esp_core_dump_image_erase();
        }
        return;
    }

    esp_core_dump_summary_t *summary =
        (esp_core_dump_summary_t *)calloc(1, sizeof(esp_core_dump_summary_t));
    if (!summary) {
        ESP_LOGE(TAG, "OOM allocating coredump summary");
        return;
    }

    err = esp_core_dump_get_summary(summary);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "previous boot crashed but summary decode failed (%s)",
                 esp_err_to_name(err));
        free(summary);
        esp_core_dump_image_erase();
        return;
    }

    // Headline: which task, where. exc_task is null-terminated.
    ESP_LOGE(TAG, "==== previous boot PANICKED ====");
    ESP_LOGE(TAG, "task='%s' PC=0x%08" PRIx32,
             summary->exc_task, summary->exc_pc);

#if __XTENSA__
    // Xtensa extra info carries the exception cause + faulting address —
    // e.g. cause 28/29 (LoadProhibited/StoreProhibited) with a bogus
    // vaddr is the classic use-after-free / null-deref signature.
    ESP_LOGE(TAG, "exc_cause=%" PRIu32 " exc_vaddr=0x%08" PRIx32,
             summary->ex_info.exc_cause, summary->ex_info.exc_vaddr);
#endif

    // Pack the whole backtrace onto one log line so it costs a single
    // tunnel record (the queue is shallow at boot). addr2line-ready.
    const esp_core_dump_bt_info_t &bt = summary->exc_bt_info;
    char line[256];
    int off = snprintf(line, sizeof(line), "backtrace%s:",
                       bt.corrupted ? " (CORRUPTED)" : "");
    for (uint32_t i = 0; i < bt.depth && off > 0 && off < (int)sizeof(line); i++) {
        off += snprintf(line + off, sizeof(line) - off, " 0x%08" PRIx32, bt.bt[i]);
    }
    ESP_LOGE(TAG, "%s", line);
    ESP_LOGE(TAG, "decode: xtensa-esp32s3-elf-addr2line -pfiaC -e "
                  "firmware/build/touchy_pad_v2.elf <PCs above>");

    free(summary);

    // One-shot: wipe the image so we don't re-report it on every boot.
    err = esp_core_dump_image_erase();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to erase coredump image (%s)",
                 esp_err_to_name(err));
    }
}

#else  // CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH

void coredump_report_check_and_log(void) {}

#endif  // CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
