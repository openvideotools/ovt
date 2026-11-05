#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Serve a directory over local HTTP: ``python3 tests/serve_corpus.py --root <dir> --port 0`` (WI-0.551).

Prints the bound base URL on the first line of stdout, then serves until interrupted.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "tests"))
from fixture_server import serve  # noqa: E402


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--root", required=True, type=Path)
    p.add_argument("--port", type=int, default=8080)
    args = p.parse_args()
    with serve(args.root, port=args.port) as url:
        print(url, flush=True)
        try:
            while True:
                time.sleep(3600)
        except KeyboardInterrupt:
            return 0


if __name__ == "__main__":
    sys.exit(main())
