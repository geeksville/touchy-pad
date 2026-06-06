// SPDX-License-Identifier: GPL-3.0-or-later

#include "debug.h"
#include "tc_tag.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <new>

static const char *TAG = TOUCHY_TAG("debug");

void dump_critical_info()
{
    ESP_LOGD(TAG, "heap: free=%u  min_ever=%u  internal=%u  psram=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    UBaseType_t n = uxTaskGetNumberOfTasks();
    // Allocate a few extra slots in case tasks spawn between the count
    // and the snapshot; uxTaskGetSystemState caps at the array size.
    auto *tasks = new (std::nothrow) TaskStatus_t[n + 4];
    if (!tasks) {
        ESP_LOGW(TAG, "dump_critical_info: OOM for task array");
        return;
    }
    UBaseType_t filled = uxTaskGetSystemState(tasks, n + 4, nullptr);
    for (UBaseType_t i = 0; i < filled; i++) {
        // usStackHighWaterMark is in words; convert to bytes.
        // Thresholds:
        //   >= configMINIMAL_STACK_SIZE (default idle-task stack, ~1536 B + Xtensa overhead)  → OK
        //   >= configMINIMAL_STACK_SIZE/4                                                      → WARN
        //   <  configMINIMAL_STACK_SIZE/4                                                      → !!!!
        unsigned hwm_b = (unsigned)(tasks[i].usStackHighWaterMark * sizeof(StackType_t));
        const char *level =
            hwm_b >= configMINIMAL_STACK_SIZE       ? "  OK" :
            hwm_b >= configMINIMAL_STACK_SIZE / 4u  ? "WARN" : "!!!!";
        ESP_LOGD(TAG, "  task %-16s  stack_free=%5u B  [%s]",
                 tasks[i].pcTaskName, hwm_b, level);
    }
    delete[] tasks;
}
