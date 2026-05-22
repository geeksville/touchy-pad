"""Structured JSONL + pretty-text logger used by the probe."""
from __future__ import annotations

import json
import sys
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Any


class ProbeLogger:
    """Writes one JSON-line per observation plus a parallel human log.

    Every record carries a wall-clock `ts`, a phase tag (e.g. "set_key_image"),
    an optional deck index, an event name, and a free-form `data` dict.
    """

    def __init__(self, jsonl_path: Path, text_path: Path) -> None:
        jsonl_path.parent.mkdir(parents=True, exist_ok=True)
        self._jsonl = jsonl_path.open("w", buffering=1, encoding="utf-8")
        self._text = text_path.open("w", buffering=1, encoding="utf-8")
        self._jsonl_path = jsonl_path
        self._text_path = text_path

    @property
    def jsonl_path(self) -> Path:
        return self._jsonl_path

    @property
    def text_path(self) -> Path:
        return self._text_path

    def log(
        self,
        phase: str,
        event: str,
        *,
        deck_index: int | None = None,
        data: dict[str, Any] | None = None,
    ) -> None:
        rec = {
            "ts": time.time(),
            "phase": phase,
            "event": event,
            "deck_index": deck_index,
            "data": data or {},
        }
        self._jsonl.write(json.dumps(rec, default=_json_default) + "\n")

        # Pretty line: "[12:34:56.789] phase/event  deck=0  key=value ..."
        ts_str = time.strftime("%H:%M:%S", time.localtime(rec["ts"])) + f".{int((rec['ts'] % 1) * 1000):03d}"
        prefix = f"[{ts_str}] {phase}/{event}"
        if deck_index is not None:
            prefix += f"  deck={deck_index}"
        payload = ""
        if data:
            payload = "  " + " ".join(f"{k}={_short(v)}" for k, v in data.items())
        line = prefix + payload
        self._text.write(line + "\n")
        print(line, file=sys.stderr)

    @contextmanager
    def timed(self, phase: str, event: str, *, deck_index: int | None = None, **data: Any):
        """Time a block and emit one log record with duration_ms + any error."""
        t0 = time.perf_counter()
        err: BaseException | None = None
        try:
            yield
        except BaseException as e:  # noqa: BLE001 -- we want to record then re-raise
            err = e
            raise
        finally:
            dur_ms = (time.perf_counter() - t0) * 1000
            d = dict(data)
            d["duration_ms"] = round(dur_ms, 3)
            if err is not None:
                d["error_type"] = type(err).__name__
                d["error"] = str(err)
            self.log(phase, event, deck_index=deck_index, data=d)

    def close(self) -> None:
        self._jsonl.close()
        self._text.close()


def _json_default(o: Any) -> Any:
    """Best-effort JSON serializer for things like bytes / tuples / enums."""
    if isinstance(o, bytes):
        return {"__bytes__": True, "len": len(o)}
    if hasattr(o, "name") and hasattr(o, "value"):  # Enum
        return o.name
    return repr(o)


def _short(v: Any) -> str:
    """Compact representation for the pretty-log line."""
    if isinstance(v, bytes):
        return f"<{len(v)} bytes>"
    if isinstance(v, dict):
        return "{" + ", ".join(f"{k}={_short(val)}" for k, val in v.items()) + "}"
    if isinstance(v, (list, tuple)):
        return "[" + ", ".join(_short(x) for x in v) + "]"
    s = repr(v)
    if len(s) > 80:
        return s[:77] + "..."
    return s
