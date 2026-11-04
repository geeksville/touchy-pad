# Plan: LED-matrix brightness (a safe default for `backlight_level`)

**Status: DONE — implemented as Stage lb18** (`docs/design.md`). The dimming
*mechanism* already existed (see "Current state" below); this plan drove the
remaining piece, the **10 % default for LED boards when `backlight_level` is
unset**. The file is kept as the record of the design and the reasoning.

## Goal / summary

touchy-pad drives two display families: **LCD** panels (with a real backlight)
and **LED matrices** (WS2812B). A WS2812B matrix at full duty is *very* bright —
bright enough that it is uncomfortable and plausibly hard on the LEDs / power
budget if left at 100 %. We want **one** brightness control that dims the whole
surface, reusing the existing Stage-94 `PreferencesFile.backlight_level` pref
(0 = off … 100 = full, 50 = half), and we want LED boards to **default to 10 %**
rather than 100 % when the host has never set the pref.

Non-goal: a hard *cap*. This plan only changes the default; an explicit
`touchy pref backlight-level 100` still means 100 %. (Whether to additionally
clamp the maximum for LED boards is an open question — see the end.)

## Current state (what already exists — do not re-invent)

The dimming mechanism is already implemented and committed:

* **`LEDChain` carries a brightness scalar.** `firmware/boards/common/leds/led_panel.h`
  (`LEDChain(int gpio, int w, int h, uint8_t brightness = 255)` +
  `set_brightness()`), and `led_panel.cpp::set_pixel()` multiplies each channel:
  `r = r * _brightness / 255` (same for g/b) — an integer 0..255 scalar.
* **The scalar is applied at the very last stage of the pipeline**, in
  `led_display.cpp::flush_cb()` → `s_chain->set_pixel(..., s_gamma[r8], …)` →
  `LEDChain::set_pixel()`. Because it happens *after* the RGB565→RGB888
  expansion and gamma LUT, and per output pixel, it dims **everything** —
  text, shapes, and **images alike**. This is *better* than the note's
  fallback ("images OK to skip"): no per-widget work is needed and there is no
  extra blending cost (one multiply per channel), unlike an LVGL opacity pass.
* **A percent-level entry point exists.** `led_display_set_brightness(level_0_100)`
  (`led_display.h` / `led_display.cpp`) converts % → 0..255, stores it (so it is
  remembered if called before the panel is up), calls `LEDChain::set_brightness()`,
  and invalidates the active screen to force a repaint (which the Stage-lb16
  wake path makes prompt).
* **LED boards already route `backlight_set()` through it.** `backlight_set(level)`
  in `esp32_s3_devkitc_1`, `esp32_s3_matrix` and `jc_esp32p4_m3` `board.cpp`
  forwards straight to `led_display_set_brightness(level)`.
* **The pref is already wired in.** `backlight_init()` reads
  `Prefs::instance().backlight_level()` and calls `backlight_set(s_level)`;
  `backlight_set_level()` persists it and re-applies when awake; the host surface
  (`TouchyClient.set_backlight_level`, CLI `touchy pref backlight-level`, Rust
  `Touchy::set_backlight_level`, and the `backlight_level` field of
  `SetPreferencesCmd`) is done.

So the "single brightness control scaling all colours, images included" is
**done**. The gap is purely the LED default.

### Why a panel-level scalar and not LVGL opacity

LVGL *does* have native dimming (`lv_obj_set_style_opa()` per widget / screen).
We deliberately did **not** use it:

* Opacity forces alpha blending during the render pass for every affected widget
  and does not cover everything uniformly (styles, images and layers interact);
  it also re-introduces the per-widget cost the note wanted to avoid.
* The panel-level scalar is one integer multiply per output channel in the flush
  path — the matrix is tiny (e.g. 32×8 = 256 px) and we already do a gamma lookup
  per pixel, so the marginal cost is negligible.
* It keeps dimming a *display-driver* concern, so LVGL never sees it and the
  widget tree / proto stay clean.

## The remaining change: a 10 % default for LED boards

Today `Prefs::m_backlight_level` defaults to **100** for every board
(`prefs.h`), and `save()` always writes `backlight_level` explicitly
(`prefs.cpp`). We need: *LED matrix boards* boot at **10 %** unless the host
explicitly set `backlight_level`, and LCD boards keep defaulting to 100.

Two things are required: a way to know the board is an LED matrix, and a way to
know whether the pref was *explicitly* set (so a real 100 % is not overridden).

### 1. LED-board signal — a weak board hook

Add `bool platform_is_led_matrix(void)`, declared in `firmware/main/platform.h`
and given a **weak** default `false` in `platform.cpp` — exactly mirroring the
existing `platform_is_touchable()` idiom. The three LED boards
(`esp32_s3_devkitc_1`, `esp32_s3_matrix`, `jc_esp32p4_m3`) add a strong override
returning `true`. LCD boards need no change.

*Chosen over* inferring "is LED" from `board_config` presence: a fresh LED board
has no `board_config` yet (it comes up headless until the host pushes
`pref from-template`), so presence would misclassify it; and it conflates "no
geometry programmed yet" with "not an LED board". A compile-time board fact is
correct from the first boot, matching how `is_multitouch` / `has_usb` work.

### 2. Track whether `backlight_level` was explicitly set

Add `bool m_backlight_explicit = false;` to `Prefs`:

* `load()` sets it `true` iff the stored file carried `has_backlight_level`.
* `set_backlight_level()` (host-driven) sets it `true`.
* `save()` writes `has_backlight_level = m_backlight_explicit` (rather than
  always `true`), so an auto default is never baked into flash and a later
  firmware change to the LED default still applies.

Then make the accessor return the **effective** value:

```cpp
uint8_t backlight_level() const {
    if (m_backlight_explicit) return m_backlight_level;
    return platform_is_led_matrix() ? 10 : 100;   // named constant, see below
}
```

`backlight_init()` already calls this accessor, so no change there. `to_proto()`
should report the effective value so `touchy pref get` shows 10 on a fresh LED
board. Put the `10` in a single named constant (e.g. `LED_DEFAULT_BRIGHTNESS`).

Net effect: fresh LED board → 10 %; host `pref backlight-level 100` → 100 %
persisted; LCD board → 100 % unchanged.

## Files & components

| Side | File | Change |
|---|---|---|
| `firmware/main/` | `platform.h` / `platform.cpp` | declare + weak-default `platform_is_led_matrix()` |
| `firmware/main/` | `prefs.h` / `prefs.cpp` | `m_backlight_explicit`, effective `backlight_level()`, conditional `save()`, `to_proto()` |
| `firmware/boards/` | `esp32_s3_devkitc_1/board/board.cpp`, `esp32_s3_matrix/board/board.cpp`, `jc_esp32p4_m3/board/board.cpp` | strong `platform_is_led_matrix()` override → `true` |
| docs | `docs/design.md` (new stage), `AGENTS.md`, memory-bank plans index | record the stage |

**No proto change, no `PreferencesFile.Version` bump** — this reuses the existing
`backlight_level` field and only changes a default. **No `app/` or `rust/`
change** — the host already has `set_backlight_level` / `pref backlight-level`;
the only host-visible difference is the value reported by `pref get` /
`GetPreferencesCmd` on a fresh LED board. **No simulator change** — the sim
treats `backlight_level` as a stored no-op (`sim/device.py`); mirroring the 10 %
default there is optional (see below).


## Phased implementation sequence

1. **Board hook.** Add `platform_is_led_matrix()` (weak default in
   `platform.cpp`) + strong `true` overrides in the three LED `board.cpp`s.
   Build one LED board + one LCD board to confirm it links.
2. **Prefs default.** Add `m_backlight_explicit`, the effective
   `backlight_level()`, the conditional `save()` write, and `to_proto()`.
   Point `backlight_init()` at the effective value (it already is — verify).
3. **Docs.** New `docs/design.md` stage section (next free `lb` number),
   `AGENTS.md` bullet, and the plans list in the memory bank.

## Testing strategy

Firmware has no unit-test suite, so validation is build + hardware:

* `just firmware-build` green for an LED board (`esp32_s3_devkitc_1` or
  `esp32_s3_matrix`) **and** an LCD board (`jc4827w543`) — the weak/strong
  symbol linkage is the main thing a build can catch.
* Hardware (LED matrix, e.g. `esp32_s3_matrix` + `pref from-template led-32x8`):
  fresh boot → visibly dimmed (10 %), not full; `touchy pref backlight-level 100`
  → full and survives reboot (persisted as explicit); `pref backlight-level 0`
  → off; auto-sleep wake restores the remembered level (unchanged).
* LCD board regression: default stays 100 %; explicit set still works.
* `just app-test` should stay green unchanged (no host/proto change).

## Risks / open questions

* **Is 10 % the right number, and is it perceptually right?** The scalar
  multiplies the **gamma-corrected** value linearly, so low levels dim more
  aggressively than a "linear light" model would suggest. 10 % may look darker
  than expected. If so, the fix is to fold brightness into the gamma LUT (dim in
  the perceptually-uniform domain) rather than a second multiply — worth
  measuring on hardware before tuning. Keep 10 in one named constant so it is
  trivial to change.
* **Default vs. cap.** This plan lowers the *default* only; a host can still ask
  for 100 %. If the brightness genuinely risks damaging hardware, we may also
  want a per-board *maximum* (clamp in `led_display_set_brightness()`). That is a
  larger policy decision (it would make a host-set 100 % silently become, say,
  50 %) — left open, not implemented here.
* **`m_backlight_explicit` persistence subtlety.** Because `save()` now
  conditionally omits `backlight_level`, a device configured before this change
  already has `has_backlight_level = true` (explicit 100) in its file — so
  existing LED devices keep 100 % until the user re-sets the pref. If we want
  them to pick up 10 %, that needs a one-shot migration (e.g. version-gated
  reset) — decide whether that is worth it; the conservative default is "no
  migration".
* **Simulator parity (optional).** The sim ignores brightness; a host test could
  assert `GetPreferencesCmd` reports 10 on an LED-configured sim device. Only
  worth doing if we want a host-visible regression test; the default itself lives
  device-side.

## Scope recap

* **Touches:** `firmware/main/` (prefs + platform) and three
  `firmware/boards/*/board.cpp`.
* **Does not touch:** `proto/` (no wire change, no `PreferencesFile.Version`
  bump), `app/`, `rust/`, the simulator.
* **`just` recipes that validate it:** `just firmware-build` (LED + LCD board),
  `just app-test` (must stay green), and `just firmware-reconfigure` when
  switching boards.

