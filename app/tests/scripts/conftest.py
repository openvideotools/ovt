# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixtures of the interaction scripts (chapter 14 section 14.10.3; WI-0.568).

``ipc`` launches ``ovt-studio --test-ipc <socket>`` and connects to the socket; ``studio(ipc)`` is the command set of
14.10.3 on top of it. The server is WI-1.465; until it lands, a script that uses ``studio`` is skipped when the
binary is not found, and the client is tested against a fake server (test_ipc_client.py).

Wire format (SPEC-ASSUMPTION(#353), to be implemented by WI-1.465): JSON lines
over a Unix domain socket (``QLocalServer`` on Linux and macOS). One request per line,
``{"id": <int>, "cmd": "<name>", "args": [<json>...]}``; one response per request, in order,
``{"id": <int>, "ok": true, "value": <json>}`` or ``{"id": <int>, "ok": false, "error": "<text>"}``.
Commands: ``open <path>``, ``click <objectName> [row] | click <objectName> <x> <y>``, ``select-syntax-node <path>``,
``key <sequence>``, ``get <objectName>.<property>``, ``screenshot <path>``, ``wait-idle [timeout_ms]``.
"""

from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from collections.abc import Iterator
from pathlib import Path
from typing import Any

import pytest

REPO = Path(__file__).resolve().parents[3]
CONNECT_TIMEOUT_S = 15.0
REQUEST_TIMEOUT_S = 30.0


class IpcError(Exception):
    """The application answered ``ok: false`` or the connection failed."""


class IpcClient:
    """A blocking JSON-lines client of the test IPC."""

    def __init__(self, sock: socket.socket, process: subprocess.Popen | None = None) -> None:
        self._sock = sock
        self._file = sock.makefile("rwb", buffering=0)
        self._process = process
        self._next_id = 1

    def request(self, cmd: str, *args: Any, timeout: float = REQUEST_TIMEOUT_S) -> Any:
        """Send one command and return the ``value`` of its response."""
        request_id = self._next_id
        self._next_id += 1
        line = json.dumps({"id": request_id, "cmd": cmd, "args": list(args)}, separators=(",", ":")) + "\n"
        self._sock.settimeout(timeout)
        try:
            self._file.write(line.encode("utf-8"))
            raw = self._file.readline()
        except (OSError, TimeoutError) as err:
            raise IpcError(f"{cmd}: {err}") from err
        if not raw:
            code = self._process.poll() if self._process is not None else None
            raise IpcError(
                f"{cmd}: connection closed" + (f" (application exited with {code})" if code is not None else "")
            )
        reply = json.loads(raw)
        if reply.get("id") != request_id:
            raise IpcError(f"{cmd}: response id {reply.get('id')} for request {request_id}")
        if not reply.get("ok", False):
            raise IpcError(f"{cmd}: {reply.get('error', 'failed')}")
        return reply.get("value")

    def close(self) -> None:
        try:
            self._file.close()
            self._sock.close()
        except OSError:
            pass


class Studio:
    """The command set of 14.10.3. Every method returns the response value."""

    def __init__(self, ipc: IpcClient) -> None:
        self.ipc = ipc

    def open(self, path: str | os.PathLike) -> Any:
        return self.ipc.request("open", os.fspath(path))

    def click(self, object_name: str, *where: int) -> Any:
        """``click("timeline", 37)`` (a row/frame index) or ``click("picture", 200, 120)`` (item-local x, y)."""
        return self.ipc.request("click", object_name, *where)

    def select_syntax_node(self, path: str) -> Any:
        return self.ipc.request("select-syntax-node", path)

    def key(self, sequence: str) -> Any:
        return self.ipc.request("key", sequence)

    def get(self, name_dot_property: str) -> Any:
        """``get("hexView.highlightedBitSpan")``."""
        return self.ipc.request("get", name_dot_property)

    def screenshot(self, path: str | os.PathLike) -> Any:
        return self.ipc.request("screenshot", os.fspath(path))

    def wait_idle(self, timeout_ms: int = 10000) -> Any:
        """No running jobs and no pending render (WI-1.465)."""
        return self.ipc.request("wait-idle", timeout_ms, timeout=timeout_ms / 1000.0 + 5.0)


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption("--ovt-studio", default=os.environ.get("OVT_STUDIO"), help="path of the ovt-studio executable")
    parser.addoption(
        "--ovt-studio-screenshots",
        default=os.environ.get("OVT_STUDIO_SCREENSHOTS"),
        help="path of the ovt_studio_screenshots executable",
    )


def connect_unix(path: str, timeout: float, process: subprocess.Popen | None = None) -> socket.socket:
    """Connect to a Unix socket that the application creates some time after it starts."""
    deadline = time.monotonic() + timeout
    last: Exception | None = None
    while time.monotonic() < deadline:
        if process is not None and process.poll() is not None:
            raise IpcError(f"application exited with {process.returncode} before the socket appeared")
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.connect(path)
            return sock
        except OSError as err:
            last = err
            sock.close()
            time.sleep(0.05)
    raise IpcError(f"cannot connect to {path} within {timeout:.0f} s: {last}")


def launch_studio(
    exe: str, workdir: Path, extra_env: dict[str, str] | None = None
) -> tuple[subprocess.Popen, IpcClient]:
    """Start ``exe --test-ipc <socket>`` and connect. Offscreen, stub renderer, private settings directory."""
    sock_path = str(workdir / "ipc.sock")
    env = dict(os.environ)
    env.setdefault("QT_QPA_PLATFORM", "offscreen" if sys.platform.startswith("linux") else "")
    if not env["QT_QPA_PLATFORM"]:
        del env["QT_QPA_PLATFORM"]
    env.update(
        {"OVT_RENDER_STUB": "1", "OVT_CONFIG_DIR": str(workdir / "config"), "QT_SCALE_FACTOR": "1", "QT_FONT_DPI": "96"}
    )
    env.update(extra_env or {})
    process = subprocess.Popen(
        [exe, "--test-ipc", sock_path], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE
    )
    try:
        client = IpcClient(connect_unix(sock_path, CONNECT_TIMEOUT_S, process), process)
    except IpcError:
        process.kill()
        process.wait()
        raise
    return process, client


@pytest.fixture
def ovt_studio_exe(request: pytest.FixtureRequest) -> str:
    """The ``ovt-studio`` executable, or a skip when none was given and none was found in a build tree."""
    path = request.config.getoption("--ovt-studio")
    if not path:
        found = sorted(REPO.glob("build/*/app/ovt-studio"))
        path = str(found[0]) if found else shutil.which("ovt-studio")
    if not path or not Path(path).exists():
        pytest.skip("ovt-studio not found (pass --ovt-studio or set OVT_STUDIO)")
    return path


@pytest.fixture
def ipc(ovt_studio_exe: str) -> Iterator[IpcClient]:
    """A connection to a freshly started ``ovt-studio --test-ipc``; the application is stopped afterwards."""
    # Unix socket paths are short (104 bytes on macOS), so the directory is under the system temp, not pytest's.
    workdir = Path(tempfile.mkdtemp(prefix="ovt-ipc-"))
    process, client = launch_studio(ovt_studio_exe, workdir)
    try:
        yield client
    finally:
        client.close()
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        shutil.rmtree(workdir, ignore_errors=True)


@pytest.fixture
def studio(ipc: IpcClient) -> Studio:
    """The command set of 14.10.3 over ``ipc``."""
    return Studio(ipc)
