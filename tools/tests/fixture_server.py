# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Local HTTP fixture server (chapter 14 section 14.2.4; WI-0.551; D27).

``serve(root)`` serves a directory over ``http://127.0.0.1:<port>/`` from a background thread, with ``Range``
support (single ``bytes=a-b`` ranges), optional corruption and latency. Used as a pytest fixture by the fetcher
tests and by the chapter 8 and 12 DASH/HLS acceptance tests.

``corrupt`` names the files to damage: an iterable of names (the last byte is flipped, Content-Length stays
right) or a mapping ``name -> mode`` with mode ``"flip"`` or ``"truncate"`` (the response promises the full
length but the connection closes after half of it).
"""

from __future__ import annotations

import contextlib
import functools
import http.server
import re
import threading
import time
from collections.abc import Iterable, Iterator, Mapping
from pathlib import Path

_RANGE = re.compile(r"^bytes=(\d*)-(\d*)$")


class _Handler(http.server.SimpleHTTPRequestHandler):
    corrupt: Mapping[str, str]
    delay_ms: int

    def log_message(self, format: str, *args: object) -> None:
        pass

    def do_GET(self) -> None:
        if self.delay_ms:
            time.sleep(self.delay_ms / 1000)
        path = Path(self.translate_path(self.path.split("?", 1)[0]))
        if not path.is_file():
            self.send_error(404)
            return
        data = path.read_bytes()
        mode = self.corrupt.get(self.path.lstrip("/").split("?", 1)[0])
        if mode == "flip" and data:
            data = data[:-1] + bytes([data[-1] ^ 0xFF])
        start, end, status = 0, len(data) - 1, 200
        header = self.headers.get("Range")
        if header:
            m = _RANGE.match(header.strip())
            if not m or (not m.group(1) and not m.group(2)):
                self.send_error(416)
                return
            if m.group(1):
                start = int(m.group(1))
                end = min(int(m.group(2)), len(data) - 1) if m.group(2) else len(data) - 1
            else:  # suffix range
                start = max(len(data) - int(m.group(2)), 0)
            if start > end or start >= len(data):
                self.send_error(416)
                return
            status = 206
        body = data[start : end + 1]
        self.send_response(status)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{len(data)}")
        self.end_headers()
        if mode == "truncate":
            self.wfile.write(body[: len(body) // 2])
            self.wfile.flush()
            self.close_connection = True
            return
        self.wfile.write(body)

    def do_HEAD(self) -> None:
        path = Path(self.translate_path(self.path.split("?", 1)[0]))
        if not path.is_file():
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(path.stat().st_size))
        self.end_headers()


@contextlib.contextmanager
def serve(
    root: str | Path, *, corrupt: Iterable[str] | Mapping[str, str] | None = None, delay_ms: int = 0, port: int = 0
) -> Iterator[str]:
    """Serve ``root`` and yield the base URL (``http://127.0.0.1:<port>/``); stops the server on exit."""
    modes = dict(corrupt) if isinstance(corrupt, Mapping) else {name: "flip" for name in (corrupt or ())}
    handler = type("Handler", (_Handler,), {"corrupt": modes, "delay_ms": delay_ms})
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), functools.partial(handler, directory=str(root)))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_address[1]}/"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
