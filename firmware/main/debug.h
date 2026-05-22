// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Print a snapshot of free heap (total / internal / PSRAM) and each
// FreeRTOS task's stack high-water mark (bytes remaining).
void dump_critical_info();
