"""Public re-export of the generated protobuf message classes.

This is the supported way for application code to construct raw
``Screen`` / ``Widget`` / ``Layout*`` / ``Action`` / ``MacroStep``
messages without depending on the (internal) ``touchy_pad._proto``
module layout.

Example::

    from touchy_pad.api import protobuf

    screen = protobuf.Screen(name="home", version=protobuf.Screen.Version.CURRENT)
    screen.active.layout_flex.flow = protobuf.LayoutFlex.ROW
"""

from __future__ import annotations

from .. import _proto as _internal

# Re-export every public name from the internal protobuf module.
_public = [name for name in dir(_internal) if not name.startswith("_")]
globals().update({name: getattr(_internal, name) for name in _public})

__all__ = sorted(_public)
