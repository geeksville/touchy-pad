// SPDX-License-Identifier: GPL-3.0-or-later
//
// See lv_throttled.h.

#include "lv_throttled.h"
#include "tc_tag.h"

#include "esp_log.h"
#include "lvgl.h"

#include <deque>
#include <functional>
#include <utility>

static const char *TAG = TOUCHY_TAG("lv.throttled");

namespace {

// FIFO of pending work. Accessed only on the LVGL task / under the LVGL
// port lock (see header), so it needs no separate mutex.
std::deque<std::function<void()>> g_queue;
lv_timer_t                       *g_timer = nullptr;

// How many items to run before yielding the lock until the next slice.
// Kept small so each lv_timer_handler pass stays short; the period
// below bounds how long the host_api task waits for the lock between
// slices.
constexpr uint32_t kPeriodMs = 8;
constexpr size_t   kPerSlice = 2;

void run_slice(lv_timer_t *t)
{
    size_t n = 0;
    while (n < kPerSlice && !g_queue.empty()) {
        std::function<void()> work = std::move(g_queue.front());
        g_queue.pop_front();
        work();
        n++;
    }
    if (g_queue.empty()) {
        // Drained — stop the periodic timer until the next post.
        // Deleting a timer from its own callback is supported: the
        // handler detects timer_deleted and skips the freed node.
        lv_timer_delete(t);
        g_timer = nullptr;
    }
}

}  // namespace

void lv_throttled_post(std::function<void()> work)
{
    if (!work) return;
    g_queue.emplace_back(std::move(work));
    if (g_timer) return;  // a drain is already running; it will pick this up
    g_timer = lv_timer_create(run_slice, kPeriodMs, nullptr);
    if (!g_timer) {
        // Timer alloc failed — drain inline rather than drop the work.
        // This reintroduces the burst-under-lock risk, but only in the
        // OOM corner case where deferral itself is impossible.
        ESP_LOGW(TAG, "lv_timer_create failed; draining %u item(s) inline",
                 (unsigned)g_queue.size());
        while (!g_queue.empty()) {
            std::function<void()> w = std::move(g_queue.front());
            g_queue.pop_front();
            w();
        }
    }
}
