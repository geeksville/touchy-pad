// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 16 — device-side macro runner. See macros.h.

#include "macros.h"

#include "protobuf.h"
#include "usb_hid.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "pb_encode.h"

#include <cstring>
#include <new>

static const char *TAG = "macros";

// Default inter-step delay (ms). Overridable per-macro via a `set_delay_ms`
// step.
#define MACRO_DEFAULT_STEP_DELAY_MS  10

// Up to this many pending macros may be queued before we start dropping.
// Each queue slot owns a heap-allocated PbMessage<touchy_ActionMacro>
// (the message wrapper itself is small; the steps array sits behind a
// pb_realloc'd pointer post-Stage 17), so this is cheap.
#define MACRO_QUEUE_DEPTH  4

// Scratch buffer used by `macros_run` to deep-copy a queued macro via
// nanopb encode + decode. Sized for the worst-case 16-step macro at the
// largest possible per-step payload (MouseMove with 3 varints plus tags
// is < 16 bytes; KeyEvent likewise; plus a few bytes of oneof framing).
// 512 bytes is ~5x the realistic high-water mark.
#define MACRO_CLONE_SCRATCH  512

namespace {

using MacroMsg = PbMessage<touchy_ActionMacro>;

QueueHandle_t s_queue;
TaskHandle_t  s_task;

// Run a single step. Returns the new sticky-delay value after this step
// completes (caller uses it for the next inter-step pause).
uint32_t run_step(const touchy_MacroStep &step, uint32_t sticky_delay_ms)
{
    switch (step.which_step) {
    case touchy_MacroStep_key_down_tag: {
        const auto &k = step.step.key_down;
        uint8_t kc[6] = { (uint8_t)k.keycode, 0, 0, 0, 0, 0 };
        usb_hid_keyboard_report((uint8_t)k.modifiers, kc);
        break;
    }
    case touchy_MacroStep_key_up_tag: {
        // Release everything. Macros are not expected to chord across
        // independent steps; chords are built via key_down + key_up pairs
        // that bracket explicit modifier setup/teardown.
        usb_hid_keyboard_report(0, nullptr);
        break;
    }
    case touchy_MacroStep_key_tap_tag: {
        const auto &k = step.step.key_tap;
        uint8_t kc[6] = { (uint8_t)k.keycode, 0, 0, 0, 0, 0 };
        usb_hid_keyboard_report((uint8_t)k.modifiers, kc);
        vTaskDelay(pdMS_TO_TICKS(sticky_delay_ms));
        usb_hid_keyboard_report(0, nullptr);
        break;
    }
    case touchy_MacroStep_mouse_button_down_tag:
        usb_hid_mouse_buttons((uint8_t)step.step.mouse_button_down, 0, 0, 0);
        break;
    case touchy_MacroStep_mouse_button_up_tag:
        // Same single-button semantics as key_up: emit an "all released"
        // mouse report.
        usb_hid_mouse_buttons(0, 0, 0, 0);
        break;
    case touchy_MacroStep_mouse_click_tag: {
        uint8_t b = (uint8_t)step.step.mouse_click;
        usb_hid_mouse_buttons(b, 0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(sticky_delay_ms));
        usb_hid_mouse_buttons(0, 0, 0, 0);
        break;
    }
    case touchy_MacroStep_mouse_move_tag: {
        const auto &m = step.step.mouse_move;
        // HID single-report deltas are int8; clamp.
        int8_t dx = m.dx >  127 ?  127 : m.dx < -127 ? -127 : (int8_t)m.dx;
        int8_t dy = m.dy >  127 ?  127 : m.dy < -127 ? -127 : (int8_t)m.dy;
        int8_t w  = m.wheel > 127 ? 127 : m.wheel < -127 ? -127 : (int8_t)m.wheel;
        usb_hid_mouse_buttons(0, dx, dy, w);
        break;
    }
    case touchy_MacroStep_set_delay_ms_tag:
        // Update the sticky delay for subsequent steps. No HID I/O.
        return step.step.set_delay_ms;
    case touchy_MacroStep_delay_ms_tag:
        // One-shot extra pause on top of the upcoming sticky delay.
        vTaskDelay(pdMS_TO_TICKS(step.step.delay_ms));
        break;
    default:
        ESP_LOGW(TAG, "unknown macro step tag %u", (unsigned)step.which_step);
        break;
    }
    return sticky_delay_ms;
}

void runner_task(void *)
{
    ESP_LOGI(TAG, "macro runner started");
    MacroMsg *m = nullptr;
    for (;;) {
        if (xQueueReceive(s_queue, &m, portMAX_DELAY) != pdTRUE) continue;
        if (!m) continue;

        const touchy_ActionMacro &payload = **m;
        uint32_t sticky = MACRO_DEFAULT_STEP_DELAY_MS;
        for (pb_size_t i = 0; i < payload.steps_count; i++) {
            sticky = run_step(payload.steps[i], sticky);
            vTaskDelay(pdMS_TO_TICKS(sticky));
        }
        delete m;
        m = nullptr;
    }
}

}  // namespace

extern "C" void macros_init(void)
{
    if (s_task) return;
    s_queue = xQueueCreate(MACRO_QUEUE_DEPTH, sizeof(MacroMsg *));
    // 4 KB stack is generous; runner only does small HID writes and delays.
    xTaskCreatePinnedToCore(runner_task, "macros", 4 * 1024,
                            nullptr, tskIDLE_PRIORITY + 2, &s_task, 1);
}

extern "C" bool macros_run(const touchy_ActionMacro *macro)
{
    if (!s_queue || !macro || macro->steps_count == 0) return false;

    // Deep-copy `macro` into a fresh PbMessage. With FT_POINTER on
    // ActionMacro.steps, the input macro's `steps` pointer is owned by
    // the active touchy_Screen; queuing a shallow copy would race screen
    // reloads. nanopb has no in-memory deep-copy primitive, so we round-
    // trip through encode/decode using a stack scratch buffer.
    uint8_t scratch[MACRO_CLONE_SCRATCH];
    pb_ostream_t out = pb_ostream_from_buffer(scratch, sizeof(scratch));
    if (!pb_encode(&out, touchy_ActionMacro_fields, macro)) {
        ESP_LOGE(TAG, "macro encode failed (%u steps, scratch=%u)",
                 (unsigned)macro->steps_count, (unsigned)sizeof(scratch));
        return false;
    }

    auto *copy = new (std::nothrow) MacroMsg(touchy_ActionMacro_fields);
    if (!copy) {
        ESP_LOGE(TAG, "out of memory wrapping macro");
        return false;
    }
    if (!copy->decode(scratch, out.bytes_written)) {
        ESP_LOGE(TAG, "macro re-decode failed");
        delete copy;
        return false;
    }
    if (xQueueSend(s_queue, &copy, 0) != pdTRUE) {
        ESP_LOGW(TAG, "macro queue full; dropping macro (%u steps)",
                 (unsigned)macro->steps_count);
        delete copy;
        return false;
    }
    return true;
}
