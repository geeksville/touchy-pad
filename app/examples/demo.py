#!/usr/bin/env python3
"""A friendly first taste of touchy-pad.

This is a complete, runnable example you can use as follows:

    pip install touchy-pad
    python demo.py

Plug in your Touchy-Pad first. The script builds a small page of
widgets, sends it to the device, shows it, and then prints a line every
time you poke one of the widgets. Press Ctrl-C when you're done.

Everything below uses the supported public API under ``touchy_pad.api``
-- the same building blocks the rest of touchy uses internally.

This demo is constructed to use a simple column layour, but for more advanced applications you should
probably use LayoutGrid.
"""

# The whole public API is re-exported from one module, so a single import
# line gives us the connection helper plus every widget/layout builder.
from touchy_pad.api import *
from touchy_pad.api import macros

def build_page() -> protobuf.Widget:
    """Build the demo page and return it as a single ``Widget``.

    We use a ``col`` (vertical) layout, so every widget we add with
    ``+=`` simply stacks below the previous one -- one row per widget,
    no grid coordinates to think about.

    The interesting part is the ``on_click`` / ``on_change`` arguments:

    * ``macro_action(...)`` runs entirely *on the device* -- here it
      makes the Touchy-Pad act like a USB keyboard and type some text.
    * ``host_action(on_event=...)`` sends an event back to *this* Python
      script, so your ``lambda`` runs on your computer. Great for wiring
      a button up to whatever you like (launch an app, hit an API, ...).
    """
    page = Layer(layout=col(gap=8))

    # A friendly title.
    page += label("title", text="Hello, Touchy-Pad!", font_size=24, style=style(text_color=0xFFFFFF))

    # A button that *types* for you. Tap it and the device sends the
    # keystrokes over USB, exactly as if you'd typed them yourself.
    page += button(
        "type",
        text="Type a greeting",
        on_click=macro_action(macros.type_text("Hi from Touchy! ")),
    )

    # A button that calls back into this script.
    page += button(
        "ping",
        text="Ping me",
        on_click=host_action(on_event=lambda e: print(f"  [ping]  you tapped {e.user_data!r}")),
    )

    # A slider. ``e.value`` carries its current position.
    page += slider(
        "level",
        min=0,
        max=100,
        value=42,
        on_change=host_action(on_event=lambda e: print(f"  [slider] now at {e.value}")),
    )

    # An on/off switch. ``e.checked`` is True/False.
    page += toggle(
        "power",
        on=True,
        on_change=host_action(
            on_event=lambda e: print(f"  [toggle] {'on' if e.checked else 'off'}")
        ),
    )

    # An image button using the smiley PNG we upload in main().
    page += image_button(
        "smile",
        asset="F:host/images/smiley.png",
        scale=2.0,  # draw it at 200% so it's easy to hit
        on_click=host_action(on_event=lambda e: print("  [smile] :)")),
    )

    # A ``Layer`` isn't a protobuf message yet, so pour it into a
    # ``Widget`` (the unit ``user_screen_save`` expects).
    widget = protobuf.Widget()
    page.copy_into(widget)
    return widget


def main() -> None:
    # ``touchy_open()`` finds the first attached device, checks it's
    # running compatible firmware, and hands back a ``Touchy`` object.
    # Using it as a context manager guarantees a clean disconnect.
    with touchy_open() as pad:
        # Say hello to whatever we connected to.
        info = pad.board_info
        if info is not None:
            print(
                f"Connected to {info.board_name or 'device'} "
                f"(firmware {info.firmware_version_str or info.firmware_version})"
            )

        # 1. Upload the smiley image so the image_button has something to
        #    show. ``file_save`` takes a drive-prefixed path: "F:" is
        #    persistent flash on the device.
        pad.file_save("F:host/images/smiley.png", images.make_smiley_png())

        # 2. Save our page as a "user screen" body named "demo". This
        #    writes it to F:host/uscr/demo.pb -- no Screen object needed.
        pad.user_screen_save("demo", build_page())

        # 3. Bring our page to the front. The device's built-in chrome
        #    (the Prev / Next bar) hosts a swappable "page" area;
        #    ``show_user_screen`` points it at our "demo" page so it
        #    appears right away -- no screen reload, no user touch.
        pad.show_user_screen("demo")

        # 4. Sit back and let events roll in. The inline ``host_action``
        #    callbacks above were registered automatically by
        #    ``user_screen_save``; they fire from a background thread, so
        #    all we have to do here is keep the program alive.
        print("\nPage is live -- tap things on the device! Press Ctrl-C to quit.\n")
        try:
            import time

            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            print("\nBye!")


if __name__ == "__main__":
    main()
