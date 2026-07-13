// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared LEDC-PWM backlight driver (Stage 94). See backlight_pwm.h for the
// list of board_pins.h knobs and the adoption recipe.

#include "backlight_pwm.h"

#include "board.h"
#include "board_pins.h"
#include "tc_tag.h"

#include "driver/ledc.h"
#include "esp_log.h"

// ---- Defaults (overridable per board in board_pins.h) ---------------------

#ifndef BACKLIGHT_MIN_PWM
#define BACKLIGHT_MIN_PWM 16
#endif
#ifndef BACKLIGHT_PWM_BITS
#define BACKLIGHT_PWM_BITS 12
#endif
#ifndef BACKLIGHT_PWM_FREQ
#define BACKLIGHT_PWM_FREQ 6000
#endif
#ifndef BACKLIGHT_PWM_INVERT
#define BACKLIGHT_PWM_INVERT 0
#endif
#ifndef BACKLIGHT_LEDC_TIMER
#define BACKLIGHT_LEDC_TIMER LEDC_TIMER_0
#endif
#ifndef BACKLIGHT_LEDC_CHANNEL
#define BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_0
#endif
#ifndef BACKLIGHT_LEDC_CLK
#define BACKLIGHT_LEDC_CLK LEDC_AUTO_CLK
#endif

static const char *TAG = TOUCHY_TAG("backlight_pwm");

// Maximum LEDC duty for the configured resolution.
static constexpr uint32_t BL_FULL = (1u << BACKLIGHT_PWM_BITS) - 1u;

// Translate a 0..100 brightness level into a raw LEDC duty (pre-inversion).
static uint32_t level_to_duty(uint8_t level)
{
    if (level == 0) return 0;  // 0 always means truly off
    if (level > 100) level = 100;
    const uint32_t span = BL_FULL - (uint32_t)BACKLIGHT_MIN_PWM;
    return (uint32_t)BACKLIGHT_MIN_PWM + (uint32_t)((uint64_t)span * level / 100u);
}

void backlight_pwm_init(void)
{
    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode      = LEDC_LOW_SPEED_MODE;
    timer_cfg.duty_resolution = (ledc_timer_bit_t)BACKLIGHT_PWM_BITS;
    timer_cfg.timer_num       = BACKLIGHT_LEDC_TIMER;
    timer_cfg.freq_hz         = BACKLIGHT_PWM_FREQ;
    timer_cfg.clk_cfg         = BACKLIGHT_LEDC_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    // Start switched off (level 0). With active-low panels the off duty is
    // the inverted maximum.
    const uint32_t off_duty = BACKLIGHT_PWM_INVERT ? BL_FULL : 0u;

    ledc_channel_config_t ch_cfg = {};
    ch_cfg.gpio_num   = BOARD_BL_GPIO;
    ch_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
    ch_cfg.channel    = BACKLIGHT_LEDC_CHANNEL;
    ch_cfg.timer_sel  = BACKLIGHT_LEDC_TIMER;
    ch_cfg.duty       = off_duty;
    ch_cfg.hpoint     = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));

    ESP_LOGI(TAG, "Backlight PWM ready (GPIO%d, %d-bit, %d Hz, min=%d%s)",
             (int)BOARD_BL_GPIO, (int)BACKLIGHT_PWM_BITS, (int)BACKLIGHT_PWM_FREQ,
             (int)BACKLIGHT_MIN_PWM, BACKLIGHT_PWM_INVERT ? ", inverted" : "");
}

extern "C" void backlight_set(uint8_t level)
{
    uint32_t duty = level_to_duty(level);
    if (BACKLIGHT_PWM_INVERT) duty = BL_FULL - duty;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BACKLIGHT_LEDC_CHANNEL);
}
