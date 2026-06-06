// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// TOUCHY_TAG(s) — prepend "tc-" to an ESP_LOG tag string via C literal
// concatenation (zero runtime cost).  All touchy-pad subsystem tags use this
// so the host log tunnel can distinguish our logs from generic ESP-IDF /
// driver noise: the log_proto filter passes DEBUG/TRACE only for tags that
// start with "tc-", suppressing the third-party flood unconditionally.
#define TOUCHY_TAG(s) ("tc-" s)
