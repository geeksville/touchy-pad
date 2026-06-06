"""Built-in page bodies for ``F:host/uscr/``.

Each submodule exposes a ``build()`` function that returns a
``(name, Widget)`` tuple ready to pass to
:meth:`~touchy_pad.api.touchy.Touchy.user_screen_save`.
"""

from . import test, trackpad

__all__ = ["test", "trackpad"]
