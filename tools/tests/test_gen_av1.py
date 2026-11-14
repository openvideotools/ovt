# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/goldens/gen_av1.py and av1_selfcheck.py (chapter 14 §14.4.5, WI-0.566)."""

from __future__ import annotations

import struct
import sys
from pathlib import Path
from typing import Any

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "goldens"))

import av1_selfcheck as sc  # noqa: E402
import gen_av1 as gen  # noqa: E402


def ivf(payloads: list[bytes]) -> bytes:
    head = b"DKIF" + struct.pack("<HH4sHHIIII", 0, 32, b"AV01", 16, 16, 30, 1, len(payloads), 0)
    return head + b"".join(struct.pack("<IQ", len(p), n) + p for n, p in enumerate(payloads))


def test_ivf_frames_and_prefix() -> None:
    data = ivf([b"aaaa", b"bb", b"cccccc"])
    frames = gen.ivf_frames(data)
    assert [e - s for s, e in frames] == [16, 14, 18]
    cut = gen.ivf_prefix(data, frames, 2)
    assert cut == ivf([b"aaaa", b"bb"])  # the frame count in the header is patched too
    assert gen.ivf_frames(cut) == frames[:2]


def test_ivf_frames_ignores_a_truncated_last_record() -> None:
    data = ivf([b"aaaa", b"bbbb"])[:-1]
    assert len(gen.ivf_frames(data)) == 1


def test_ivf_frames_refuses_other_files() -> None:
    with pytest.raises(gen.StepError):
        gen.ivf_frames(b"\x1a\x45\xdf\xa3" + bytes(40))


MANIFEST: dict[str, Any] = {
    "entries": [
        {"id": "aom_fast", "source": "aom", "tags": ["fast"]},
        {"id": "aom_slow", "source": "aom", "tags": ["slow"]},
        {"id": "aom_unverified", "source": "aom", "tags": ["fast"], "unverified": True},
        {"id": "syn_av1_a", "source": "syn"},
        {"id": "syn_av1_long", "source": "syn", "unverified": True, "local_only": True},
        {"id": "syn_h264_a", "source": "syn"},
    ]
}


def test_select() -> None:
    chosen, skipped = gen.select(MANIFEST, None)
    assert [e["id"] for e in chosen] == ["aom_fast", "syn_av1_a"]
    assert set(skipped) == {"aom_unverified", "syn_av1_long"}
    only, _ = gen.select(MANIFEST, ["aom_slow"])
    assert [e["id"] for e in only] == ["aom_slow"]
    with pytest.raises(gen.StepError):
        gen.select(MANIFEST, ["nope"])


def frame(n: int, shown: int, bits: int = 5) -> dict[str, Any]:
    return {"frame": n, "header": {"show_frame": shown}, "symbols": [{"x4": 0, "y4": 0, "name": "s", "bits_x8": bits}]}


def test_parse_summary() -> None:
    assert sc.parse_summary("junk\n5 decoded frames/4 showed frames in 12 us (1 fps)\n") == (5, 4)
    with pytest.raises(ValueError):
        sc.parse_summary("nothing")


def test_selfcheck_accepts_hidden_frame_and_show_existing() -> None:
    # TU0: key frame; TU1: hidden ARF + shown frame; TU2: show_existing_frame of the ARF (no inspect object)
    doc = {"frames": [frame(0, 1), frame(1, 0), frame(2, 1)]}
    rep = sc.check(doc, 3, 3, 3)
    assert rep["problems"] == []
    assert (rep["hidden"], rep["show_existing"]) == (1, 1)


def test_selfcheck_finds_disagreements() -> None:
    doc = {"frames": [frame(0, 1), frame(1, 1)]}
    assert any("temporal units" in p for p in sc.check(doc, 2, 1, 2)["problems"])
    assert any("shows only" in p for p in sc.check(doc, 2, 2, 1)["problems"])
    empty = {"frames": [frame(0, 1, bits=0)]}
    assert any("no accounting symbol" in p for p in sc.check(empty, 1, 1, 1)["problems"])


def test_gz_bytes_matches_write_gz(tmp_path: Path) -> None:
    text = '{"frames": []}\n' * 50
    gen.common.write_gz(tmp_path / "a.gz", text)
    assert (tmp_path / "a.gz").read_bytes() == gen.gz_bytes(text)
