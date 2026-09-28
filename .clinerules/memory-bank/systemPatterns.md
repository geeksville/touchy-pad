# System Patterns — touchy-pad architecture

## Top-level shape

| Layer | Where | Responsibility |
|---|---|---|
| Host apps | `rust/touchy-opendeck`, `tools/StreamController` | End-user features built on the APIs |
| Host APIs | `app/src/touchy_pad/api/` (Python), `rust/touchy-pad/` (Rust) | Screen DSL, macros, image cache, transports, device lifecycle |
| Simulator | `app/src/touchy_pad/sim/` | In-process/TCP fake device (+ optional Qt window); wire-accurate framing, HTTP server on 8083 |
| Wire protocol | `proto/` + `app/.../_proto/` + `firmware/main/proto/` | `MAGIC(0xA5 0x5A)│LEN(u16 LE)│payload│CRC8`, one `Command`→one `Response` |
| Firmware | `firmware/main/` (subsystems), `firmware/boards/<board>/` (bring-up) | LVGL rendering, USB HID, RPC dispatch, filesystems, prefs, WiFi/HTTPS |
| Board support | `firmware/boards/*` + `firmware/boards/common/*` | Panel/touch bring-up, shared PWM backlight, LED chains |

## Firmware architecture

**Boot order** (`firmware/main/main.cpp::app_main` — deliberately thin):

1. `log_proto_start()` — hook `esp_log_set_vprintf()` before any other log so
   records can tunnel to the host (no-op if `CONFIG_TOUCHY_LOG_OVER_PROTO=n`).
2. `heap_caps_register_failed_alloc_callback()` + `coredump_report_check_and_log()`.
3. `usb_hid_init()` (only when `CONFIG_SOC_USB_OTG_SUPPORTED`) — bring USB up
   *before* the slow display/touch init so the host port doesn't time out.
4. `host_api_start()` — registers every available `HostApiLink`, one dispatcher
   task each.
5. `fs_init()` → mount `F:` LittleFS (+ `R:` PSRAM ramdisk, `T:` transient).
6. `Prefs::instance().load()` → apply persisted settings.
7. `display_create()` + `Display::init()` (falls back to `HeadlessDisplay` on
   failure → device still serves the protocol, `board-info` reports 0×0).
8. `touch_init(disp)` → `log_proto_enable()` → `fs_register_lvgl_drivers()`.
9. `screens_set_touch(tp)`; touch-resets-backlight callback.
10. `screens_load(last_viewed_path or default)` — preference order:
    last-viewed screen (persisted pref) → first registered screen → compiled-in
    fallback generated from `proto/default_screen.json`.
11. `network_apply(prefs.network())` if `CONFIG_TOUCHY_WIFI`.
12. `vTaskDelete(NULL)` — everything afterwards is event/task driven.

**Subsystem files** (`firmware/main/`, one concern per `.cpp/.h` pair):
`display.{h,cpp}` (Display ABC + HeadlessDisplay), `board.h` (board seam:
`board_init`, `backlight_set(0..100)`), `touch.h`, `backlight.{h,cpp}`
(auto-sleep manager), `screens.{h,cpp}`, `prefs.{h,cpp}`, `macros.{h,cpp}`,
`log_proto.{h,cpp}`, `lv_throttled.{h,cpp}` (rate-limited LVGL posting from
non-LVGL tasks), `usb_hid.{h,cpp}` / `usb_hid_stub.cpp`, `platform.{h,cpp}`
(`platform_get()` → `{is_multitouch, has_usb}`), `protobuf.h` (`PbMessage<T>`
RAII), `tc_tag.h`, `debug.{h,cpp}`, `coredump_report.{h,cpp}`.

**Subdirectories:**

| Dir | Contents |
|---|---|
| `api/` | `host_api.{h,cpp}` (dispatcher + `host_api_dispatch_message` seam), `host_api_link.h` base, `vendor_link.*` (USB vendor bulk), `serial_link.*` (USB-CDC-ACM), `uart_link.*` (hardware UART) |
| `widgets/` | `widget_builders.{h,cpp}` (id→obj registry, widget-ref resolution), `screen_layout.{h,cpp}` (`apply_rect`/`apply_grid_cell`, `grow_*`), `widget_actions.{h,cpp}` (ActionHost/Macro/Device), `widget_animations.{h,cpp}`, `widget_styles.{h,cpp}`, `widget_property.{h,cpp}` (Stage lb12 overrides), `trackpad_widget.{h,cpp}` (all gestures), `image_mmap.{h,cpp}`, `log_line.*`, `fps_widget.*`, `force_render_widget.*` |
| `fs/` | `fs.{h,cpp}` (drive dispatch `fs_for_drive`), `flash_fs.{h,cpp}` (LittleFS + `usage()`), `ram_fs.{h,cpp}` (PSRAM-first hashmap), `temp_fs.{h,cpp}` (Stage 87 `T:`) |
| `net/` | `network.{h,cpp}` (WiFi/mDNS, `mtls_provisioned()`), `http_api.{h,cpp}` (`esp_https_server`, `POST /touchy/api/v1/command`), `json.{h,cpp}` (cJSON ↔ proto) |

**Board seam (Stage lb7):** `Display` is a C++ ABC (`Display::init()` runs
`hw_init()` then a virtual `post_init()`); each board component defines a strong
`Display *display_create(void)` factory returning a local subclass of `Display`
(LED boards share `LEDMatrixDisplay`; LCD boards each have their own
`BoardLCDDisplay`, the CYD family shares `cyd_common/`). Headless is a `Display`
subclass rather than a special case in `main.cpp`.

**Board self-description:** every board dir has a one-line `target` file
(`esp32` / `esp32s3` / `esp32p4`) read by `just firmware-reconfigure`; USB
capability is keyed off `CONFIG_SOC_USB_OTG_SUPPORTED`, never on a custom flag.
CYD boards compile `firmware/boards/cyd_common/*.cpp` and contribute only
`board/board_pins.h` (which selects ST7789 vs ILI9341 at compile time via
`BOARD_LCD_CONTROLLER_*`). Shared board code lives in
`firmware/boards/common/` (PWM backlight, LED panels/chains).

### Supported boards (dir → chip → display/touch → link)

| Board dir | Chip | Display | Touch | Host link |
|---|---|---|---|---|
| `jc4827w543` | esp32s3 | 480×272 NV3041A QSPI | GT911 | USB |
| `jc4827w543r` | esp32s3 | same, resistive variant | (resistive) | USB |
| `waveshare_s3_lcd_7b` | esp32s3 | 800×480 ST7262 RGB | GT911 | USB |
| `elecrow_s3_lcd_7` / `_adv` | esp32s3 | 800×480 RGB | GT911 | USB |
| `elecrow_p4_lcd_7` | esp32p4 | 1024×600 MIPI-DSI | GT911 | USB (no WiFi chip) |
| `matouch_43` | esp32s3 | 800×480 RGB | GT911 | USB |
| `squixl` | esp32s3 | ST7701S (bit-banged init via LCA9555) | GT911 | USB |
| `esp32_2432s028rv3` | esp32 | 320×240 ST7789 SPI | XPT2046 (resistive, single-touch) | UART0 @115200 (CH340) |
| `esp32_2432s024` | esp32 | 320×240 ILI9341 SPI | XPT2046 | UART0 |
| `esp32_s3_devkitc_1`, `esp32_s3_matrix`, `jc_esp32p4_m3` | s3 / p4 | LED panels or headless | — | USB/UART |

## Transports & framing

* **One frame format on every byte stream:** `MAGIC(0xA5 0x5A)` +
  `LEN(u16 LE)` + payload + `CRC8`. One decoder per side:
  `api/host_api.cpp` (`HostApiLink`), `app/.../transport.py`
  (`_StreamFramedTransport` / `_FrameDecoder`), `rust/.../transport.rs`
  (`FrameDecoder`). Serial transports always run 115200 8N1 and carry *only*
  protocol frames — device text logs ride the `LogRecord` tunnel instead.
* **HTTP(S) is the exception:** the body is a *bare* serialized
  `Command`/`Response` (no MAGIC/LEN/CRC8 — HTTP delimits and integrity-checks
  it). The handler calls the same `host_api_dispatch_serialized()`, so semantics
  are identical. `Content-Type: application/json` selects canonical
  protobuf-JSON in/out (Stage lb13) on the same URI.
* **Multi-link (Stage LB5):** `host_api_start()` registers all *available*
  links, one dispatcher task each. "Last used wins" — responses go back on the
  originating link, events go to whoever polls; `s_active_link` tracks the most
  recent for future unsolicited pushes. Three **independent** Kconfig flags
  (`CONFIG_TOUCHY_PROTO_OVER_VENDORUSB` / `_CDCACM` / `_UART`), each only
  instantiated when the board also has the backing hardware.
* **Events are polled:** `EventConsumeCmd` in a loop until `RESULT_NOT_FOUND`.
  When the queue is empty but a log record is pending, the same response
  carries `LogRecord` (payload tag 5) instead — the Stage 64.1 tunnel. (The
  S3 USB-OTG controller exposes only 5 IN endpoints, so there is no room for a
  real event mailbox; the interrupt-IN endpoint `0x85` just signals "events
  available".)
* **USB composite class:** CDC-ACM + HID (mouse + keyboard via report IDs 1/2,
  plus a Consumer-Control report ID 3 from Stage 93) + vendor-class bulk pair
  (command/response) + the interrupt-IN mailbox. VID/PID = `0x303A / 0x8369`
  (assigned in Stage 58).

## Filesystem drives (host-visible paths)

| Drive | Backing | Persists | Used for |
|---|---|---|---|
| `F:` | LittleFS flash partition | yes | Screens (`F:host/s/`), user pages (`F:host/uscr/`), widget files (`F:host/widgets/`), long-lived images, mTLS certs (`F:tls/`) |
| `R:` | PSRAM ramdisk (`RamFs`, falls back to internal SRAM) | no | Explicitly volatile assets |
| `T:` | PSRAM ramdisk, else flash scratch (Stage 87) | no | Throwaway assets: dynamic images (`T:dyn/<n>.bin`), image caches (`T:host/icache/<hash>.bin`) |

Paths are always drive-prefixed (`fs_for_drive('T')`); the device refuses
unprefixed paths. Constants live in `app/src/touchy_pad/paths.py` (Python),
`rust/touchy-pad/src/lib.rs` (Rust) and the firmware's path helpers.

## Screen / widget / action model

* **`Screen`** = name + four LVGL layers (`active`, optional `top`/`sys`/
  `bottom`). Only `active` is swapped by `lv_screen_load`; the other layers
  persist and are only touched when the incoming `Screen` actually carries them
  (an explicit empty `Widget` clears a layer). Versioning lives on the *root
  `Widget`* of each file (`active.version`) — there is **no** `Screen.Version`
  enum in the schema (an `AGENTS.md` claim to the contrary is stale — see
  `activeContext.md`).
* **`Widget`** is a big oneof (`kind`) plus placement (`rect` / `grid_cell` /
  flex), `grow_x`/`grow_y` sizing, `style[]`, `action` lists (`on_click`,
  `on_press`/`on_release`, …), `widget_ref`, animations.
* **Widget refs (Stages 54/57):** `Widget.widget_ref{path}` splices a standalone
  widget file at load time; `ActionChangeWidgetRef{BY_PATH|NEXT|PREVIOUS, path,
  target_id}` retargets a live `widget_ref(id=...)` — that's how the default
  chrome's `widget_ref(id="page")` pages through `F:host/uscr/*.pb`.
* **Actions (Stages 24/67/90-95):** `ActionHost` (host event code / inline
  Python callback), `ActionMacro` (device-side macro steps → USB HID),
  `ActionDevice` (screen switch, widget-ref change, backlight, reboot, image
  slot swap…). `RunActionsCmd` (Stage 71) feeds host-supplied `Action`s through
  the exact same runner, which is how `show_user_screen` /
  `set_image_button_slot` work.
* **Gestures are Actions (Stages 90-95).** `Trackpad` carries repeated `Action`
  lists — `on_left_click`/`on_right_click`/`on_middle_click`, `on_move`,
  `on_scroll`, `on_left/right/up/down_swipe`, `on_zoom_in`/`out`,
  `on_cw_twist`/`on_ccw_twist`, `on_hold`, `on_hold_release` — plus optional
  tuning knobs. **Each fancy gesture has a master enable field**
  (`swipe_initial_distance`, `zoom_initial_distance`, `twist_initial_angle`,
  `hold_time`): unset ⇒ feature is off with zero per-frame cost, and detection is
  *additive* (never suppresses `on_move`/`on_scroll`), so defaults behave exactly
  like Stage 90. Click macros run on the **async** runner; high-frequency
  move/scroll/swipe/zoom/twist use `macros_run_inline` with a `MacroMoveCtx`
  carrying the live per-frame delta (an unset axis on a `Move` step pulls the
  ambient delta — how `mouse_move()` / `scroll_move()` / `zoom_move()` get their
  values). Physical quantity choices matter: pinches measure the **inter-touch
  span** (order-invariant) and twists the **undirected line angle** wrapped to
  (−90°, +90°], so GT911 slot swaps are harmless.
* **Preferences (Stage 82+):** one `SetPreferencesCmd{PreferencesFile prefs}`;
  every value field is `optional` (proto3 presence) so the host sends only what
  changes and the device merges (`Prefs::apply_partial`) + persists. Two classes
  of side effect: **live** (`backlight_set_level`, `log_proto_set_min_level`,
  `screens_load`, `network_apply`) and **boot-time only** (`BoardConfig` LED
  panel geometry, `boot_delay_s`). `file_version` is device-owned — the host
  must never set it. Nested `NetworkConfig` merges *per sub-field* (setting only
  `wifi_ssid` keeps the stored `psk`).
* **Runtime property overrides (Stage lb12):** `SetPropertyCmd{widget_id,
  property_name|property_id, bool|int|string|color|point}` sets an LVGL property
  via `CONFIG_LV_USE_OBJ_PROPERTY{,_NAME}`. Overrides are **session-scoped and
  sticky**: a `(widget_id, ident)→value` table re-applies whenever the widget is
  (re)built, so setting a property *before* the widget exists still works.
  An unset `value` removes the override. Python needs `Color`/`Point` wrappers
  to disambiguate from int; the sim logs a WARN (no LVGL property API) and
  returns OK.
* **Image plumbing:** uploads are normalised to LVGL `.bin` by Pillow host-side
  (`images.normalize_for_device`); `ImageCache` (Python Stage 100, Rust Stages
  85/87) is content-addressed (blake2b-128 Python / xxh3-128 Rust) onto `T:`,
  LRU-capped at 128, wiping the cache root on first use. Dynamic images
  (`ImageSource`, Stage 87) own a stable `T:dyn/<n>.bin` path and repaint by
  *rewriting the file* (the Stage 60 image registry notices); `ImageButton`
  per-slot repaint uses the Stage 86 `ActionChangeWidgetRef` RELEASED/PRESSED
  action, which swaps one slot in place without rebuilding the widget — so a
  held key keeps its touch state and still emits RELEASE.

## Wire-format versioning (current values)

| Schema | File | Current | Meaning |
|---|---|---|---|
| `Widget.Version.CURRENT` | `widgets.proto` | **28** | Bumped for every widget-level schema change (lb11: 27→28) |
| `SysBoardInfoResponse.ProtocolVersion.CURRENT` | `touchy.proto` | **13** | Bumped for new *commands*/protocol changes (lb12: 12→13) |
| `PreferencesFile.Version.CURRENT` | `preferences.proto` | **9** | Bumped for pref-field changes (lb10: 8→9) |

Rules of thumb learned here: **additive enum values on existing action
payloads** do *not* need a bump (Stage 86); a new *command* does; a pure rename
keeping the same tag does not (Stage 95 `tap_max_ms`→`tap_time`); a prefs
reshape does. `CURRENT` is an alias that must be listed **first** in
`touchy.proto` so prost (Rust) can see it. Firmware validates the root widget
version on load and deletes incompatible files rather than mis-rendering.

## Host architecture (Python, `app/src/touchy_pad/`)

| Module | Role |
|---|---|
| `api/__init__.py` | The **only** supported public surface; re-exports `Touchy`, `TouchyClient`, `touchy_open`, `touchy_get_pad_ids`, the whole screen DSL, macros, `ImageCache`, `Color`/`Point`, sim-registry helpers, path constants |
| `api/device.py` | `Touchy` — high-level op-oriented API (`screen_save`, `user_screen_save`, `show_user_screen`, `set_image_button_slot`, `set_property`, `file_save`, …); harvests inline `host_action(on_event=…)` callbacks |
| `api/_events.py` | Inline-callback registry; auto-allocates codes from `AUTO_CODE_BASE = 0x10000` (manual codes stay below) |
| `api/client.py` | `TouchyClient` — thin 1:1 RPC over a transport; `LogRecord` dispatch to the `touchy_pad.device` logger; RPC traces on child logger `touchy_pad.client.rpc` |
| `api/screens.py` | Screen/widget DSL (`Screen`, `row`/`col`/`grid`/`cell`/`absolute`, `grow`, `style`, `transition`, widget factories, `build_default_screen`, `build_demo`, `build_setup_screen[_touchless]`, `build_user_pages`) |
| `api/macros.py` | Macro DSL (`key_tap`, `key_down/up`, `mouse_move`, `scroll_move`, `zoom_move`, `mouse_button_*`, `consumer_key`, `type_text`, `delay`) |
| `api/_transport*.py` | `_transport.py` (base + `DeviceNotFoundError`), USB vendor, `_transport_serial.py`, `_transport_net.py` (TCP sim), `_transport_http.py` (bare-protobuf HTTP/S + mTLS `SSLContext`) |
| `api/image_cache.py`, `images.py`, `images_dynamic.py`, `lvgl_image.py` | Content-addressed cache, Pillow normalisation, dynamic `ImageSource`, LVGL `.bin` writer |
| `api/mtls.py`, `props.py`, `errors.py`, `hid_keys.py`, `sim_registry.py` | mTLS provisioning, `Color`/`Point`, exceptions, keycodes, sim-device registry |
| `cli.py` | `touchy`: `board-info`, `events`, `file-reset/delete/save`, `writefiles`, `screen {load,wake,push,demo,init}`, `pref {backlight-timeout,backlight-level,log-level,boot-delay,wifi-set-ssid,wifi-set-psk,provision-mtls,json-get,json-set,from-template}`, `property set`, `touchpad {image,gif}`, `simulator`, `init`, `update`, `reboot-bootloader` |
| `pages/` | Shipped example screens (`trackpad.py`, `test.py`) |
| `sim/` | `device.py` (`SimDevice.handle_command`), `server.py` (TCP), `http_server.py` (plaintext HTTP on 8083), `fs.py` (in-memory `F:`/`R:`/`T:`), `widgets.py` (layout engine), `transport.py`, `window.py` (Qt view) |
| `touchydeck/` | StreamDeck shim: `deck.py`, `layout.py`, `discovery.py` — `install()` must be called **explicitly**; no import side effects |
| `paths.py`, `update.py`, `assets/templates/*.json` | Path constants, `touchy update` flashing, prefs templates (`led-32x8.json`, `neopixel-1.json`, `led-96x8-chain.json`) |

CLI global options: `--debug`, `--url` / `TOUCHY_URL`, `--sim`, `--sim-headless`,
`--sim-gui`, `--sim-remote [HOST:PORT]`.

## Rust workspace (`rust/`)

Workspace resolver 2, edition 2024, rust-version 1.85; `prost`/`prost-build`/
`nusb`(+tokio)/`tokio`/`image`/`clap`/`async-trait`; version synced from `VERSION`
by `just _rust-sync-version`.

* `touchy-pad` — async client: `pad.rs` (`Touchy`), `client.rs`, `transport*.rs`
  (USB / net / serial behind the `serial` feature), `image_cache.rs`,
  `images.rs`, `discover.rs` (USB **and** UART autodiscovery), `error.rs`,
  `proto.rs`, path constants in `lib.rs`.
* `touchy-demo` — CLI demo app.
* `touchy-opendeck` — OpenDeck plugin: one `ImageButton` per key
  (`opendeck_key_<k>`), blank-image seed, host code on press+release, tracks a
  `HashSet<u8>` of held keys so `set_image` observes the right slot; repaint =
  `cache.set_cached_image()` + the cheap slot-swap action.

## Architectural patterns worth reusing

1. **One decoder / one dispatcher per side.** Never add a second framing
   implementation; route new transports through `HostApiLink` (device) or
   `_StreamFramedTransport` (host).
2. **The device is a renderer; the host owns semantics.** New UI features go in
   the proto + host DSL, not in firmware conditionals.
3. **Opt-in by field presence.** Master-enable fields keep new features at zero
   cost and preserve backwards compatibility.
4. **Additive, not modal.** New gesture detectors run alongside existing ones.
5. **Capability flags, not board `#ifdef`s** for behaviour differences
   (`CONFIG_SOC_USB_OTG_SUPPORTED`, `platform_get()`, `board-info` fields).
6. **Registries keyed by `widget_id`** for anything the host addresses later
   (image registry, property overrides, `widget_ref` targets) — rebuilt each
   screen load and committed via `widget_refs_reset_pending()` /
   `widget_refs_commit()`.
7. **RAII for nanopb** (`PbMessage<T>`) and **heap-owned style structs** freed
   from an `LV_EVENT_DELETE` callback (LVGL stores pointers, not copies).
8. **Never call LVGL from a foreign task** without `lvgl_port_lock`; use
   `lv_throttled_post` for high-frequency callbacks.
9. **Generated artefacts stay generated** (`app/.../_proto/`,
   `firmware/main/proto/`, `firmware/main/default_screen_pb.h`) — regenerate with
   `just build-proto` / `just gen-default-screen`.
10. **Fail soft, log loudly.** Headless display on bring-up failure, `WARN`+OK
    for unimplemented sim commands, `logging.getLogger(__name__)` everywhere,
    device logs tunneled to the host.

## Critical implementation paths (look here first)

* **Upload + show a screen:** `screens.py` DSL → `Touchy.screen_save` →
  `FileWrite` stream → `firmware/main/fs/*` + `screens.cpp`
  (`screens_notify_file_changed`) → `widget_builders.cpp` builds the LVGL tree.
* **Touch → HID macro:** LVGL indev → `trackpad_widget.cpp` classifier →
  `widget_run_actions[_inline]` (`widget_actions.cpp`) → `macros.cpp` →
  `usb_hid.cpp`.
* **Any host RPC:** `api/client.py` → transport framing → `api/host_api.cpp`
  `dispatch()` → handler → `Response`.
* **Device log to host:** `log_proto.cpp` (vprintf hook + ring) →
  `EventConsumeCmd` response payload tag 5 → `TouchyClient._dispatch_log_record`
  / Rust `dispatch_log_record`.
* **Per-key StreamDeck repaint:** host renders bytes → `ImageCache` (`T:` hash
  path) → `set_image_button_slot` → `ActionChangeWidgetRef` RELEASED/PRESSED →
  `widget_image_button_set_slot` → `widget_image_registry_notify` repaint.
* **New board:** copy an existing `firmware/boards/<board>/` dir → set `target`,
  `sdkconfig.defaults`, `board/board_pins.h`, `board/CMakeLists.txt`,
  `idf_component.yml`; write `board.cpp`/`display.cpp`/`touch.cpp` (or reuse
  `cyd_common/` / `boards/common/`); then
  `just firmware-reconfigure <board> && just firmware-build`.
