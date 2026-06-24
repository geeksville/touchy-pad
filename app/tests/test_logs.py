"""Stage 64.1 — host-side log tunneling tests.

Exercises the sim's ``LogRecord`` queue, the client's ``poll()``
return shape, and the ``event_consume`` log-dispatch side-effect.
"""

from __future__ import annotations

import logging

import pytest

from touchy_pad import _proto
from touchy_pad.api import TouchyClient
from touchy_pad.sim.transport import make_tempdir_transport


def test_poll_returns_log_record_when_only_logs_pending() -> None:
    with make_tempdir_transport() as t:
        t.device.push_log("hello world", priority=_proto.LOG_PRIORITY_INFO, tag="WIFI")
        c = TouchyClient(t)
        item = c.poll()
    assert isinstance(item, _proto.LogRecord)
    assert item.message == "hello world"
    assert item.tag == "WIFI"
    assert item.priority == _proto.LOG_PRIORITY_INFO


def test_events_take_priority_over_logs() -> None:
    with make_tempdir_transport() as t:
        t.device._events.put(_proto.LvEvent(code=1, host_code=42))  # noqa: SLF001
        t.device.push_log("background chatter", tag="DBG")
        c = TouchyClient(t)
        first = c.poll()
        second = c.poll()
    assert isinstance(first, _proto.LvEvent)
    assert isinstance(second, _proto.LogRecord)


def test_event_consume_routes_logs_to_device_logger(
    caplog: pytest.LogCaptureFixture,
) -> None:
    with make_tempdir_transport() as t:
        t.device.push_log(
            "oops something failed",
            priority=_proto.LOG_PRIORITY_ERROR,
            tag="WIFI",
        )
        c = TouchyClient(t)
        with caplog.at_level(logging.DEBUG, logger="touchy_pad.device"):
            evt = c.event_consume()
    # No real event ever existed — log records are silently consumed.
    assert evt is None
    records = [r for r in caplog.records if r.name == "touchy_pad.device"]
    assert len(records) == 1
    assert records[0].levelno == logging.ERROR
    assert records[0].getMessage() == "oops something failed"
    assert records[0].device_tag == "WIFI"


def test_trace_priority_uses_trace_child_logger(
    caplog: pytest.LogCaptureFixture,
) -> None:
    with make_tempdir_transport() as t:
        t.device.push_log("noisy", priority=_proto.LOG_PRIORITY_TRACE)
        c = TouchyClient(t)
        with caplog.at_level(logging.DEBUG, logger="touchy_pad.device.trace"):
            c.event_consume()
    names = [r.name for r in caplog.records]
    assert "touchy_pad.device.trace" in names


def test_num_dropped_emits_warning(caplog: pytest.LogCaptureFixture) -> None:
    rec = _proto.LogRecord(
        priority=_proto.LOG_PRIORITY_INFO,
        message="payload",
        num_dropped=3,
    )
    with make_tempdir_transport() as t:
        t.device._logs.put(rec)  # noqa: SLF001
        c = TouchyClient(t)
        with caplog.at_level(logging.WARNING, logger="touchy_pad.device"):
            c.event_consume()
    warns = [
        r for r in caplog.records if r.name == "touchy_pad.device" and r.levelno == logging.WARNING
    ]
    assert warns, "expected a dropped-records warning"
    assert "3" in warns[0].getMessage()
