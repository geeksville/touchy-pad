"""Re-exports from the generated protobuf module.

The actual ``touchy_pb2`` module is generated from ``proto/touchy.proto`` by
``just build-proto`` (or ``just build-proto-py``) and committed under
``touchy_pad/_proto/touchy_pb2.py`` so the package is installable from
PyPI without protoc.
"""

from __future__ import annotations

from .touchy_pb2 import (  # noqa: F401  (re-exported)
    Action,
    Arc,
    Button,
    Command,
    Event,
    EventConsumeCmd,
    EventConsumeResponse,
    FileResetCmd,
    FileSaveCmd,
    Image,
    Label,
    Layout,
    LvEvent,
    Rect,
    Response,
    ResultCode,
    Screen,
    ScreenLoadCmd,
    ScreenSleepTimeoutCmd,
    ScreenWakeCmd,
    Slider,
    Spacer,
    StreamEventsCmd,
    Style,
    Switch,
    SysRebootBootloaderCmd,
    SysVersionGetCmd,
    SysVersionResponse,
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
