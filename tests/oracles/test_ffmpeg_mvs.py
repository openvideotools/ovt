#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Acceptance test 3 of WI-0.563 (chapter 14 §14.4.3a): run ffmpeg_mvs on an H.264 stream with P frames.

    test_ffmpeg_mvs.py <path to ffmpeg_mvs>

Given jvt_BA1_Sony_D (OVT_CORPORA_DIR, fetched by tools/fetch_corpora.py), every P frame has at least one row and
every row has motion_scale == 4. Without the corpus file the stream is encoded here with libx264 from a lavfi test
pattern (SPEC-ASSUMPTION(#181): any baseline stream with P frames satisfies the test). The --qp CSV and the
normaliser round trip are checked too. Exit 77 (CTest skip) when neither the corpus nor ffmpeg with libx264 exists.
"""

from __future__ import annotations

import csv
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SKIP = 77
HERE = Path(__file__).resolve().parent
COLUMNS = "framenum,source,blockw,blockh,srcx,srcy,dstx,dsty,flags,motion_x,motion_y,motion_scale".split(
    ","
)


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, check=False, **kw)  # noqa: S603


def corpus_file(corpus_id: str) -> Path | None:
    root = os.environ.get("OVT_CORPORA_DIR")
    if not root:
        return None
    entry = Path(root) / corpus_id
    ok = entry / ".ok"
    if not ok.is_file():
        return None
    try:
        primary = json.loads(ok.read_text()).get("primary")
    except ValueError:
        primary = None
    if primary:
        return entry / primary
    files = [f for f in entry.iterdir() if f.is_file() and f.name != ".ok"]
    return files[0] if len(files) == 1 else None


def make_stream(work: Path) -> Path | None:
    given = corpus_file("jvt_BA1_Sony_D")
    if given is not None and given.is_file():
        return given
    ff = shutil.which("ffmpeg")
    if ff is None or "libx264" not in run([ff, "-hide_banner", "-encoders"]).stdout:
        return None
    out = work / "m.264"
    r = run([ff, "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=176x144:rate=30", "-frames:v", "10",
             "-pix_fmt", "yuv420p", "-c:v", "libx264", "-profile:v", "baseline", "-x264-params",
             "keyint=30:scenecut=0:bframes=0", "-f", "h264", str(out)])  # fmt: skip
    assert r.returncode == 0, r.stderr
    return out


def frame_types(path: Path) -> list[str]:
    """Picture types in decode (= output, no B frames) order, from ffprobe."""
    probe = shutil.which("ffprobe")
    if probe is None:
        return []
    r = run([probe, "-v", "error", "-select_streams", "v:0", "-show_entries", "frame=pict_type", "-of", "csv=p=0",
             str(path)])  # fmt: skip
    return [line.strip().rstrip(",") for line in r.stdout.splitlines() if line.strip()]


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    exe = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        stream = make_stream(work)
        if stream is None:
            print(
                "SKIP: no jvt_BA1_Sony_D in OVT_CORPORA_DIR and no ffmpeg with libx264"
            )
            return SKIP

        qp_path = work / "qp.csv"
        res = run([exe, "--qp", str(qp_path), str(stream)])
        assert res.returncode == 0, res.stderr
        rows = list(csv.DictReader(res.stdout.splitlines()))
        assert rows, "no motion vector rows"
        assert list(rows[0].keys()) == COLUMNS, list(rows[0].keys())
        assert all(int(r["motion_scale"]) == 4 for r in rows), (
            "motion_scale must be 4 for H.264"
        )
        assert all(r["flags"].startswith("0x") for r in rows)
        with_rows = {int(r["framenum"]) for r in rows}

        types = frame_types(stream)
        if types:
            p_frames = [i + 1 for i, t in enumerate(types) if t == "P"]
            assert p_frames, types
            missing = [n for n in p_frames if n not in with_rows]
            assert not missing, f"P frames without a row: {missing}"
        assert min(with_rows) >= 1

        qp_rows = list(csv.DictReader(qp_path.read_text().splitlines()))
        assert qp_rows and set(qp_rows[0]) == {
            "framenum",
            "src_x",
            "src_y",
            "w",
            "h",
            "qp",
            "delta_qp",
        }
        assert all(int(r["w"]) == 16 and int(r["h"]) == 16 for r in qp_rows), (
            "H.264 QP blocks are macroblocks"
        )

        # Without --qp the second file is not written and stdout is the same.
        again = run([exe, str(stream)])
        assert again.returncode == 0 and again.stdout == res.stdout

        # Determinism: a second run is byte-identical (§0.7.5).
        assert run([exe, str(stream)]).stdout == again.stdout

        # The normalisers accept the files.
        norm = HERE / "normalize" / "ffmpeg_mvs.py"
        (work / "mv.csv").write_text(res.stdout)
        for args in ([str(work / "mv.csv")], ["--qp", str(qp_path)]):
            r = run([sys.executable, "-I", str(norm), *args])
            assert r.returncode == 0, r.stderr

        # Usage and error exits.
        assert run([exe]).returncode == 2
        assert run([exe, str(work / "absent.264")]).returncode == 1
    print("ffmpeg_mvs: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
