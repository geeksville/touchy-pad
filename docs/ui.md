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

## Image representation
Images sent from host are always in LVGL BIN format.  Converted on the host.  Some sort of compression will be supported, possibly https://lvgl.io/docs/open/libs/image_support/lz4?

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
`text_color`) maps to exactly one `lv_style_set_<prop>` call inside
`build_lv_style()` in [firmware/main/screens.cpp](../firmware/main/screens.cpp);
fields left at their proto3 default (zero) are skipped and inherit the
theme.

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
[`touchy_pad.screens`](../app/src/touchy_pad/screens.py).

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
from touchy_pad.screens import image_button, host_action, style, STATE_PRESSED

image_button(
    "smile",
    asset="images/smiley.bmp",
    on_click=host_action(0x103),
    style=[style(bg_color=0x1E90FF, for_state=STATE_PRESSED)],
)
```

A Dodger-blue background only paints under the smiley while the user
is actively pressing it; on release LVGL drops back to the
default-state look (no background, theme defaults).

### Lifetime

`lv_obj_add_style` keeps a *pointer* to each `lv_style_t`, so the
firmware can't stack-allocate styles inside the build loop. They live
on the heap, owned by a `WidgetStyles` struct that the build loop
attaches to the widget via an `LV_EVENT_DELETE` callback
(`widget_styles_delete_cb`). When the widget is destroyed — typically
by `lv_obj_clean()` on screen switch — the callback calls
`lv_style_reset` + `delete` on each entry. Authors of new widget
factories don't need to wire any of this up: it's done once in
`apply_styles()`.

## Special events
Some event codes are special and handled locally entirely within the device.

* SCREEN_NEXT, SCREEN_PREV
* SLEEP_NOW (turns backlight off but leaves touchscreen still working - if a touch occurs turn backlight back on)