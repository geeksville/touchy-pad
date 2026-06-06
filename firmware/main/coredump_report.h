// SPDX-License-Identifier: GPL-3.0-or-later
//
// Boot-time panic core-dump reporter.
//
// The shipped boards have no UART console wired, so a panic backtrace
// printed by the ROM panic handler is invisible. Instead we persist an
// ELF core dump to the `coredump` flash partition on panic
// (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH), then on the *next* boot decode
// its summary (faulting task, PC, exception cause, backtrace) and log it
// — which rides the Stage 64.1 proto log tunnel back to the host — and
// erase the image so it reports exactly once.
//
// Decode the logged PCs with:
//   xtensa-esp32s3-elf-addr2line -pfiaC
//       -e firmware/build/touchy_pad_v2.elf  0x<pc> 0x<bt0> 0x<bt1> ...

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Check the coredump partition for a saved panic image. If present, log
// a one-shot summary (at ERROR level so it survives any log-level
// filtering) and erase the image. No-op when no dump is stored or when
// coredump-to-flash is disabled. Call early in app_main(), right after
// log_proto_start(), so the report is among the first records queued and
// survives the boot-time log flood before the host connects.
void coredump_report_check_and_log(void);

#ifdef __cplusplus
}
#endif
