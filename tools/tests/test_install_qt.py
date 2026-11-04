# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/install_qt.py (WI-0.018) with a fake `aqt` module and fake qmake; no network."""

from __future__ import annotations

import os
import subprocess
import sys
import textwrap
import time
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import install_qt

FAKE_AQT = textwrap.dedent(
    """
    import os, sys
    from pathlib import Path
    # argv: install-qt HOST TARGET VERSION ARCH -O DEST -m MODULES...
    _, host, target, version, arch = sys.argv[1:6]
    dest = Path(sys.argv[sys.argv.index("-O") + 1])
    mods = sys.argv[sys.argv.index("-m") + 1:]
    kit = {"clang_64": "macos", "linux_gcc_64": "gcc_64", "linux_gcc_arm64": "gcc_arm64"}.get(arch, arch)
    with open(os.environ["FAKE_AQT_LOG"], "a") as log:
        log.write(" ".join([host, target, version, arch, *mods]) + "\\n")
    if os.environ.get("FAKE_AQT_FAIL"):
        sys.exit(3)
    qmake = dest / version / kit / "bin" / "qmake"
    qmake.parent.mkdir(parents=True)
    qmake.write_text("#!/bin/sh\\necho " + os.environ.get("FAKE_QT_VERSION", version) + "\\n")
    qmake.chmod(0o755)
    """
)

HOST_IS_SUPPORTED = sys.platform in ("linux", "darwin")


@pytest.fixture
def fake(tmp_path, monkeypatch):
    mod = tmp_path / "fakeaqt" / "aqt"
    mod.mkdir(parents=True)
    (mod / "__init__.py").write_text("")
    (mod / "__main__.py").write_text(FAKE_AQT)
    log = tmp_path / "aqt.log"
    monkeypatch.setenv("PYTHONPATH", str(tmp_path / "fakeaqt"))
    monkeypatch.setenv("FAKE_AQT_LOG", str(log))
    return log


def run(*args: str, env: dict | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-I", str(TOOLS / "install_qt.py"), *args],
        capture_output=True, text=True, env={**os.environ, **(env or {})},
        check=False,
    )  # fmt: skip


@pytest.mark.skipif(not HOST_IS_SUPPORTED, reason="host kits exist for Linux and macOS only")
def test_plan_desktop_and_android():
    kits = install_qt.plan(["desktop", "android"])
    assert [k[1] for k in kits] == ["desktop", "android", "android"]
    assert [k[3] for k in kits][1:] == ["android_arm64_v8a", "android_x86_64"]
    assert len(install_qt.plan(["desktop"])) == 1


def test_aqt_command_passes_only_addon_modules(tmp_path):
    cmd = install_qt.aqt_command(tmp_path, "linux", "desktop", "linux_gcc_64")
    assert cmd[1:5] == ["-m", "aqt", "install-qt", "linux"]
    mods = cmd[cmd.index("-m", 3) + 1 :]
    assert mods == ["qtshadertools", "qtgraphs", "qtimageformats"]
    assert not {"qtbase", "qtdeclarative", "qtsvg"} & set(mods)


@pytest.mark.parametrize(
    "system,machine,expected",
    [
        ("Darwin", "arm64", ("mac", "clang_64", "macos")),
        ("Darwin", "x86_64", ("mac", "clang_64", "macos")),
        ("Linux", "x86_64", ("linux", "linux_gcc_64", "gcc_64")),
        ("Linux", "aarch64", ("linux_arm64", "linux_gcc_arm64", "gcc_arm64")),
    ],
)
def test_host_kit(monkeypatch, system, machine, expected):
    monkeypatch.setattr(install_qt.platform, "system", lambda: system)
    monkeypatch.setattr(install_qt.platform, "machine", lambda: machine)
    assert install_qt.host_kit() == expected


def test_unsupported_host(monkeypatch):
    monkeypatch.setattr(install_qt.platform, "system", lambda: "Windows")
    with pytest.raises(install_qt.UnsupportedHost):
        install_qt.host_kit()


def test_requires_target():
    assert run().returncode == 2


@pytest.mark.skipif(not HOST_IS_SUPPORTED, reason="host kits exist for Linux and macOS only")
def test_install_then_skip(tmp_path, fake):
    # AT1: every requested kit gets qmake 6.8.3; AT2: the re-run skips and is fast.
    dest = tmp_path / "Qt"
    args = ["--target", "desktop", "--target", "android", "--dest", str(dest)]
    r = run(*args)
    assert r.returncode == 0, r.stderr
    root = dest / "6.8.3"
    assert r.stdout.strip().splitlines()[-1] == f"QT_ROOT_DIR={root}"
    kits = [install_qt.host_kit()[2], "android_arm64_v8a", "android_x86_64"]
    for kit in kits:
        q = subprocess.run(
            [str(root / kit / "bin" / "qmake"), "-query", "QT_VERSION"],
            check=False,
            capture_output=True,
            text=True,
        )
        assert q.stdout.strip() == "6.8.3"
    lines = fake.read_text().splitlines()
    assert len(lines) == 3
    assert lines[0].endswith("qtshadertools qtgraphs qtimageformats")

    t0 = time.monotonic()
    r2 = run(*args)
    assert r2.returncode == 0 and "skipped" in r2.stderr
    assert time.monotonic() - t0 < 5
    assert len(fake.read_text().splitlines()) == 3  # aqt not called again
    assert r2.stdout == r.stdout  # deterministic output


@pytest.mark.skipif(not HOST_IS_SUPPORTED, reason="host kits exist for Linux and macOS only")
def test_wrong_version_is_rejected(tmp_path, fake):
    r = run(
        "--target",
        "desktop",
        "--dest",
        str(tmp_path / "Qt"),
        env={"FAKE_QT_VERSION": "6.7.0"},
    )
    assert r.returncode == 1 and "did not print 6.8.3" in r.stderr
    assert "QT_ROOT_DIR" not in r.stdout


@pytest.mark.skipif(not HOST_IS_SUPPORTED, reason="host kits exist for Linux and macOS only")
def test_aqt_failure_propagates(tmp_path, fake):
    r = run(
        "--target",
        "desktop",
        "--dest",
        str(tmp_path / "Qt"),
        env={"FAKE_AQT_FAIL": "1"},
    )
    assert r.returncode == 3 and "aqt failed" in r.stderr
