"""Tests for ``bin/firmware-report.sh`` (the body of ``just firmware-report``).

Each test builds a throwaway repo with a fake ``idf.py`` and ESP-IDF activation
script, so nothing touches a real build. The script is run with ``/bin/bash``:
3.2 on macOS, 5.x on Linux. Windows has no bash, so these tests skip there.
"""

from __future__ import annotations

import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

import pytest

pytestmark = pytest.mark.skipif(sys.platform == "win32", reason="bin/firmware-report.sh needs bash")

SCRIPT = Path(__file__).resolve().parents[2] / "bin" / "firmware-report.sh"

# The line ESP-IDF's check_sizes.py prints for the app partition. This is what
# the script parses for its summary.
SIZE_OUTPUT = (
    "touchy_pad_v2.bin binary size 0x1b6c80 bytes. Smallest app partition is "
    "0x300000 bytes. 0x149380 bytes (43%) free.\n"
)


def _executable(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


class Fixture:
    def __init__(self, tmp_path: Path) -> None:
        self.repo = tmp_path / "repo"
        self.repo.mkdir()
        (self.repo / "bin").mkdir()
        shutil.copy2(SCRIPT, self.repo / "bin" / "firmware-report.sh")

        self.home = tmp_path / "home"
        self.home.mkdir()
        self.shim = tmp_path / "shim"
        self.shim.mkdir()
        self.log = tmp_path / "idf.log"
        self.out = self.shim / "out.txt"

    def fake_idf(self, output: str = SIZE_OUTPUT) -> None:
        self.out.write_text(output)
        _executable(
            self.shim / "idf.py",
            "#!/usr/bin/env bash\n" 'printf "%s\\n" "$@" > "$IDF_LOG"\n' 'cat "$IDF_OUTPUT"\n',
        )

    def activation(self, kind: str = "wrapper", *, fails: bool = False) -> Path:
        body = (
            "#!/usr/bin/env bash\nreturn 1\n"
            if fails
            else "#!/usr/bin/env bash\n" f'export PATH="{self.shim}:$PATH"\n'
        )
        if kind == "wrapper":
            path = self.home / ".espressif" / "tools" / "activate_idf_v6.1.sh"
        else:
            path = self.home / "esp" / "esp-idf" / "export.sh"
        _executable(path, body)
        return path

    def env(self, **overrides: str | None) -> dict[str, str]:
        env = os.environ.copy()
        env["HOME"] = str(self.home)
        env["PATH"] = f"{self.shim}:/usr/bin:/bin:/usr/sbin:/sbin"
        env["TOUCHY_IDF_VERSION"] = "v6.1"
        env["IDF_LOG"] = str(self.log)
        env["IDF_OUTPUT"] = str(self.out)
        env.pop("TOUCHY_PORT", None)
        for key, value in overrides.items():
            if value is None:
                env.pop(key, None)
            else:
                env[key] = value
        return env

    def run(self, **env_overrides: str | None) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["/bin/bash", str(self.repo / "bin" / "firmware-report.sh")],
            cwd=self.repo,
            env=self.env(**env_overrides),
            capture_output=True,
            text=True,
            timeout=30,
        )


def test_runs_idf_size_and_prints_the_app_partition_summary(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.activation("wrapper")
    fx.fake_idf()

    result = fx.run()

    assert result.returncode == 0, result.stderr
    # `0x1b6c80` = 1797248 bytes of a `0x300000` = 3145728-byte partition → 57%.
    assert (
        "App image: 1797248 of 3145728 bytes used — 57% full (1348480 bytes free)" in result.stdout
    )
    # The size tool was invoked for the current build + target.
    assert fx.log.read_text().splitlines() == ["-C", "firmware", "size"]


def test_export_sh_fallback(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.activation("export")
    fx.fake_idf()

    result = fx.run()

    assert result.returncode == 0, result.stderr
    assert "57% full" in result.stdout


def test_path_only_toolchain_needs_no_activation(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.fake_idf()

    result = fx.run()

    assert result.returncode == 0, result.stderr
    assert "57% full" in result.stdout


def test_activation_failure_is_reported(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.activation("wrapper", fails=True)
    fx.fake_idf()

    result = fx.run()

    assert result.returncode != 0
    assert "ESP-IDF activation failed" in result.stderr
    assert not fx.log.exists()


def test_missing_idf_py_is_reported(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)

    result = fx.run()

    assert result.returncode != 0
    assert "idf.py not found on PATH" in result.stderr


def test_missing_app_partition_line_still_succeeds(tmp_path: Path) -> None:
    """A build whose output has no parse line reports IDF's own output only."""
    fx = Fixture(tmp_path)
    fx.fake_idf(output="nothing useful here\n")

    result = fx.run()

    assert result.returncode == 0, result.stderr
    assert "App image:" not in result.stdout


def test_script_parses_under_bash_and_avoids_bash4_builtins() -> None:
    text = SCRIPT.read_text()
    assert "mapfile" not in text
    assert "readarray" not in text

    result = subprocess.run(
        ["/bin/bash", "-n", str(SCRIPT)], capture_output=True, text=True, timeout=30
    )
    assert result.returncode == 0, result.stderr
