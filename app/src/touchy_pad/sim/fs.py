"""Pseudo-filesystem for the device simulator.

Mirrors the firmware's host-uploaded file area inside a per-serial
subdirectory of the user's cache dir, so the sim's state survives
restarts and isolates between simultaneously-running sim instances.

After stage 51 the device exposes two filesystems addressed by drive
letter:

* ``F:host/...`` — persistent flash (LittleFS on real hardware).
* ``R:host/...`` — PSRAM RAM-disk; lost on reboot in firmware but the
  sim mirrors it to disk too for implementation simplicity (per
  ``docs/design.md`` stage 51 notes).

We sub-directory by drive letter under the sim root so the two stores
stay separated. Keys passed across the API are full drive-prefixed
paths, exactly as they appear on the wire.

Cache root resolution uses :mod:`platformdirs` when available, with a
``$XDG_CACHE_HOME``/``~/.cache`` fallback so the sim still works in
minimal environments (e.g. CI without optional deps installed).
"""

from __future__ import annotations

import os
import shutil
from pathlib import Path


def default_cache_root() -> Path:
    """Return the user-cache root for sim state.

    Linux:   ``$XDG_CACHE_HOME/touchy-pad/sim`` or ``~/.cache/touchy-pad/sim``
    macOS:   ``~/Library/Caches/touchy-pad/sim``
    Windows: ``%LOCALAPPDATA%\\touchy-pad\\sim``
    """
    try:
        from platformdirs import user_cache_dir
    except ImportError:
        env = os.environ.get("XDG_CACHE_HOME")
        base = Path(env) if env else Path.home() / ".cache"
        return base / "touchy-pad" / "sim"
    return Path(user_cache_dir("touchy-pad", appauthor=False)) / "sim"


def _split_drive(path: str) -> tuple[str, str]:
    """Return ``(letter, rest)`` for ``"<L>:rest"`` paths.

    Tolerates a leading slash after the colon (``"F:/host/x"`` →
    ``("F", "host/x")``) for callers that habitually include one.
    Raises ``ValueError`` for missing or malformed prefixes so the
    sim mirrors the device's refusal to silently rebase legacy paths.
    """
    if len(path) < 2 or path[1] != ":":
        raise ValueError(f"missing drive letter in path: {path!r}")
    letter = path[0].upper()
    if letter not in ("F", "R"):
        raise ValueError(f"unknown drive {letter!r} in {path!r}")
    rest = path[2:]
    if rest.startswith("/"):
        rest = rest[1:]
    if not rest or ".." in Path(rest).parts:
        raise ValueError(f"bad sim-fs path: {path!r}")
    return letter, rest


class SimFs:
    """A flat key/value store under ``<root>/<serial>/<drive>/``.

    Keys are full drive-prefixed paths the host uses on the wire (e.g.
    ``"F:host/screens/home.pb"``, ``"R:host/images/avatar.bin"``).
    They map to ``<root>/<serial>/<letter>/<rest>`` on disk; parent
    directories are created on demand. Path traversal (``..``) and
    missing/unknown drive letters are rejected.
    """

    def __init__(self, root: Path, serial: str) -> None:
        self.root = (root / serial).resolve()
        self.root.mkdir(parents=True, exist_ok=True)

    # -- helpers ----------------------------------------------------------

    def _resolve(self, path: str) -> Path:
        """Map a drive-prefixed path to an absolute path inside :attr:`root`."""
        letter, rest = _split_drive(path)
        abs_path = (self.root / letter / rest).resolve()
        # Belt-and-suspenders: confirm the resolved target really sits
        # under root, even if Path.parts looked clean.
        try:
            abs_path.relative_to(self.root)
        except ValueError as exc:
            raise ValueError(f"path escapes sim-fs root: {path!r}") from exc
        return abs_path

    def _drive_root(self, letter: str) -> Path:
        if letter not in ("F", "R"):
            raise ValueError(f"unknown drive {letter!r}")
        return self.root / letter

    # -- API --------------------------------------------------------------

    def delete(self, path: str) -> None:
        """Remove a file or directory subtree at the given drive path.

        Mirrors firmware ``FlashFs::removeTree``: deleting a directory
        recursively unlinks every child; deleting a missing path is a
        no-op (so the host can issue blanket wipes without checking).
        """
        letter, rest = _split_drive(path)
        target = self._resolve(path)
        if target.is_dir():
            shutil.rmtree(target, ignore_errors=True)
        elif target.is_file():
            target.unlink()
        # else: missing — treat as a no-op (matches firmware semantics).
        _ = letter  # only used for validation

    def save(self, path: str, data: bytes) -> None:
        """Write *data* to drive-prefixed *path*, creating parents."""
        target = self._resolve(path)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    def read(self, path: str) -> bytes:
        return self._resolve(path).read_bytes()

    def exists(self, path: str) -> bool:
        try:
            return self._resolve(path).is_file()
        except ValueError:
            return False

    def list_screens(self) -> list[str]:
        """Return uploaded screen paths, sorted lexicographically.

        Returns full drive-prefixed paths
        (e.g. ``"F:host/screens/home.pb"``) so callers can hand them
        straight to :meth:`SimDevice._do_screen_load` /
        :meth:`TouchyClient.screen_load`. Mirrors the firmware's
        boot-time scan of every drive's ``host/screens/*.pb`` subtree.
        """
        found: list[str] = []
        for letter in ("F", "R"):
            d = self._drive_root(letter) / "host" / "screens"
            if not d.is_dir():
                continue
            for p in d.glob("*.pb"):
                found.append(f"{letter}:host/screens/{p.name}")
        found.sort()
        return found

    def list_widget_files(self, directory: str) -> list[str]:
        """Enumerate ``*.pb`` files under a drive-prefixed directory.

        Stage 57 — used by the sim's ``ActionChangeWidgetRef`` NEXT /
        PREVIOUS handler. *directory* must be drive-prefixed (e.g.
        ``"F:host/w/"``). Returns full drive-prefixed paths sorted
        lexicographically; missing directories return an empty list.
        """
        try:
            d = self._resolve(directory.rstrip("/") + "/")
        except ValueError:
            return []
        if not d.is_dir():
            return []
        # Recover the drive-prefixed prefix from the input.
        prefix = directory if directory.endswith("/") else directory + "/"
        return sorted(f"{prefix}{p.name}" for p in d.glob("*.pb"))
