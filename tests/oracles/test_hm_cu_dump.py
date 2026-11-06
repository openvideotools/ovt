#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Acceptance test 1 of WI-0.563 (chapter 14 §14.4.4): run hm_cu_dump on an HEVC stream with AMP partitions.

    test_hm_cu_dump.py <path to hm_cu_dump>

Given jctvc_AMP_A_Samsung_7 (OVT_CORPORA_DIR), a grid with getPartitionSize in {4,5,6,7} (AMP) appears. Without the
corpus file the stream is encoded here with libx265 (amp=1:rect=1). The dump must also satisfy the contract of
normalize/hm_grid.py (every getter of hm_grid_map.json, w4*h4 cells each) and be deterministic. Exit 77 (CTest skip)
when neither the corpus nor ffmpeg with libx265 exists.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SKIP = 77
HERE = Path(__file__).resolve().parent


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
    given = corpus_file("jctvc_AMP_A_Samsung_7")
    if given is not None and given.is_file():
        return given
    ff = shutil.which("ffmpeg")
    if ff is None or "libx265" not in run([ff, "-hide_banner", "-encoders"]).stdout:
        return None
    out = work / "m.265"
    r = run([ff, "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=208x120:rate=30", "-frames:v", "10",
             "-pix_fmt", "yuv420p", "-c:v", "libx265", "-preset", "slow", "-x265-params",
             "amp=1:rect=1:keyint=6:bframes=3:ref=3:info=0:log-level=error", "-f", "hevc", str(out)])  # fmt: skip
    assert r.returncode == 0, r.stderr
    return out


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
                "SKIP: no jctvc_AMP_A_Samsung_7 in OVT_CORPORA_DIR and no ffmpeg with libx265"
            )
            return SKIP

        res = run([exe, "-b", str(stream)])
        assert res.returncode == 0, res.stderr
        doc = json.loads(res.stdout)
        frames = doc["frames"]
        assert len(frames) >= 2, len(frames)

        hm_map = json.loads((HERE / "normalize" / "hm_grid_map.json").read_text())
        wanted = (
            set(hm_map["copy"])
            | set(hm_map["mv_quarter"])
            | set(hm_map["derived_from"])
            | set(hm_map["ignored"])
        )
        for f in frames:
            assert set(f["cells"]) == wanted, set(f["cells"]) ^ wanted
            n = f["w4"] * f["h4"]
            assert all(len(v) == n for v in f["cells"].values())
            assert f["log2_ctu_size"] in (4, 5, 6)

        # Acceptance 1: AMP partitions appear (SIZE_2NxnU=4, 2NxnD=5, nLx2N=6, nRx2N=7).
        sizes = {v for f in frames for v in f["cells"]["getPartitionSize"]}
        assert sizes & {4, 5, 6, 7}, f"no AMP partition among {sorted(sizes)}"

        assert run([exe, "-b", str(stream)]).stdout == res.stdout, "not deterministic"

        (work / "grid.json").write_text(res.stdout)
        r = run(
            [
                sys.executable,
                "-I",
                str(HERE / "normalize" / "hm_grid.py"),
                str(work / "grid.json"),
            ]
        )
        assert r.returncode == 0, r.stderr

        assert run([exe]).returncode == 2
        assert run([exe, "-b", str(work / "absent.bit")]).returncode == 1
    print("hm_cu_dump: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
