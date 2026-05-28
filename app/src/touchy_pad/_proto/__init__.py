"""Re-exports from the generated protobuf modules.

The actual ``touchy_pb2`` and ``widgets_pb2`` modules are generated from
``proto/touchy.proto`` and ``proto/widgets.proto`` by ``just build-proto``
(or ``just build-proto-py``) and committed under ``touchy_pad/_proto/`` so
the package is installable from PyPI without protoc.
"""

from __future__ import annotations

from .touchy_pb2 import (  # noqa: F401  (re-exported)
    Command,
    Event,
    EventConsumeCmd,
    EventConsumeResponse,
    FileCloseCmd,
    FileDeleteCmd,
    FileOpenWriteCmd,
    FileOpenWriteResponse,
    FileWriteCmd,
    LogPriority,
    LogRecord,
    LvEvent,
    Response,
    ResultCode,
    ScreenLoadCmd,
    ScreenSleepTimeoutCmd,
    ScreenWakeCmd,
    StreamEventsCmd,
    SysBoardInfoGetCmd,
    SysBoardInfoResponse,
    SysRebootBootloaderCmd,
)
from .widgets_pb2 import (  # noqa: F401  (re-exported)
    Action,
    ActionChangeWidgetRef,
    ActionDevice,
    ActionHost,
    ActionMacro,
    Animation,
    AnimPath,
    AnimTrack,
    Arc,
    Button,
    Checkbox,
    ForceRender,
    Fps,
    GridCell,
    Image,
    KeyEvent,
    Label,
    LayoutAbsolute,
    LayoutFlex,
    LayoutGrid,
    LogLine,
    LvEventCode,
    LvState,
    MacroStep,
    MouseMove,
    Rect,
    RippleAnimation,
    Screen,
    Slider,
    Spacer,
    Style,
    StyleProp,
    Switch,
    TextAlign,
    Trackpad,
    Transition,
    Widget,
)

# Convenience aliases so callers can write `_proto.RESULT_OK` without poking
# into the enum descriptor.
RESULT_OK = ResultCode.Value("RESULT_OK")
RESULT_UNKNOWN_ERROR = ResultCode.Value("RESULT_UNKNOWN_ERROR")
RESULT_INVALID_ARG = ResultCode.Value("RESULT_INVALID_ARG")
RESULT_NOT_FOUND = ResultCode.Value("RESULT_NOT_FOUND")
RESULT_NO_SPACE = ResultCode.Value("RESULT_NO_SPACE")
RESULT_IO_ERROR = ResultCode.Value("RESULT_IO_ERROR")
RESULT_NOT_SUPPORTED = ResultCode.Value("RESULT_NOT_SUPPORTED")

# Flat aliases for ``LogPriority`` so callers can write
# ``_proto.LOG_PRIORITY_INFO`` without poking into the descriptor.
LOG_PRIORITY_TRACE = LogPriority.Value("LOG_PRIORITY_TRACE")
LOG_PRIORITY_DEBUG = LogPriority.Value("LOG_PRIORITY_DEBUG")
LOG_PRIORITY_INFO = LogPriority.Value("LOG_PRIORITY_INFO")
LOG_PRIORITY_WARN = LogPriority.Value("LOG_PRIORITY_WARN")
LOG_PRIORITY_ERROR = LogPriority.Value("LOG_PRIORITY_ERROR")

# Flat aliases for the ``TextAlign`` enum so callers can write
# ``_proto.TEXT_ALIGN_CENTER`` without poking into the descriptor.
TEXT_ALIGN_AUTO = TextAlign.Value("TEXT_ALIGN_AUTO")
TEXT_ALIGN_LEFT = TextAlign.Value("TEXT_ALIGN_LEFT")
TEXT_ALIGN_CENTER = TextAlign.Value("TEXT_ALIGN_CENTER")
TEXT_ALIGN_RIGHT = TextAlign.Value("TEXT_ALIGN_RIGHT")

# Flat aliases for the most-used ``LvEventCode`` values so callers can write
# ``_proto.LV_EVENT_PRESSED`` without poking into the descriptor.
LV_EVENT_ALL = LvEventCode.Value("LV_EVENT_ALL")
LV_EVENT_PRESSED = LvEventCode.Value("LV_EVENT_PRESSED")
LV_EVENT_PRESSING = LvEventCode.Value("LV_EVENT_PRESSING")
LV_EVENT_PRESS_LOST = LvEventCode.Value("LV_EVENT_PRESS_LOST")
LV_EVENT_SHORT_CLICKED = LvEventCode.Value("LV_EVENT_SHORT_CLICKED")
LV_EVENT_LONG_PRESSED = LvEventCode.Value("LV_EVENT_LONG_PRESSED")
LV_EVENT_CLICKED = LvEventCode.Value("LV_EVENT_CLICKED")
LV_EVENT_RELEASED = LvEventCode.Value("LV_EVENT_RELEASED")
LV_EVENT_VALUE_CHANGED = LvEventCode.Value("LV_EVENT_VALUE_CHANGED")
LV_EVENT_STATE_CHANGED = LvEventCode.Value("LV_EVENT_STATE_CHANGED")
