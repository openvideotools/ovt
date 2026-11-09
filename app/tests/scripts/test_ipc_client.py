# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""The IPC client and the ``studio`` fixture against a fake ``ovt-studio`` (WI-0.568 step 4).

The real server arrives with WI-1.465; the fake implements the proposed wire format of conftest.py so the client
can be tested now and the server tested against the same client later.
"""

from __future__ import annotations

import stat
import sys
import tempfile
import textwrap
from pathlib import Path

import pytest
from conftest import IpcClient, IpcError, Studio, connect_unix, launch_studio

FAKE = textwrap.dedent(
    f"""\
    #!{sys.executable}
    import json, os, socket, sys
    assert sys.argv[1] == "--test-ipc", sys.argv
    assert os.environ.get("OVT_RENDER_STUB") == "1"
    path = sys.argv[2]
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(1)
    conn, _ = srv.accept()
    f = conn.makefile("rwb", buffering=0)
    state = {{"opened": None}}
    for line in f:
        req = json.loads(line)
        cmd, args = req["cmd"], req["args"]
        if cmd == "open":
            state["opened"] = args[0]
            out = {{"ok": True, "value": None}}
        elif cmd == "get":
            out = {{"ok": True, "value": {{"picture.displayIndex": 37, "app.opened": state["opened"]}}.get(args[0])}}
        elif cmd == "click":
            out = {{"ok": True, "value": args}}
        elif cmd == "die":
            sys.exit(3)
        else:
            out = {{"ok": False, "error": "unknown command " + cmd}}
        out["id"] = req["id"]
        f.write((json.dumps(out) + "\\n").encode())
    """
)


@pytest.fixture
def fake_studio(tmp_path: Path) -> str:
    exe = tmp_path / "ovt-studio"
    exe.write_text(FAKE)
    exe.chmod(exe.stat().st_mode | stat.S_IXUSR)
    return str(exe)


@pytest.fixture
def client(fake_studio: str):
    workdir = Path(tempfile.mkdtemp(prefix="ovt-ipc-test-"))
    process, ipc = launch_studio(fake_studio, workdir)
    yield ipc
    ipc.close()
    process.kill()
    process.wait()


def test_commands_roundtrip(client: IpcClient) -> None:
    studio = Studio(client)
    studio.open("/x/y.mp4")
    assert studio.get("app.opened") == "/x/y.mp4"
    assert studio.click("timeline", 37) == ["timeline", 37]
    assert studio.click("picture", 200, 120) == ["picture", 200, 120]
    assert studio.get("picture.displayIndex") == 37


def test_error_response_raises(client: IpcClient) -> None:
    with pytest.raises(IpcError, match="unknown command nope"):
        client.request("nope")


def test_application_exit_is_reported(client: IpcClient) -> None:
    with pytest.raises(IpcError, match="connection closed"):
        client.request("die")


def test_connect_gives_up(tmp_path: Path) -> None:
    with pytest.raises(IpcError, match="cannot connect"):
        connect_unix(str(tmp_path / "missing.sock"), 0.2)
