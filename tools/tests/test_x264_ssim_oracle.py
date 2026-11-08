# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of the x264_ssim oracle program (tests/oracles/x264_ssim, chapter 14 §14.4.12; WI-0.565).

The C program is built with the system compiler and run on small random Y4M pairs. Its per-plane values are compared
with an independent transcription of chapter 9 §9.4.4 in float64 (the C code is float32, so within 2e-6).
"""

from __future__ import annotations

import random
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "tests" / "oracles" / "x264_ssim"

pytestmark = pytest.mark.skipif(
    not (shutil.which("cmake") and shutil.which("cc")),
    reason="needs cmake and a C compiler",
)


@pytest.fixture(scope="module")
def oracle(tmp_path_factory: pytest.TempPathFactory) -> Path:
    build = tmp_path_factory.mktemp("x264_ssim_build")
    subprocess.run(["cmake", "-S", str(SRC), "-B", str(build)], check=True, capture_output=True)
    subprocess.run(["cmake", "--build", str(build)], check=True, capture_output=True)
    return build / "x264_ssim"


def write_y4m(path: Path, frames: list[list[list[int]]], w: int, h: int, depth: int) -> None:
    tag = "C420p10" if depth == 10 else "C420jpeg"
    with path.open("wb") as f:
        f.write(f"YUV4MPEG2 W{w} H{h} F25:1 Ip A0:0 {tag}\n".encode())
        for planes in frames:
            f.write(b"FRAME\n")
            for plane in planes:
                if depth == 10:
                    f.write(b"".join(v.to_bytes(2, "little") for v in plane))
                else:
                    f.write(bytes(plane))


def random_frames(rng: random.Random, n: int, w: int, h: int, depth: int, ref: list | None = None) -> list:
    peak = (1 << depth) - 1
    sizes = [w * h, (w // 2) * (h // 2), (w // 2) * (h // 2)]
    out = []
    for i in range(n):
        planes = []
        for p, size in enumerate(sizes):
            if ref is None:
                planes.append([rng.randint(0, peak) for _ in range(size)])
            else:
                planes.append([min(peak, max(0, v + rng.randint(-12, 12))) for v in ref[i][p]])
        out.append(planes)
    return out


def reference_ssim(a: list[int], b: list[int], w: int, h: int, depth: int) -> float:
    """§9.4.4 steps 1-4 in float64."""
    peak = (1 << depth) - 1
    pw, ph = w - 2, h - 2
    bw, bh = pw // 4, ph // 4
    sums = {}
    for by in range(bh):
        for bx in range(bw):
            s1 = s2 = ss = s12 = 0
            for y in range(4):
                for x in range(4):
                    i = 2 + (2 + by * 4 + y) * w + bx * 4 + x
                    s1 += a[i]
                    s2 += b[i]
                    ss += a[i] * a[i] + b[i] * b[i]
                    s12 += a[i] * b[i]
            sums[bx, by] = (s1, s2, ss, s12)
    if depth <= 9:
        c1 = int(0.01 * 0.01 * peak * peak * 64 + 0.5)
        c2 = int(0.03 * 0.03 * peak * peak * 64 * 63 + 0.5)
    else:
        c1 = 0.01 * 0.01 * peak * peak * 64
        c2 = 0.03 * 0.03 * peak * peak * 64 * 63
    total, count = 0.0, 0
    for wy in range(bh - 1):
        for wx in range(bw - 1):
            fs = [sum(sums[wx + dx, wy + dy][k] for dx in (0, 1) for dy in (0, 1)) for k in range(4)]
            fs1, fs2, fss, fs12 = fs
            vars_ = fss * 64 - fs1 * fs1 - fs2 * fs2
            covar = fs12 * 64 - fs1 * fs2
            total += ((2 * fs1 * fs2 + c1) * (2 * covar + c2)) / ((fs1 * fs1 + fs2 * fs2 + c1) * (vars_ + c2))
            count += 1
    return total / count


@pytest.mark.parametrize("depth", [8, 10])
def test_matches_the_chapter_9_transcription(oracle: Path, tmp_path: Path, depth: int) -> None:
    rng = random.Random(depth)
    w, h = 48, 38
    ref = random_frames(rng, 3, w, h, depth)
    dist = random_frames(rng, 3, w, h, depth, ref)
    write_y4m(tmp_path / "r.y4m", ref, w, h, depth)
    write_y4m(tmp_path / "d.y4m", dist, w, h, depth)
    res = subprocess.run(
        [str(oracle), str(tmp_path / "r.y4m"), str(tmp_path / "d.y4m")],
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0, res.stderr
    lines = res.stdout.splitlines()
    assert len(lines) == 3
    for i, line in enumerate(lines):
        words = line.split()
        assert words[0] == f"n:{i}"
        got = [float(wd[2:]) for wd in words[1:]]
        sizes = [(w, h), (w // 2, h // 2), (w // 2, h // 2)]
        for p in range(3):
            want = reference_ssim(ref[i][p], dist[i][p], *sizes[p], depth)
            assert abs(got[p] - want) < 2e-6, (depth, i, p, got[p], want)


def test_identical_frames_give_one_and_the_output_is_deterministic(oracle: Path, tmp_path: Path) -> None:
    ref = random_frames(random.Random(7), 2, 32, 32, 8)
    write_y4m(tmp_path / "r.y4m", ref, 32, 32, 8)
    runs = [
        subprocess.run(
            [str(oracle), str(tmp_path / "r.y4m"), str(tmp_path / "r.y4m")],
            capture_output=True,
            text=True,
        )
        for _ in range(2)
    ]
    assert runs[0].returncode == 0
    assert runs[0].stdout == runs[1].stdout
    assert runs[0].stdout.splitlines()[0] == "n:0 Y:1.000000000 U:1.000000000 V:1.000000000"


def test_mismatched_inputs_fail(oracle: Path, tmp_path: Path) -> None:
    rng = random.Random(3)
    write_y4m(tmp_path / "a.y4m", random_frames(rng, 2, 32, 32, 8), 32, 32, 8)
    write_y4m(tmp_path / "b.y4m", random_frames(rng, 3, 32, 32, 8), 32, 32, 8)
    write_y4m(tmp_path / "c.y4m", random_frames(rng, 2, 32, 32, 10), 32, 32, 10)
    for other in ("b", "c"):
        res = subprocess.run(
            [str(oracle), str(tmp_path / "a.y4m"), str(tmp_path / f"{other}.y4m")],
            capture_output=True,
            text=True,
        )
        assert res.returncode == 1
    assert subprocess.run([str(oracle), str(tmp_path / "a.y4m")], capture_output=True).returncode == 2
