// SPDX-License-Identifier: GPL-3.0-or-later
//
// Touchy-Pad backlight control + auto-sleep (stage 19).

#include "backlight.h"
#include "tc_tag.h"

#include "board.h"
#include "prefs.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <stdatomic.h>

static const char *TAG = TOUCHY_TAG("backlight");

static esp_timer_handle_t s_timer    = nullptr;
static uint32_t           s_timeout_ms = 0;
static atomic_bool        s_on       = true;  // tracks current backlight state
static uint8_t            s_level    = 100;   // remembered brightness when awake

// ---------------------------------------------------------------------------
// Timer callback — fires after `s_timeout_ms` of inactivity.
// ---------------------------------------------------------------------------

static void sleep_timer_cb(void *)
{
    ESP_LOGI(TAG, "auto-sleep: backlight off");
    backlight_set(0);
    atomic_store(&s_on, false);
}

// ---------------------------------------------------------------------------
// Internal helpers (not thread-safe on their own — callers hold no lock,
// but esp_timer_stop/start_once are internally serialised by ESP-IDF).
// ---------------------------------------------------------------------------

static void arm_timer(void)
{
    if (!s_timer || s_timeout_ms == 0) return;
    esp_timer_stop(s_timer);  // no-op if not running (returns ERR_INVALID_STATE)
    esp_timer_start_once(s_timer, (uint64_t)s_timeout_ms * 1000ULL);
}

static void disarm_timer(void)
{
    if (s_timer) esp_timer_stop(s_timer);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void backlight_init(uint32_t timeout_ms)
{
    esp_timer_create_args_t args = {};
    args.callback = sleep_timer_cb;
    args.name     = "bl_sleep";
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_timer));

    s_timeout_ms = timeout_ms;
    s_level      = Prefs::instance().backlight_level();
    atomic_store(&s_on, true);
    // board_init() configured the backlight hardware but left it off; turn
    // it on now at the persisted brightness.
    backlight_set(s_level);

    if (timeout_ms > 0) {
        arm_timer();
        ESP_LOGI(TAG, "auto-sleep armed: %" PRIu32 " ms", timeout_ms);
    } else {
        ESP_LOGI(TAG, "auto-sleep disabled");
    }
}

void backlight_wake(void)
{
    // Turn on (if it was off).
    if (!atomic_load(&s_on)) {
        ESP_LOGI(TAG, "wake: backlight on");
        backlight_set(s_level);
        atomic_store(&s_on, true);
    }
    // Always reset the countdown so the user gets a full timeout from now.
    arm_timer();
}

void backlight_touch_activity(void)
{
    backlight_wake();
}

void backlight_set_timeout(uint32_t ms)
{
    s_timeout_ms = ms;
    // Persist the new value immediately.
    Prefs::instance().set_screen_timeout_ms(ms);

    if (ms == 0) {
        // Disable: cancel any pending sleep and ensure the light is on.
        disarm_timer();
        if (!atomic_load(&s_on)) {
            backlight_set(s_level);
            atomic_store(&s_on, true);
        }
        ESP_LOGI(TAG, "auto-sleep disabled");
    } else {
        // Arm (or re-arm) from now.
        arm_timer();
        ESP_LOGI(TAG, "auto-sleep set to %" PRIu32 " ms", ms);
    }
}

void backlight_set_level(uint8_t level)
{
    if (level > 100) level = 100;
    s_level = level;
    // Persist immediately so the brightness survives a reboot.
    Prefs::instance().set_backlight_level(level);
    // Apply now only if the display is currently awake; if it is auto-slept
    // the new level takes effect on the next wake.
    if (atomic_load(&s_on)) {
        backlight_set(level);
    }
    ESP_LOGI(TAG, "backlight level set to %u%%", (unsigned)level);
}
