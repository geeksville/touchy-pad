// SPDX-License-Identifier: GPL-3.0-or-later
//
// Throttled deferral of work onto the LVGL task.
//
// `lv_async_call` is the obvious way to run a closure on the LVGL task,
// but it has a sharp edge: it creates a period-0 lv_timer, and
// lv_timer_handler's "a timer was created → restart the run loop" path
// (managed_components/lvgl__lvgl/src/misc/lv_timer.c) re-runs any
// freshly created period-0 timer *within the same lv_timer_handler
// invocation* — i.e. under ONE continuous LVGL-port-lock hold. So a
// burst of N async calls, each doing real work (a widget-subtree
// rebuild, an image re-mmap, a screen reload), drains back-to-back
// without ever releasing the port lock. While that lock is held, the
// host_api dispatch task cannot take it (e.g. in
// screens_prepare_file_overwrite) — which surfaces host-side as multi-
// second RPC timeouts, "stale write transaction" aborts, and dropped
// uploads. Observed on a jc4827w543 during an OpenDeck profile switch:
// ~40 per-key stub/image writes stalled the lock ~1.5 s.
//
// lv_throttled_post() funnels deferred work through a single FIFO that
// is drained by a *periodic* lv_timer (non-zero period). A timer with a
// non-zero period has last_run == now, so it is NOT ready in the
// current lv_timer_handler pass: the run loop ends, the port lock is
// released, and the next batch runs a few ms later in a fresh handler
// call — handing the host_api task a window to take the lock between
// batches. At most kPerSlice items run per batch, so no burst can
// monopolise the lock.
//
// THREADING: post only from the LVGL task or while holding the LVGL
// port lock (lvgl_port_lock). The queue and timer are plain statics
// guarded by that lock; every current caller already satisfies this
// (event callbacks run under lv_timer_handler's lock; host_api callers
// wrap the post in lvgl_port_lock()).

#pragma once

#include <functional>

// Queue `work` to run on the LVGL task, throttled so a burst of posts
// cannot hold the LVGL port lock continuously. Items run in FIFO order,
// a few per timer slice. Must be called on the LVGL task or under the
// LVGL port lock.
void lv_throttled_post(std::function<void()> work);
