# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""The test IPC server of the real ``ovt-studio`` (chapter 14 section 14.10.3; WI-1.465).

test_ipc_client.py tests the client against a fake server; this file starts the application with
``--test-ipc <socket>`` and runs every command of 14.10.3 against it. The command semantics are also covered without
the application by app/tests/testipc (Catch2).
"""

from __future__ import annotations

import os
import shutil
import stat
import struct
import subprocess
import tempfile
from pathlib import Path

import pytest
from conftest import REPO, IpcError, Studio

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def png_size(path: Path) -> tuple[int, int]:
    data = path.read_bytes()
    assert data[:8] == PNG_SIGNATURE
    return struct.unpack(">II", data[16:24])


@pytest.fixture
def window(studio: Studio) -> Studio:
    """The application with its main window up (the Welcome page is the empty state)."""
    studio.wait_idle(20000)
    return studio


def test_get_reads_items_and_singletons(window: Studio) -> None:
    assert window.get("Project.fileCount") == 0
    assert window.get("welcomePage.visible") is True
    assert window.get("jobsButton.text") == "Jobs"
    assert window.get("JobsModel.activeCount") == 0


def test_get_errors_name_the_problem(window: Studio) -> None:
    with pytest.raises(IpcError, match="no object named 'noSuchItem'"):
        window.get("noSuchItem.width")
    with pytest.raises(IpcError, match="no property 'noSuchProperty'"):
        window.get("jobsButton.noSuchProperty")
    with pytest.raises(IpcError, match="unknown command"):
        window.ipc.request("dance")


def test_open_registers_the_file(window: Studio) -> None:
    result = window.open(REPO / "VERSION")
    assert result["fileCount"] == 1
    assert window.get("Project.fileCount") == 1
    assert window.get("Project.activeFile") == result["activeFile"]
    window.wait_idle()


def test_click_opens_the_jobs_popover(window: Studio) -> None:
    assert window.get("jobsPopover.opened") is False
    window.click("jobsButton")
    window.wait_idle()
    assert window.get("jobsPopover.opened") is True


def test_key_runs_the_shortcut_of_the_keymap(window: Studio) -> None:
    assert window.get("jobsPopover.opened") is False
    window.key("Ctrl+J")  # action view.jobs (chapter 11 section 11.4.3)
    window.wait_idle()
    assert window.get("jobsPopover.opened") is True


def test_screenshot_writes_a_png(window: Studio, tmp_path: Path) -> None:
    out = tmp_path / "main.png"
    value = window.screenshot(out)
    assert png_size(out) == (value["width"], value["height"])
    assert value["width"] >= 1024 and value["height"] >= 640
    with pytest.raises(IpcError, match="cannot write"):
        window.screenshot(tmp_path / "missing" / "main.png")


def test_wait_idle_answers_and_validates(window: Studio) -> None:
    assert window.wait_idle(5000) is None
    with pytest.raises(IpcError, match="usage"):
        window.ipc.request("wait-idle", "soon")


def test_requests_are_answered_in_order(window: Studio) -> None:
    # wait-idle answers late; a get sent behind it must still come second. The client is blocking, so pipeline by hand.
    raw = window.ipc._sock  # noqa: SLF001
    raw.sendall(b'{"id":101,"cmd":"wait-idle","args":[3000]}\n{"id":102,"cmd":"get","args":["Project.fileCount"]}\n')
    f = raw.makefile("rb")
    first, second = f.readline(), f.readline()
    assert b'"id":101' in first
    assert b'"id":102' in second


def test_the_socket_is_private_to_the_user(ovt_studio_exe: str) -> None:
    workdir = Path(tempfile.mkdtemp(prefix="ovt-ipc-"))
    try:
        from conftest import launch_studio

        process, client = launch_studio(ovt_studio_exe, workdir)
        try:
            mode = stat.S_IMODE(os.stat(workdir / "ipc.sock").st_mode)
            assert mode & 0o077 == 0, oct(mode)
        finally:
            client.close()
            process.kill()
            process.wait()
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


def test_no_flag_no_socket(ovt_studio_exe: str, tmp_path: Path) -> None:
    """WI-1.465 acceptance test 2: without --test-ipc the application creates no socket."""
    env = dict(os.environ)
    env.update(
        QT_QPA_PLATFORM="offscreen",
        OVT_RENDER_STUB="1",
        OVT_CONFIG_DIR=str(tmp_path / "config"),
        TMPDIR=str(tmp_path),
        XDG_RUNTIME_DIR=str(tmp_path),
    )
    result = subprocess.run([ovt_studio_exe, "--self-test"], env=env, capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    sockets = [p for p in tmp_path.rglob("*") if stat.S_ISSOCK(p.lstat().st_mode)]
    assert sockets == []
