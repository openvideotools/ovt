# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/imgdiff.py (chapter 14 section 14.10.2; WI-0.568 acceptance tests 1 and 2)."""

from __future__ import annotations

import json
import struct
import subprocess
import sys
import zlib
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))

pytest.importorskip("numpy")  # NumPy is a pytest-lane requirement (tools/requirements-test.txt)

import imgdiff
import numpy as np

IMGDIFF = TOOLS / "imgdiff.py"


def gradient(width: int = 200, height: int = 120) -> np.ndarray:
    x = np.linspace(0, 255, width)[None, :].repeat(height, axis=0)
    y = np.linspace(0, 255, height)[:, None].repeat(width, axis=1)
    return np.stack([x, y, (x + y) / 2], axis=-1).astype(np.uint8)


def write_png16(path: Path, pix: np.ndarray, filter_type: int = 0) -> None:
    """Minimal RGB16 PNG writer for the tests (the tool itself only writes 8-bit PNGs)."""
    height, width, _ = pix.shape
    be = pix.astype(">u2").tobytes()
    row = width * 6
    raw = b""
    prev = bytes(row)
    for y in range(height):
        line = be[y * row : (y + 1) * row]
        if filter_type == 2:  # Up
            raw += b"\x02" + bytes((a - b) & 0xFF for a, b in zip(line, prev))
        elif filter_type == 1:  # Sub
            raw += b"\x01" + bytes((line[i] - (line[i - 6] if i >= 6 else 0)) & 0xFF for i in range(row))
        else:
            raw += b"\x00" + line
        prev = line

    def chunk(t: bytes, b: bytes) -> bytes:
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 16, 2, 0, 0, 0)
    path.write_bytes(
        imgdiff.PNG_SIGNATURE + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    )


def run(*args: object) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-I", str(IMGDIFF), *map(str, args)], capture_output=True, text=True, check=False
    )


def test_png_roundtrip(tmp_path: Path) -> None:
    pix = gradient()
    imgdiff.write_png(tmp_path / "a.png", pix)
    back, depth = imgdiff.read_png(tmp_path / "a.png")
    assert depth == 8 and np.array_equal(back, pix)


@pytest.mark.parametrize("filter_type", [0, 1, 2])
def test_png16_read(tmp_path: Path, filter_type: int) -> None:
    pix = (np.arange(4 * 5 * 3, dtype=np.uint32).reshape(4, 5, 3) * 997 % 65536).astype(np.uint16)
    write_png16(tmp_path / "p.png", pix, filter_type)
    back, depth = imgdiff.read_png(tmp_path / "p.png")
    assert depth == 16 and np.array_equal(back, pix)


def test_ui_identical_and_red_square(tmp_path: Path) -> None:
    """Acceptance test 1."""
    pytest.importorskip("skimage")
    a = gradient()
    imgdiff.write_png(tmp_path / "a.png", a)
    imgdiff.write_png(tmp_path / "same.png", a)
    ok = run("--mode", "ui", tmp_path / "a.png", tmp_path / "same.png", "--diff", tmp_path / "none.png")
    assert ok.returncode == 0, ok.stdout + ok.stderr
    assert not (tmp_path / "none.png").exists()

    b = a.copy()
    b[40:60, 90:110] = (255, 0, 0)
    imgdiff.write_png(tmp_path / "b.png", b)
    bad = run("--mode", "ui", tmp_path / "a.png", tmp_path / "b.png", "--diff", tmp_path / "diff.png")
    assert bad.returncode == 1, bad.stdout + bad.stderr
    diff, _ = imgdiff.read_png(tmp_path / "diff.png")
    assert diff.shape == (120, 3 * 200 + 8, 3)


def test_ui_tolerates_noise(tmp_path: Path) -> None:
    pytest.importorskip("skimage")
    a = gradient()
    b = a.copy()
    b[::7, ::5] = np.clip(b[::7, ::5].astype(int) + 2, 0, 255).astype(np.uint8)
    imgdiff.write_png(tmp_path / "a.png", a)
    imgdiff.write_png(tmp_path / "b.png", b)
    assert run("--mode", "ui", tmp_path / "a.png", tmp_path / "b.png").returncode == 0


def test_ui_size_mismatch_is_error(tmp_path: Path) -> None:
    imgdiff.write_png(tmp_path / "a.png", gradient(10, 10))
    imgdiff.write_png(tmp_path / "b.png", gradient(12, 10))
    res = run("--mode", "ui", tmp_path / "a.png", tmp_path / "b.png")
    assert res.returncode == 2 and "size mismatch" in res.stderr


def test_not_a_png_is_error(tmp_path: Path) -> None:
    (tmp_path / "x.png").write_bytes(b"nope")
    imgdiff.write_png(tmp_path / "a.png", gradient(10, 10))
    assert run("--mode", "ui", tmp_path / "x.png", tmp_path / "a.png").returncode == 2


def test_pq_roundtrip() -> None:
    nits = np.array([0.0, 0.01, 1.0, 100.0, 1000.0, 10000.0])
    assert np.allclose(imgdiff.pq_eotf(imgdiff.pq_oetf(nits)), nits, rtol=1e-6, atol=1e-6)
    assert imgdiff.pq_eotf(np.array([0.508078]))[0] == pytest.approx(100.0, rel=1e-3)  # 100 nits = PQ 0.508


def pq_image(width: int = 64, height: int = 48) -> np.ndarray:
    """A PQ image with a spread of levels from 5 to 1000 nits in all three channels."""
    level = np.geomspace(5.0, 1000.0, width)[None, :].repeat(height, axis=0)
    nits = np.stack([level, level[:, ::-1], np.full_like(level, 50.0)], axis=-1)
    return nits


def test_hdr_parity_scaled_linear_light(tmp_path: Path) -> None:
    """Acceptance test 2: 1.01 in linear light gives p99 dE_ITP between 0.1 and 1.0."""
    nits = pq_image()
    for name, scale in (("a", 1.0), ("b", 1.01)):
        code = np.round(imgdiff.pq_oetf(nits * scale) * 65535.0).astype(np.uint16)
        write_png16(tmp_path / f"{name}.png", code)
    res = run("--mode", "hdr-parity", tmp_path / "a.png", tmp_path / "b.png", "--json")
    out = json.loads(res.stdout)
    assert 0.1 < out["p99_delta_e_itp"] < 1.0, out
    assert res.returncode == 0
    # A tight limit fails with exit 1 and writes the diff image.
    res = run(
        "--mode",
        "hdr-parity",
        tmp_path / "a.png",
        tmp_path / "b.png",
        "--p99-max",
        "0.01",
        "--diff",
        tmp_path / "d.png",
    )
    assert res.returncode == 1 and (tmp_path / "d.png").exists()


def test_hdr_parity_identical_is_zero(tmp_path: Path) -> None:
    code = np.round(imgdiff.pq_oetf(pq_image()) * 65535.0).astype(np.uint16)
    write_png16(tmp_path / "a.png", code)
    out = json.loads(run("--mode", "hdr-parity", tmp_path / "a.png", tmp_path / "a.png", "--json").stdout)
    assert out["max_delta_e_itp"] == 0.0


def test_hdr_parity_sdr_input(tmp_path: Path) -> None:
    """8-bit inputs are BT.1886 at 203 nits: white maps to 203 nits and differs from black by far more than 1."""
    imgdiff.write_png(tmp_path / "w.png", np.full((8, 8, 3), 255, dtype=np.uint8))
    imgdiff.write_png(tmp_path / "k.png", np.zeros((8, 8, 3), dtype=np.uint8))
    nits = imgdiff.to_linear_nits(imgdiff.read_png(tmp_path / "w.png")[0], 8, "bt2020")
    assert nits[0, 0, 0] == pytest.approx(203.0)
    assert run("--mode", "hdr-parity", tmp_path / "w.png", tmp_path / "k.png").returncode == 1
