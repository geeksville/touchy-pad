"""In-process Transport that targets the Python device simulator.

Wraps :class:`~touchy_pad.sim.device.SimDevice` with the same
command-OUT / response-IN queue model as the real USB transport, so
:class:`touchy_pad.api.TouchyClient` can use either interchangeably.

Implementation: two ``queue.Queue`` channels plus one worker thread
that drains the command queue, dispatches via :meth:`SimDevice.handle_command`,
and pushes the reply onto the response queue.

No framing is required (queues carry discrete messages), so this is
significantly simpler than :class:`~touchy_pad.api._transport.UsbTransport`.
"""

from __future__ import annotations

import logging
import tempfile
import threading
from collections.abc import Callable
from pathlib import Path
from queue import Empty, Queue

from .. import _proto
from ..api._transport import Transport, TransportError
from .device import SimDevice
from .fs import SimFs, default_cache_root

_log = logging.getLogger("touchy_pad.sim")


class SimDeviceTransport(Transport):
    """A :class:`Transport` backed by an in-process :class:`SimDevice`.

    Parameters
    ----------
    serial:
        Pseudo-USB serial number; used as the sim's pseudo-fs subdir
        name so multiple sim instances can coexist. Defaults to
        ``"SIM0000"``.
    fs_root:
        Cache directory root. ``None`` (default) uses :func:`default_cache_root`.
        Pass a :class:`pathlib.Path` (often a ``tmp_path`` in tests) to
        sandbox state.
    headless:
        When ``True``, only the device-core runs; no GUI is constructed
        and PySide6 is never imported. The default (``False``) opens
        the window — but the GUI itself lands in step 4 of Stage 30,
        so for now only headless is functional.
    on_screen_change:
        Optional callback invoked from the worker thread whenever the
        sim's active screen changes. The GUI wires this; tests use it
        to observe transitions.
    """

    # The sim sees PNG / JPG / etc. and decodes them with Pillow, so
    # the host should *not* convert images to LVGL .bin first.
    needs_image_conversion = False

    def __init__(
        self,
        *,
        serial: str = "SIM0000",
        fs_root: Path | None = None,
        headless: bool = True,
        on_screen_change: Callable[[_proto.Screen | None], None] | None = None,
        display_size: tuple[int, int] = (480, 300),
    ) -> None:
        # ``headless`` is retained for API symmetry but currently only
        # affects whether the transport will *expect* a GUI to attach;
        # the transport itself never imports PySide6. Windowed mode is
        # driven by :class:`touchy_pad.sim.window.SimWindow`, which the
        # caller wires to ``self.device`` after construction (or via
        # the ``touchy sim`` CLI subcommand for the standalone case).
        del headless  # informational only for now
        self._serial = serial
        self._fs = SimFs(fs_root or default_cache_root(), serial)
        self._device = SimDevice(
            self._fs,
            on_screen_change=on_screen_change,
            display_size=display_size,
        )

        self._cmd_q: Queue[bytes | None] = Queue()
        self._resp_q: Queue[bytes] = Queue()
        self._closed = threading.Event()
        self._worker = threading.Thread(target=self._run, name=f"sim-device-{serial}", daemon=True)
        self._worker.start()
        _log.info("sim: device transport up (serial=%s, fs=%s)", serial, self._fs.root)

    # -- introspection used by the GUI / tests ---------------------------

    @property
    def device(self) -> SimDevice:
        return self._device

    @property
    def serial(self) -> str:
        return self._serial

    # -- Transport interface ---------------------------------------------

    def send_command(self, payload: bytes) -> None:
        if self._closed.is_set():
            raise TransportError("sim transport is closed")
        self._cmd_q.put(bytes(payload))

    def recv_response(self, timeout_ms: int = 2000) -> bytes:
        try:
            return self._resp_q.get(timeout=timeout_ms / 1000.0)
        except Empty as exc:
            raise TransportError(f"sim transport: no response within {timeout_ms} ms") from exc

    def close(self) -> None:
        if self._closed.is_set():
            return
        self._closed.set()
        # Sentinel wakes the worker so it exits its loop promptly.
        self._cmd_q.put(None)
        # Don't join indefinitely — a misbehaving handler shouldn't
        # hang the client's __exit__.
        self._worker.join(timeout=2.0)

    # -- worker -----------------------------------------------------------

    def _run(self) -> None:
        while not self._closed.is_set():
            payload = self._cmd_q.get()
            if payload is None:  # shutdown sentinel
                return
            try:
                reply = self._device.handle_command(payload)
            except Exception as exc:  # noqa: BLE001 — keep transport alive
                _log.exception("sim: handler crashed: %s", exc)
                reply = _proto.Response(code=_proto.RESULT_UNKNOWN_ERROR).SerializeToString()
            self._resp_q.put(reply)


# ---------------------------------------------------------------------------


def make_tempdir_transport(**kwargs: object) -> SimDeviceTransport:
    """Convenience: a headless sim with state in a fresh temp dir.

    Lifetime of the temp dir is tied to the returned transport: it's
    pinned via ``_tempdir`` so the directory survives until ``close()``.
    """
    td = tempfile.TemporaryDirectory(prefix="touchy-pad-sim-")
    kwargs.setdefault("headless", True)
    kwargs.setdefault("fs_root", Path(td.name))
    t = SimDeviceTransport(**kwargs)  # type: ignore[arg-type]
    t._tempdir = td  # type: ignore[attr-defined]  # keep the TemporaryDirectory alive
    return t
