# touchy-pad — AI Agent Guide

Open-source multitouch USB touchpad / button matrix with a built-in
customisable LCD (ESP32-S3 and ESP32-P4; boards: jc4827w543, waveshare_s3_lcd_7b,
elecrow_s3_lcd_7, elecrow_s3_lcd_7_adv, elecrow_p4_lcd_7, squixl, matouch_43).
The host-side companion is a Python package (`touchy-pad`) that ships a
CLI (`touchy`), a high-level API, a Tkinter/PySide6 device simulator, and
a StreamDeck-compatibility shim (`TouchyDeck`).

`CLAUDE.md` is a symlink to this file — keep them in sync via the symlink.

## Repo layout
| Path | Purpose |
|------|---------|
| `firmware/main/` | ESP-IDF C++ firmware (CMake, **not** PlatformIO) |
| `firmware/main/main.cpp` | Entry point — keep thin; subsystems live in their own `.cpp/.h` |
| `firmware/boards/<board>/` | Per-board pinout / display / touch drivers |
| `proto/` | Shared protobuf schemas (`touchy.proto`, `widgets.proto`, `preferences.proto`) + nanopb `.options` files |
| `app/src/touchy_pad/` | Python package — `cli.py`, `client.py`, `transport.py`, `api/`, `sim/`, `touchydeck/`, `_proto/` |
| `app/tests/` | pytest suite (host-side only; firmware has no unit tests) |
| `tools/StreamController/` | git submodule, branch `pr-touchypad`, with `touchy_bootstrap.py` shim |
| `tools/streamdeck-probe/` | Stage 50.1 reverse-engineering tool |
| `docs/design.md` | **Authoritative stage history — read before starting new work** |
| `docs/host-api.md` | USB endpoint + protocol spec |
| `Justfile` | All build/test/run tasks — prefer `just <recipe>` over raw commands |
| `VERSION` | Single-source version (read by Python + CMake) |

## Implementation status
All stages 0–24.4, 50.2, 51, 64.1, 64.3, 64.4, 65, 65.1, 67, 68, 72, 81, 82, 83, 84, 85, 86, 87, and 90 are **done**. Latest active wire-format:
`Screen.Version.CURRENT == 5`, `Widget.Version.CURRENT == 21`,
`SysBoardInfoResponse.ProtocolVersion.CURRENT == 10`,
`PreferencesFile.Version.CURRENT == 4`.
Highlights worth remembering:

- **Boards span two chips (Stage 65).** ESP32-S3 boards (`jc4827w543`,
  `waveshare_s3_lcd_7b`) have native USB; the classic-ESP32
  `esp32_2432s028rv3` ("CYD2USB") does not. Each board declares its chip
  in a one-line `firmware/boards/<board>/target` file; USB capability is
  keyed off `CONFIG_SOC_USB_OTG_SUPPORTED` (not a custom flag). `just
  firmware-reconfigure [board]` reads `target` and runs `idf.py
  -DBOARD=<board> set-target <chip>` — the `-DBOARD` is required, and
  `rm -f firmware/sdkconfig firmware/sdkconfig.<board>` before switching
  chips. `firmware/main/platform.{h,cpp}` exposes `platform_get()` →
  `{is_multitouch, has_usb}`, surfaced over proto as
  `SysBoardInfoResponse.is_multitouch` / `has_usb`.

- **The "CYD" family shares one C++ implementation (Stage 65.1).** All
  classic-ESP32 CYD boards (`esp32_2432s028rv3` 2.8" ST7789,
  `esp32_2432s024` 2.4" ILI9341, more coming) compile the *same* sources in
  `firmware/boards/cyd_common/` (`board.cpp`, `display.cpp`, `touch.cpp`).
  Each board dir keeps only its `board_pins.h` + a tiny
  `board/CMakeLists.txt` (which lists `../../cyd_common/*.cpp` and sets
  `PRIV_INCLUDE_DIRS "."` so the board's own `board_pins.h` wins) +
  `idf_component.yml`. No symlinks. The panel driver is chosen at compile
  time from `board_pins.h`: `cyd_common/display.cpp` keys off
  `BOARD_LCD_CONTROLLER_ILI9341` (pulls the `espressif/esp_lcd_ili9341`
  managed component, `esp_lcd_new_panel_ili9341`) vs the default
  `BOARD_LCD_CONTROLLER_ST7789` (in-tree `esp_lcd_new_panel_st7789`); bring-up
  is otherwise identical. ILI9341 typically wants `BOARD_LCD_INVERT_COLOR=0`
  where ST7789 wants `1`.

- USB device is a composite class: CDC-ACM + HID (mouse + keyboard via
  report IDs 1/2) + vendor-class bulk pair (command/response) + interrupt-IN
  mailbox endpoint (0x85) that just signals "events available".
- Host ↔ device wire protocol = self-synchronising frames
  `MAGIC(0xA5 0x5A) | LEN(u16 LE) | payload | CRC8` (Stage 64.3) over the
  bulk pair. Identical framing on every transport (USB, simulator TCP,
  serial). One decoder per side: `firmware/main/host_api.cpp` (the
  `HostApiLink` abstraction), `app/src/touchy_pad/transport.py`
  (`_StreamFramedTransport` / `_FrameDecoder`), and
  `rust/touchy-pad/src/transport.rs` (`FrameDecoder`). The serial
  transport (`transport_serial.py`; Rust `transport_serial.rs` behind the
  `serial` feature) always runs at 115200 baud and carries only protocol
  frames — device logs ride the Stage 64.1 `LogRecord` tunnel, never raw
  text on the protocol port. Firmware serial path is gated on
  `CONFIG_TOUCHY_PROTO_OVER_SERIAL` (default n; `y` for the CYD board,
  which has a `UartLink` on `UART_NUM_0` gated `#if
  CONFIG_TOUCHY_PROTO_OVER_SERIAL && !CONFIG_SOC_USB_OTG_SUPPORTED`).
  `firmware/main/CMakeLists.txt` REQUIRES `esp_driver_uart` for this
  (IDF v6 split out `driver/uart.h`). Gotcha: in `sdkconfig.defaults`,
  `# CONFIG_X is not set` is **not** a comment — it's the `X=n` directive
  and will silently override an earlier `CONFIG_X=y`.
- nanopb uses `FT_POINTER` (heap) for `repeated` widget/action/step
  fields and the `FileWrite` payload. RAII via `PbMessage<T>` in
  `firmware/main/protobuf.h`.
- Filesystem paths are drive-prefixed: `F:host/...` = LittleFS (persistent
  flash), `R:host/...` = ramdisk (transient, e.g. image assets). RamFs
  prefers `MALLOC_CAP_SPIRAM` but falls back to internal RAM, so the `R:`
  drive and image assets work even on no-PSRAM boards (CYD, ~520 KB SRAM).
  Screens live under `F:host/s/` (moved from `host/screens/` in Stage 68);
  `host/s/default.pb` is the canonical prev/next chrome the firmware prefers
  as its boot screen. User page bodies live under `F:host/uscr/` and the
  chrome's `widget_ref(id="page")` pages through them. Path constants are
  centralised in `app/src/touchy_pad/paths.py` (Python) and `lib.rs` (Rust).
- Stage 21 (Python CLI for layouts) is implemented as `touchy screens push`,
  consuming the `touchy_pad.api.screens` DSL (`button`, `slider`, `toggle`,
  `image_button`, `trackpad`, `log_line`, layout helpers `row`/`col`/`grid`).
- Stage 67 lets `host_action(on_event=lambda e: ...)` attach a callback
  inline. Codes auto-allocate from `_events.AUTO_CODE_BASE` (`0x10000`);
  manual codes stay below that. The callable is stashed in
  `app/src/touchy_pad/api/_events.py` keyed by code, then
  `Touchy.screen_save`/`widget_save` walk the proto tree
  (`_collect_host_codes`), `harvest()` the matching bindings, and register
  them via `on_host_event` — so inline callbacks light up on upload.
- Stage 68 cleaned up screen switching. Screens moved `host/screens/`→
  `host/s/` via symbolic path constants. The prev/next chrome is now one
  default screen `host/s/default.pb` built from
  `touchy_pad.api.screens.build_default_screen()` — a vertical flex column
  (`col`) holding a prev/next chrome row plus a growing body
  `widget_ref(id="page")`.
- **Sizing is opt-in via `Widget.grow_x` / `grow_y` (Stage 72).** These
  two `int32` fields live on `Widget` itself (NOT on `Rect`/`GridCell`),
  so they work the same regardless of the `placement` oneof. Default
  `0` = content-sized (children no longer auto-stretch). Semantics depend
  on the parent and axis:
  - **Flex main axis** (`grow_x` for a ROW parent, `grow_y` for a COLUMN
    parent) maps to `lv_obj_set_flex_grow(obj, factor)` — proportional,
    so the magnitude matters.
  - **Flex cross axis** (`grow_y` in a ROW, `grow_x` in a COLUMN) and
    **grid** (both axes) treat any value `> 0` as "fill" (magnitude
    ignored): cross-axis flex → `lv_pct(100)`, grid → `LV_GRID_ALIGN_STRETCH`
    (otherwise grid cells now CENTER, the new default). There is no more
    implicit COLUMN cross-fill — callers must set `grow_x=1` to get
    full-width COLUMN children. `screens.grow(widget, x=, y=)` is the DSL
    helper; `cell(..., grow_x=, grow_y=)` sets it on grid children. The
    firmware reads it in `apply_rect`/`apply_grid_cell`
    (`screen_layout.cpp`); the parent flow is threaded via the
    `parent_layout` arg (also stored on `ActiveRef` so the Stage-57
    widget-ref rebuild keeps the fill). The sim mirrors this via Qt
    box-layout stretch factors + per-axis alignment.
- A `grow_x=1` spacer between the prev/next buttons pushes them to
  opposite edges. User page bodies live in `host/uscr/` and are uploaded via
  `Touchy.user_screen_save(name, widget)`. The `touchy screen init` CLI
  provisions the chrome + a trackpad page; `screen demo` adds a smiley test
  page. The firmware's built-in fallback is **generated** from the same DSL:
  `proto/gen_default_screen.py` → `proto/default_screen.json` (pure JSON,
  no comments — `json_format.Parse` rejects them) → `embed_screen_json.py`
  → `firmware/main/default_screen_pb.h`, all wired through the
  `just gen-default-screen` / `build-default-screen` recipes. The simulator
  reads `proto/default_screen.json` at runtime.
- Stage 30 simulator lives in `app/src/touchy_pad/sim/` (Tkinter/PySide6).
  Invoke with `touchy --sim ...` (or `--sim-headless` for CI).
- Stage 50.2 StreamDeck shim is `touchy_pad.touchydeck.TouchyDeck`;
  `touchy_pad.touchydeck.install()` monkey-patches
  `StreamDeck.DeviceManager.enumerate`. **Must be called explicitly** —
  no import side-effects. See `tools/StreamController/touchy_bootstrap.py`.
- Stage 64.1 tunnels device ESP_LOG output back to the host over the
  same `EventConsumeCmd` poll: when the event queue is empty but a log
  is waiting the Response's `payload` oneof carries `LogRecord` (tag 5)
  instead of `EventConsumeResponse`. Firmware hook is
  `firmware/main/log_proto.{h,cpp}` (gated on `CONFIG_TOUCHY_LOG_OVER_PROTO`,
  default `y` in `firmware/sdkconfig.defaults`). Host dispatchers:
  `TouchyClient._dispatch_log_record` → `touchy_pad.device` logger;
  Rust `dispatch_log_record` → `log` crate at target `touchy_pad::device::<TAG>`.
- Stage 81 added free-memory/storage numbers to `SysBoardInfoResponse`
  (`free_heap_bytes`, `free_psram_bytes`, `fs_total_bytes`, `fs_used_bytes`,
  all `uint64`; `ProtocolVersion.V8`). Firmware fills them in
  `host_api.cpp::fill_board_info` via `heap_caps_get_free_size` +
  `FlashFs::usage()`; the CLI `touchy board-info` prints them via
  `_fmt_bytes`.
- Stage 82 replaced the per-setting `ScreenLoadCmd` / `ScreenSleepTimeoutCmd`
  messages with one `SetPreferencesCmd { PreferencesFile prefs }`
  (`ProtocolVersion.V9`). Every value field on `PreferencesFile` is now
  `optional` (proto3 presence) so the host sends only what it wants
  changed; the device merges + persists. `file_version` stays
  non-optional (device-owned — the host must never set it). Two new prefs:
  `min_log_level` (uint32 holding a `LogPriority` value, default ERROR —
  drops device logs below it in `log_proto.cpp`) and `boot_delay_s`
  (early-boot `vTaskDelay` in `main.cpp` so a debug-log connection can
  attach). Firmware merge logic is `Prefs::apply_partial`
  (`firmware/main/prefs.cpp`), which fires side effects
  (`backlight_set_timeout`, `log_proto_set_min_level`, `screens_load`).
  Host: `TouchyClient.set_preferences` plus thin wrappers
  `screen_sleep_timeout` / `screen_load` / `set_min_log_level` /
  `set_boot_delay`; Rust mirrors all of these. CLI surface moved under a
  `touchy pref` group: `pref backlight-timeout`, `pref log-level`,
  `pref boot-delay`.

- **Stage 85 (Rust only, image cache).** `rust/touchy-pad/src/image_cache.rs`
  → `ImageCache::new(Arc<Touchy>)`; `set_cached_image(&[u8]) -> path`
  uploads each *distinct* image once, keyed by an xxh3-128 content hash
  (url-safe base64, no pad), to `T:host/icache/<HASH><suffix>` on the
  transient `T:` drive (`IMAGE_CACHE_ROOT`; moved from `R:` in Stage 87).
  LRU eviction at `MAX_IMAGE_CACHE` (128); the first call wipes
  `IMAGE_CACHE_ROOT`. Bytes are normalised via the shared
  `images::normalize_for_device` (extracted from `file_save`) so a cached
  asset is byte-identical to a direct `file_save`; `pad.rs` gained
  `file_write_raw` (chunked, no conversion) + `widget_save`.
  `ImageCache::with_max_dim` downscales to a fixed widget size (the
  OpenDeck plugin caps at the key px).

- **Stage 87 (dynamic images + `T:` transient drive).** A new *logical*
  `T:` drive: host code writes `T:...` for throwaway assets and the
  device resolves it to a PSRAM ramdisk (`heap_caps_get_total_size(
  MALLOC_CAP_SPIRAM) > 0`) else a flash scratch area —
  `firmware/main/fs/temp_fs.{h,cpp}` (`fs_for_drive('T')` + a buffered
  read-only LVGL `T` driver). Advisory `bool temp_is_flash`
  (`SysBoardInfoResponse`, `ProtocolVersion.V10`) lets the host throttle
  refreshes; `R:` stays explicit. Python `ImageSource`
  (`app/src/touchy_pad/api/images_dynamic.py`) owns a stable
  `T:dyn/<n>.bin` path (process-global monotonic `<n>`), accepted by
  `image()`/`image_button()` (also bare `PIL.Image`/`bytes`). It's
  harvested + bound on `screen_save`/`widget_save`/`user_screen_save`
  (one-shot `T:dyn/` wipe per connection); `.update(new=None)` re-renders,
  content-hash dedups, and rewrites the file — the **rewrite is the
  repaint** via the Stage 60 image registry, no `ActionChangeWidgetRef`.
  `every=` + `start()`/`stop()` is sugar over `.update()`. Rust
  `ImageCache` moved to `T:` (above). Sim mirrors all of it (`sim/fs.py`
  `T:` drive, `temp_is_flash=False`, broadened image-update gate).


- **Stage 86 (in-place ImageButton repaint).** Supersedes Stage 85's
  stub-rewrite repaint, which deleted the held button (lost keyUp) and
  raced under bursts (dropped a key). `ActionChangeWidgetRef.Behavior`
  gained `IMAGE_BUTTON_RELEASED` (3) / `IMAGE_BUTTON_PRESSED` (4): same
  `target_id` + `path` fields, but they address an `ImageButton` by its
  own `Widget.id` and swap one image *slot* in place via the Stage 60
  registry — firmware `widget_image_button_set_slot` (in
  `widget_builders.cpp`; `ButtonSlotBinding` now stores `widget_id`),
  wired in `widget_actions.cpp` behind `lv_throttled_post`. No widget
  rebuild → the pressed button keeps its touch state and still emits
  RELEASE. No `ProtocolVersion` bump (additive enum value on action wire
  content). The OpenDeck plugin (`rust/touchy-opendeck/`) reverted to
  plain per-key `ImageButton`s (stable id `opendeck_key_<k>` via
  `layout::key_widget_id`, host code on press+release) seeded with a
  cached blank `BLANK_BIN`; `Touchy::set_image_button_slot(id, pressed,
  path)` issues the action. Repaint = `cache.set_cached_image()` (bytes
  cross once) + the cheap slot action. **Slot selection** keys off press
  state the plugin tracks itself: a `HashSet<u8>` of held keys updated by
  the event task on PRESSED/RELEASED *before* dispatching key_down/key_up
  to OpenDeck, so the `set_image` it sends back observes the right state
  (held → pressed slot, idle → released). Firmware only shows the new
  bytes if that slot is currently displayed; otherwise it stages them
  for the next edge. Per-`(key, pressed)` last-path map skips redundant
  actions. `ActionHost` still reports the clickable widget's own
  `Widget.id`, so the per-key `ImageButton` id routes touch events.

- **Stage 90 (trackpad gestures via Actions).** The trackpad's
  hardwired `usb_hid_*` calls moved onto the Action mechanism: `Trackpad`
  gained five `repeated Action` lists — `on_left_click` (1-finger tap),
  `on_right_click` (2-finger), `on_middle_click` (3+-finger),
  `on_move` (1-finger drag), `on_scroll` (2-finger drag). Empty list =
  no-op; the Python DSL (`trackpad()` in `api/screens.py`) seeds sensible
  USB-HID defaults so behaviour is unchanged but fully overridable. The
  old `MouseMove` proto message was renamed **`Move`** (dropped `wheel`,
  `dx`/`dy` now `optional`); `MacroStep` gained `scroll_move` (tag 10,
  `Move`) alongside `mouse_move` (tag 7). A `Move` step with an unset
  axis pulls the trackpad's **live per-frame delta** for that axis. Host
  helpers: `macros.mouse_move()` / `macros.scroll_move()` (bare call =
  ambient delta). Firmware: clicks run on the **async** macro runner
  (`widget_run_actions`); the high-frequency move/scroll lists run
  **synchronously inline** with no inter-step delay via
  `macros_run_inline` / `widget_run_actions_inline`, threading the delta
  through `MacroMoveCtx`. `Widget.Version` 20→21.

## Build & test
Everything goes through Just; never run raw `idf.py` / `poetry` /
`protoc` unless a recipe is clearly missing:

```bash
just init              # one-time devcontainer setup
just build-proto       # regenerate Python + C nanopb bindings
just app-test          # pytest (proto bindings auto-rebuilt)
just app-lint          # ruff format + lint
just app-run -- ...    # invoke the `touchy` CLI inside Poetry
just firmware-build    # ESP-IDF build for the current board
just flash             # build + flash
just streamcontroller-run [--sim | --sim-headless]
```

CI: `.github/workflows/app-ci.yml` runs `build-app` on
ubuntu/windows/macos. **Windows has no libusb** — any code path that
touches `usb.core.find()` must guard against `NoBackendError`
(not just `ImportError`). See `app/src/touchy_pad/api/device.py` and
`app/src/touchy_pad/touchydeck/discovery.py` for the pattern.

## Justfile gotchas (learned the hard way)
- All recipe bodies use `#!/usr/bin/env bash` shebangs and must use
  **relative paths** — `justfile_directory()` produces `D:\a\...` on
  Windows, which bash interprets `\a` as `a`.
- Use `${SYS_PYTHON:-/usr/bin/python3}` inside recipes (read at runtime),
  not `{{sys_python}}` (expanded by Just at parse time).
- macOS BSD `paste` needs the explicit `-` stdin marker:
  `... | paste -sd: -`.

## Coding conventions
- **Device:** C++ via ESP-IDF (no Arduino), LVGL primitives only (no
  direct framebuffer writes). New subsystems → own `.cpp/.h` pair in
  `firmware/main/`. Long-running work → its own FreeRTOS task.
- **Host:** Python 3.11+, Poetry, ruff (format + lint), pytest. Public
  API lives under `touchy_pad.api`; the high-level entry is
  `touchy_pad.api.touchy_open()`.
- **Logging:** use `logging.getLogger(__name__)`. High-frequency RPC
  trace lines go on a child logger (e.g. `touchy_pad.client.rpc`) so
  callers can silence them independently. Python stdlib has no TRACE
  level — prefer child loggers over custom levels.
- **NotImplementedError in subclass-required methods:** prefer logging
  ERROR + returning a sensible default over raising, so optional
  StreamDeck features don't crash StreamController introspection.

## Hardware
- Display + touch panel ride a shared I²C-ish interface (board-specific);
  see `firmware/boards/<board>/`. GT911 multitouch on jc4827w543 /
  waveshare / elecrow / squixl / matouch_43. The squixl board uses an LCA9555 16-bit IO expander
  (I2C 0x20) for LCD reset, backlight enable, GT911 touch reset, and a
  bit-banged 9-bit SPI init bus for the ST7701S panel controller; the
  expander driver lives in `firmware/boards/squixl/board/lca9555.{h,cpp}`. The CYD boards (`esp32_2432s028rv3` 2.8" ST7789,
  `esp32_2432s024` 2.4" ILI9341) are an SPI panel over SPI2 +
  XPT2046 resistive single-touch over SPI3 (managed component
  `atanisoft/esp_lcd_touch_xpt2046`); BGR/INVERT/SWAP/MIRROR + backlight
  GPIO live in each `board/board_pins.h`, and the panel controller is
  selected there (`BOARD_LCD_CONTROLLER_ST7789` / `_ILI9341`). Note
  `reset_gpio_num` is `gpio_num_t` in IDF v6 — assign the enum directly.
- Optional haptics: DRV2605L on a separate I²C bus (not yet wired).
- USB-OTG controller exposes one IN/OUT bulk pair + one interrupt-IN
  endpoint for the vendor interface (no second IN for events — hence the
  mailbox-poll design).

## Workflow rules
- **Never auto-commit or push.** Make changes; let the user commit.
- `docs/design.md` is the source of truth for "what stage are we on" —
  update it when you finish a stage.
- The git submodule at `tools/StreamController` tracks branch
  `pr-touchypad`; `just streamcontroller-run` does
  `git submodule update --init --remote` first.
