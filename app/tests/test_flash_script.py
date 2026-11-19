"""Tests for ``bin/flash.sh`` (the body of the ``just flash*`` recipes).

Each test builds a throwaway repo with fake ``esptool``/ESP-IDF scripts, so
nothing touches a real device, a real build, or the network. The script is
run with ``/bin/bash``: 3.2 on macOS, 5.x on Linux. Windows has no bash or
``/dev``, so these tests skip there.
"""

from __future__ import annotations

import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

import pytest

pytestmark = pytest.mark.skipif(
    sys.platform == "win32", reason="bin/flash.sh needs bash and a /dev tree"
)

SCRIPT = Path(__file__).resolve().parents[2] / "bin" / "flash.sh"


def _executable(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def _port_name(suffix: str = "1") -> str:
    if sys.platform == "darwin":
        return f"cu.usbmodem{suffix}"
    return f"ttyACM{suffix}"


class Fixture:
    def __init__(self, tmp_path: Path) -> None:
        self.repo = tmp_path / "repo with spaces"
        self.repo.mkdir()
        (self.repo / "bin").mkdir()
        shutil.copy2(SCRIPT, self.repo / "bin" / "flash.sh")

        self.build = self.repo / "firmware" / "build"
        self.build.mkdir(parents=True)
        self._write_build_files()

        self.home = tmp_path / "home"
        self.home.mkdir()
        self.shim = tmp_path / "shim"
        self.shim.mkdir()
        self.dev = tmp_path / "dev"
        self.dev.mkdir()

        self.log = tmp_path / "esptool.log"
        self.marker = tmp_path / "activated.marker"

        _executable(
            self.shim / "esptool",
            "#!/usr/bin/env bash\n"
            'printf \'%s\\n\' "$@" > "$ESPTOOL_LOG"\n'
            'if [ -n "${TOUCHY_ACTIVATED:-}" ]; then\n'
            '  printf "activated\\n" > "$ESPTOOL_MARKER"\n'
            "fi\n",
        )

    def _write_build_files(self, flash_args: str | None = None) -> None:
        if flash_args is None:
            flash_args = (
                "--flash_mode dio --flash_freq 80m --flash_size 4MB\n"
                "0x0 bootloader/bootloader.bin\n"
                "0x8000 partition_table/partition-table.bin\n"
                "0x10000 touchy_pad.bin\n"
            )
        (self.build / "flash_args").write_text(flash_args)
        (self.build / "CMakeCache.txt").write_text("IDF_TARGET:STRING=esp32s3\n")
        (self.build / "touchy_pad_merged.bin").write_bytes(b"merged")

    def activation(self, kind: str = "wrapper", *, fails: bool = False) -> Path:
        body = (
            "#!/usr/bin/env bash\nreturn 1\n"
            if fails
            else "#!/usr/bin/env bash\n"
            "export TOUCHY_ACTIVATED=1\n"
            f'export PATH="{self.shim}:$PATH"\n'
        )
        if kind == "wrapper":
            path = self.home / ".espressif" / "tools" / "activate_idf_v6.1.sh"
        else:
            path = self.home / "esp" / "esp-idf" / "export.sh"
        _executable(path, body)
        return path

    def add_port(self, suffix: str = "1", *, mode: int = 0o666) -> Path:
        path = self.dev / _port_name(suffix)
        path.write_text("")
        path.chmod(mode)
        return path

    def env(self, **overrides: str | None) -> dict[str, str]:
        env = os.environ.copy()
        env["HOME"] = str(self.home)
        env["PATH"] = f"{self.shim}:/usr/bin:/bin:/usr/sbin:/sbin"
        env["TOUCHY_IDF_VERSION"] = "v6.1"
        env["TOUCHY_DEV_ROOTS"] = str(self.dev)
        env["ESPTOOL_LOG"] = str(self.log)
        env["ESPTOOL_MARKER"] = str(self.marker)
        for key in ("TOUCHY_PORT", "TOUCHY_IDF_SOURCED", "TOUCHY_ACTIVATED"):
            env.pop(key, None)
        for key, value in overrides.items():
            if value is None:
                env.pop(key, None)
            else:
                env[key] = value
        return env

    def run(self, *args: str, **env_overrides: str | None) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["/bin/bash", str(self.repo / "bin" / "flash.sh"), *args],
            cwd=self.repo,
            env=self.env(**env_overrides),
            capture_output=True,
            text=True,
            timeout=30,
        )

    def esptool_args(self) -> list[str]:
        return self.log.read_text().splitlines()


def test_write_uses_activation_ports_and_flash_args(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.activation("wrapper")
    port = fx.add_port("1")

    result = fx.run("write")

    assert result.returncode == 0, result.stderr
    assert fx.esptool_args() == [
        "-p",
        str(port),
        "-b",
        "460800",
        "--chip",
        "esp32s3",
        "write-flash",
        "--flash_mode",
        "dio",
        "--flash_freq",
        "80m",
        "--flash_size",
        "4MB",
        "0x0",
        "bootloader/bootloader.bin",
        "0x8000",
        "partition_table/partition-table.bin",
        "0x10000",
        "touchy_pad.bin",
    ]
    assert fx.marker.exists(), "the activation script did not run"


def test_export_sh_fallback(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.activation("export")
    fx.add_port("1")

    result = fx.run("write")

    assert result.returncode == 0, result.stderr
    assert fx.marker.exists()


def test_path_only_toolchain(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.add_port("1")

    result = fx.run("write")

    assert result.returncode == 0, result.stderr
    assert not fx.marker.exists()
    assert fx.esptool_args()[:2] == ["-p", str(fx.dev / _port_name())]


def test_activation_failure_is_reported(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.activation("wrapper", fails=True)
    fx.add_port("1")

    result = fx.run("write")

    assert result.returncode != 0
    assert "ESP-IDF activation failed" in result.stderr
    assert "install.sh esp32,esp32s3,esp32p4" in result.stderr
    assert not fx.log.exists()


def test_missing_flash_args(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.activation("wrapper")
    fx.add_port("1")
    (fx.build / "flash_args").unlink()

    result = fx.run("write")

    assert result.returncode != 0
    assert "just firmware-build" in result.stderr


def test_invalid_touchy_port(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.activation("wrapper")

    result = fx.run("write", TOUCHY_PORT=str(tmp_path / "does-not-exist"))

    assert result.returncode != 0
    assert "not a readable+writable device" in result.stderr
    assert not fx.log.exists()


def test_explicit_touchy_port_wins(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.activation("wrapper")
    explicit = fx.add_port("1")
    fx.add_port("2")

    result = fx.run("write", TOUCHY_PORT=str(explicit))

    assert result.returncode == 0, result.stderr
    assert fx.esptool_args()[:2] == ["-p", str(explicit)]


def test_no_port_is_an_actionable_error(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.activation("wrapper")

    result = fx.run("write")

    assert result.returncode != 0
    assert "no accessible USB serial port found" in result.stderr
    assert "charge-only cable" in result.stderr
    assert not fx.log.exists()


def test_inaccessible_port_is_skipped(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.activation("wrapper")
    blocked = fx.add_port("1", mode=0o000)
    usable = fx.add_port("2")

    result = fx.run("write")

    assert result.returncode == 0, result.stderr
    args = fx.esptool_args()
    assert args[0] == "-p"
    assert args[1] == str(usable)
    assert args[1] != str(blocked)


def test_multiple_ports_require_explicit_selection(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.activation("wrapper")
    first = fx.add_port("1")
    second = fx.add_port("2")

    result = fx.run("write")

    assert result.returncode != 0
    assert "more than one USB serial port" in result.stderr
    assert str(first) in result.stderr
    assert str(second) in result.stderr
    assert not fx.log.exists()


def test_flash_args_path_with_spaces(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.activation("wrapper")
    fx.add_port("1")
    fx._write_build_files(
        "--flash_mode dio --flash_freq 80m --flash_size 4MB\n0x10000 app dir/touchy pad.bin\n"
    )

    result = fx.run("write")

    assert result.returncode == 0, result.stderr
    args = fx.esptool_args()
    assert args[-2:] == ["0x10000", "app dir/touchy pad.bin"]


def test_erase_uses_the_4m_storage_region(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.activation("wrapper")
    port = fx.add_port("1")

    result = fx.run("erase")

    assert result.returncode == 0, result.stderr
    assert fx.esptool_args() == [
        "-p",
        str(port),
        "-b",
        "460800",
        "--chip",
        "esp32s3",
        "erase_region",
        "0x310000",
        "0xC0000",
    ]


def test_merge_needs_no_serial_port(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.activation("wrapper")

    result = fx.run("merge")

    assert result.returncode == 0, result.stderr
    args = fx.esptool_args()
    assert args[:5] == ["--chip", "esp32s3", "merge-bin", "-o", "touchy_pad_merged.bin"]
    assert args[5:] == [
        "--flash_mode",
        "dio",
        "--flash_freq",
        "80m",
        "--flash_size",
        "4MB",
        "0x0",
        "bootloader/bootloader.bin",
        "0x8000",
        "partition_table/partition-table.bin",
        "0x10000",
        "touchy_pad.bin",
    ]
    assert str(fx.build / "touchy_pad_merged.bin") in result.stdout


def test_merged_writes_at_offset_zero(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.activation("wrapper")
    port = fx.add_port("1")

    result = fx.run("merged")

    assert result.returncode == 0, result.stderr
    assert fx.esptool_args() == [
        "-p",
        str(port),
        "-b",
        "460800",
        "--before",
        "default-reset",
        "--after",
        "hard-reset",
        "--chip",
        "esp32s3",
        "write-flash",
        "0x0",
        str(fx.build / "touchy_pad_merged.bin"),
    ]


def test_merged_requires_a_merged_image(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.activation("wrapper")
    fx.add_port("1")
    (fx.build / "touchy_pad_merged.bin").unlink()

    result = fx.run("merged")

    assert result.returncode != 0
    assert "just merge-bin" in result.stderr
    assert not fx.log.exists()


def test_chip_comes_from_cmake_cache(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)
    fx.activation("wrapper")
    fx.add_port("1")
    (fx.build / "CMakeCache.txt").write_text("IDF_TARGET:STRING=esp32\n")

    result = fx.run("write")

    assert result.returncode == 0, result.stderr
    args = fx.esptool_args()
    assert args[args.index("--chip") + 1] == "esp32"


def test_unknown_action_is_usage_error(tmp_path: Path) -> None:
    fx = Fixture(tmp_path)

    result = fx.run("nonsense")

    assert result.returncode == 2
    assert "usage: bin/flash.sh" in result.stderr


def test_script_parses_under_bash_and_avoids_bash4_builtins() -> None:
    text = SCRIPT.read_text()
    assert "mapfile" not in text
    assert "readarray" not in text

    result = subprocess.run(
        ["/bin/bash", "-n", str(SCRIPT)], capture_output=True, text=True, timeout=30
    )
    assert result.returncode == 0, result.stderr
