#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Pre-fetch every third-party archive for an offline build (chapter 13 §13.3.4, WI-0.010).

    third_party_offline.py --out DIR [--versions third_party/VERSIONS.md] [--only DEP...]

Downloads the archive of every third_party/VERSIONS.md row whose URL is an archive and verifies it against
the row's SHA-256. Point OVT_TP_DOWNLOADS at DIR (`-DOVT_TP_DOWNLOADS=DIR`) and configure finds the archives
there instead of downloading (F-Droid and other offline builds). An archive already in DIR with the right
hash is kept; one with a wrong hash is downloaded again. A row whose SHA-256 is still a placeholder fails:
fill it with `tools/versions_to_cmake.py --update-hashes` first.

Exit status 0 when every archive is present and verified, 1 otherwise (each failure names its row).
Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # `python -I` drops the script directory

import versions_to_cmake as vtc  # noqa: E402


def fetch(row: vtc.Row, out: Path) -> str | None:
    """Returns an error message, or None when the archive is in place and verified."""
    if not row.sha256:
        return f"{row.where()}: no SHA-256 recorded (run tools/versions_to_cmake.py --update-hashes)"
    dest = out / vtc.archive_name(row)
    if dest.exists() and vtc.sha256_file(dest) == row.sha256:
        print(f"ok        {row.name}: {dest.name}")
        return None
    try:
        print(f"download  {row.name}: {row.url}")
        vtc.download(row.url, dest)
    except OSError as e:
        return f"{row.where()}: download of {row.url} failed: {e}"
    got = vtc.sha256_file(dest)
    if got != row.sha256:
        dest.unlink()
        return f"{row.where()}: SHA-256 mismatch for {row.url}: got {got}, the row pins {row.sha256}"
    print(f"verified  {row.name}: {dest.name}")
    return None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, required=True, help="download directory (OVT_TP_DOWNLOADS)")
    ap.add_argument("--versions", type=Path, default=vtc.DEFAULT_VERSIONS, help="VERSIONS.md to read")
    ap.add_argument("--only", nargs="+", metavar="DEP", help="restrict to these row ids (libaom, ffmpeg, ...)")
    args = ap.parse_args(argv)
    try:
        rows = vtc.parse(args.versions.read_text(encoding="utf-8"))
    except (vtc.VersionsError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    rows = [r for r in rows if r.is_archive]
    if args.only:
        unknown = set(args.only) - {r.dep for r in rows}
        if unknown:
            print(f"error: no archive row with id {', '.join(sorted(unknown))}", file=sys.stderr)
            return 1
        rows = [r for r in rows if r.dep in args.only]
    args.out.mkdir(parents=True, exist_ok=True)
    errors = [err for err in (fetch(r, args.out) for r in rows) if err]
    for err in errors:
        print(f"error: {err}", file=sys.stderr)
    print(f"{len(rows) - len(errors)} of {len(rows)} archives verified in {args.out}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
