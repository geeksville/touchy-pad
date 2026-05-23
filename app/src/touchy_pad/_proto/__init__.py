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
    FileResetCmd,
    FileSaveCmd,
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
    ActionDevice,
    ActionHost,
    ActionMacro,
    ActionSwitchScreen,
    AnimPath,
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
