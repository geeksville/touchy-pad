# UI design guide

The basic GUI is layed out via [LVGL xml](https://viewer.lvgl.io/?repo=geeksville%2Ftouchy-pad%2Ftree%2Fmain%2Fui).  
Note: my test layout XML (hardwired) currently lives [here](/ui).  You can view this 'hello world' XML on your dev PC by running https://github.com/lvgl/lvgl_editor/releases/tag/nightly appimage and opening that directory.

## General thoughts

NOTE: much of this section is now out-of-date because I suspect we'll just be able to use LVGL XML to define all layouts (with the full generality that provides).  We'll make a custom HIDButton widget that can also do 'macro playback' of HID events in addition to its regular button behavior.  And a custom TouchpadWidget.  Otherwise anything allowed by LVGL XML is fine.

* Allow host to send protocol buffer commands to add screens and widgets.  We will store that data in some TBD format (protobuf based?  lvgl xml?) flash filesystem (so the config is persistent without even an app provisioning)
* any interaction with widgets by the user will generate protobuf events on our private USB characteristic
* Some interactions (touchpad widget or button presses) might generate standard USB HID (mouse/keyboard) events (so many behaviors can be used by the user without even using our helper app)
* FIXME: possibly use lvgl xml for this?  Or just handcode in C? See https://lvgl.io/docs/open/xml/integration/xml for ideas.
* The device code is entirely agnostic wrt the layour of the xml provided by the host, which keeps the device code small.  This also lets us put most of the smarts into the sister python api/app.
* FIXME: Can TouchpadWidget instead be a Component rather than a Widget?  It sounds like the glue code is easier in that case.

## General layout
* A small number of buttons are standard and always visible on the screen (upper left by default?).  They include things like "Next screen", "Prev Screen".
* All other screen components are screen specific and provided based on the (xml?) sent by the host.  Most commonly it will include a large rectangular TouchpadWidget and possibly some number of app specific Widgets.  

## Layers

LVGL exposes four per-display layers that we mirror in the `Screen`
protobuf: `active`, `top`, `sys`, and `bottom`.  `lv_screen_load()`
swaps only the active layer; the other three are *persistent* across
screen changes, which makes them a natural home for navigation
chrome that should stay put while the user flips between screens.

* `active` — always present.  Rebuilt from scratch on every
  `screen_load`.  This is what the DSL's `Screen.add(...)` /
  `screen += widget` targets.
* `top` / `sys` / `bottom` — `optional` in the protobuf.  Semantics:
  * **Unset** (the default): the firmware leaves that LVGL layer
    *untouched*, preserving whatever the previous screen left there.
  * **Set to a populated `Layer`**: the firmware calls
    `lv_obj_clean()` on the LVGL layer and rebuilds it from the
    layer's widgets.
  * **Set to an empty `Layer()`**: the explicit "clear this layer"
    payload — the firmware still cleans the LVGL layer, just with
    nothing to put back.

In the Python DSL use `Screen(..., top=Layer(...))` (or the
`screen.add_top(...)` / `add_sys(...)` / `add_bottom(...)` helpers)
to populate a persistent layer.  Per-layer layouts are independent:
`active` can be a grid while `top` is a flex row, for example.

## Image representation
On the wire and on the device filesystem, images are stored in LVGL's
native `.bin` format (12-byte header + pixel planes; RGB565A8 by
default, so transparency is supported). The Python host package
(`touchy_pad.client.TouchyClient.file_save`) auto-converts any
Pillow-readable input — BMP, PNG, JPEG, GIF, WebP — to that format
before upload, so callers can pass arbitrary image bytes:

```python
client.file_save("images/avatar.png", open("avatar.png", "rb").read())
client.file_save("images/icon.bmp",   make_smiley_bmp())
```

Both end up as LVGL `.bin` payload on flash. Already-converted `.bin`
blobs and non-image data pass through unchanged. `Image` /
`ImageButton` widgets reference these files by their on-disk path
(e.g. `asset="images/avatar.png"`) regardless of the original source
format. `Image` / `ImageButton` also accept `scale` (1.0 = 100 %) and
`rotation` (degrees) for runtime transforms; `ImageButton` additionally
takes `pressed_asset` / `pressed_scale` / `pressed_rotation` for a
distinct pressed-state look. Compression
(e.g. [LVGL LZ4](https://lvgl.io/docs/open/libs/image_support/lz4)) is
not yet wired up.

## Widgets
* Button - Client can set a normal and pressed image.  If no pressed image is provided, device will default to just invert the button while pressed.  Might also send a series of HID events (to allow macro playback)
* Checkbox - Similar to button but also has a 'checked' state that can be set by the client
* Touchpad - A rectangular region, presses inside this region generate mouse/touch events on the host.  Can contain a background image and and optional touch image (to mark finger positions while touching)
* Slider

## Touchpad Widget

### Gestures

FIXME Perhaps the gesture stuff I've been hand coding could be do instead with https://lvgl.io/docs/open/examples/others/gestures?

## Styles

Each `Widget` carries a `repeated Style styles` field. Treat one
`Style` message as ≈ one `lv_style_t` instance on the device: every
populated scalar (`bg_color`, `radius`, `border_w`, `pad`,
`text_color`, `recolor`, `recolor_opa`, `transform_width`) maps to
exactly one `lv_style_set_<prop>` call inside
`build_lv_style()` in [firmware/main/screens.cpp](../firmware/main/screens.cpp);
fields left unset (`Style.HasField(...)` is false) are skipped and
inherit the theme.

Every visual field is wire-level `optional`, so explicit zero values
(`bg_color=0x000000`, `transform_width=0`) round-trip faithfully
instead of being indistinguishable from "unset".

Each `Style` also carries a `for_state` selector — the OR of any
`LvState` bits (state + part). The firmware passes it verbatim to
`lv_obj_add_style(obj, st, (lv_style_selector_t)for_state)`. Both
`LV_STATE_DEFAULT` and `LV_PART_MAIN` are 0, so leaving `for_state`
unset targets the main part in the default state — the common case.

`LvState` is a flat enum union of two LVGL concepts that share the
selector bit-field:

| Bits | Kind | Examples |
|---|---|---|
| `0x0001..0x0080` | states (`lv_state_t`) | `LV_STATE_PRESSED`, `LV_STATE_CHECKED`, `LV_STATE_DISABLED` |
| `0x010000..0x0F0000` | parts (`lv_part_t`) | `LV_PART_KNOB`, `LV_PART_INDICATOR`, `LV_PART_SCROLLBAR` |

The host DSL re-exports them as `STATE_*` / `PART_*` constants from
[`touchy_pad.api.screens`](../app/src/touchy_pad/api/screens.py).

### Cascade rules

Stack as many `Style`s on a widget as you need; the firmware adds them
in array order via `lv_obj_add_style`. Where two attached styles set
the same property under the same selector, the **later-added** entry
wins. Where their selectors differ but both match the current widget
state, LVGL's normal state-precedence rules apply — see the
[LVGL styles overview](https://lvgl.io/docs/open/common-widget-features/styles/overview).

### Example: pressed-state highlight

The smiley image-button in `build_demo_screen` shows the pattern
end-to-end:

```python
from touchy_pad.api.screens import (
    image_button, host_action, style, transition,
    STATE_PRESSED, PROP_TRANSFORM_WIDTH, PROP_IMAGE_RECOLOR_OPA,
)

image_button(
    "smile",
    asset="images/smiley.png",
    on_click=host_action(0x103),
    style=[
        # Default-state style binds the transition; LVGL uses it for
        # both entering and leaving the pressed state.
        style(transition=transition(
            props=[PROP_TRANSFORM_WIDTH, PROP_IMAGE_RECOLOR_OPA],
            duration_ms=200,
        )),
        # Pressed state: widen by 20 px and tint black at ~30 % opacity.
        style(
            transform_width=20,
            recolor=0x000000,
            recolor_opa=76,        # ≈ LV_OPA_30
            for_state=STATE_PRESSED,
        ),
    ],
)
```

While the user holds the smiley, LVGL animates its width up by 20 px
and fades in a black image-recolor over 200 ms; on release the
animation plays in reverse. The visual matches LVGL's stock
[`lv_example_imagebutton_1`](https://github.com/lvgl/lvgl/blob/master/examples/widgets/imagebutton/lv_example_imagebutton_1.c)
example.

### Transitions

A `Style` may carry an optional `transition` field — a
`Transition` message that mirrors `lv_style_transition_dsc_t`. When
the parent style is added to or removed from the widget's selector
match (e.g. entering or leaving `STATE_PRESSED`), LVGL interpolates
every property in `Transition.props` from the previous value to the
new one along the chosen `path` over `duration_ms`, starting after
`delay_ms`.

`Transition.props` references curated wire-stable `StyleProp` values
(re-exported as `PROP_BG_COLOR`, `PROP_TRANSFORM_WIDTH`,
`PROP_IMAGE_RECOLOR_OPA`, …); the firmware translates each to the
corresponding `LV_STYLE_*` constant at decode time. `Transition.path`
selects an `AnimPath` easing curve (`ANIM_PATH_LINEAR`,
`ANIM_PATH_EASE_IN_OUT`, `ANIM_PATH_OVERSHOOT`, `ANIM_PATH_BOUNCE`,
`ANIM_PATH_STEP`, …) that maps onto LVGL's built-in `lv_anim_path_*`
callbacks.

Two common patterns:

* **Symmetric in/out** — attach the transition to the default-state
  Style (`for_state = 0`). LVGL plays the same animation both ways.
* **Asymmetric** — attach *different* transitions to the default and
  pressed-state Styles (e.g. fast 100 ms into-press, slow 500 ms
  out-of-press). Each `Style.transition` controls the *entering* edge
  of the state to which that style is bound.

See LVGL's
[animation overview](https://lvgl.io/docs/open/main-modules/animation)
and the
[`lv_example_style_11`](https://github.com/lvgl/lvgl/blob/master/examples/styles/lv_example_style_11.c)
example for the underlying behaviour.

### Lifetime

`lv_obj_add_style` keeps a *pointer* to each `lv_style_t`, so the
firmware can't stack-allocate styles inside the build loop. They live
on the heap, owned by a `WidgetStyles` struct that the build loop
attaches to the widget via an `LV_EVENT_DELETE` callback
(`widget_styles_delete_cb`). The same struct also owns any
heap-allocated `lv_style_transition_dsc_t` descriptors and their
0-terminated `lv_style_prop_t[]` arrays (LVGL stores both by pointer
when `lv_style_set_transition` is called). When the widget is
destroyed — typically by `lv_obj_clean()` on screen switch — the
callback calls `lv_style_reset` + `delete` on each style and frees the
companion transition / prop-array buffers. Authors of new widget
factories don't need to wire any of this up: it's done once in
`apply_styles()`.

## Special events
Some event codes are special and handled locally entirely within the device.

* SCREEN_NEXT, SCREEN_PREV
* SLEEP_NOW (turns backlight off but leaves touchscreen still working - if a touch occurs turn backlight back on)