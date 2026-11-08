# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/extract_samples.py (WI-1.231) on a hand-built MP4."""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import extract_samples  # noqa: E402


def box(btype: bytes, payload: bytes) -> bytes:
    return struct.pack(">I4s", 8 + len(payload), btype) + payload


def full(btype: bytes, payload: bytes, version: int = 0) -> bytes:
    return box(btype, bytes([version, 0, 0, 0]) + payload)


def avc1_entry(length_size: int) -> bytes:
    visual = bytes(6) + struct.pack(">H", 1) + bytes(70)  # SampleEntry + VisualSampleEntry fields
    avcc = bytes([1, 0x42, 0, 0x1E, 0xFC | (length_size - 1), 0xE0, 0])
    return box(b"avc1", visual + box(b"avcC", avcc))


def build_mp4(samples: list[bytes], length_size: int = 4, chunks: list[int] | None = None, track_id: int = 1) -> bytes:
    """moov before mdat; `chunks` gives the samples per chunk (default: one chunk)."""
    chunks = chunks or [len(samples)]
    ftyp = box(b"ftyp", b"isom" + struct.pack(">I", 512) + b"isomavc1")
    stsd = full(b"stsd", struct.pack(">I", 1) + avc1_entry(length_size))
    stsz = full(b"stsz", struct.pack(">II", 0, len(samples)) + b"".join(struct.pack(">I", len(s)) for s in samples))
    stsc = full(b"stsc", struct.pack(">I", len(chunks)) +
                b"".join(struct.pack(">III", i + 1, n, 1) for i, n in enumerate(chunks)))

    def moov_with(offsets: list[int]) -> bytes:
        stco = full(b"stco", struct.pack(">I", len(offsets)) + b"".join(struct.pack(">I", o) for o in offsets))
        stbl = box(b"stbl", stsd + stsz + stsc + stco)
        tkhd = full(b"tkhd", bytes(8) + struct.pack(">I", track_id) + bytes(68))
        trak = box(b"trak", tkhd + box(b"mdia", box(b"minf", stbl)))
        return box(b"moov", trak)

    placeholder = moov_with([0] * len(chunks))
    data_start = len(ftyp) + len(placeholder) + 8
    offsets, pos, k = [], data_start, 0
    for n in chunks:
        offsets.append(pos)
        pos += sum(len(s) for s in samples[k : k + n])
        k += n
    return ftyp + moov_with(offsets) + box(b"mdat", b"".join(samples))


def length_prefixed(nals: list[bytes], length_size: int = 4) -> bytes:
    return b"".join(len(n).to_bytes(length_size, "big") + n for n in nals)


def test_extracts_samples_and_counts_nals(tmp_path: Path) -> None:
    samples = [length_prefixed([b"\x67\x42", b"\x68\xce", b"\x65\x88\x84"]), length_prefixed([b"\x41\x9a\x00"])]
    mp4 = tmp_path / "a.mp4"
    mp4.write_bytes(build_mp4(samples, chunks=[1, 1]))
    out = tmp_path / "out"
    assert extract_samples.main(["--track", "1", str(mp4), "-o", str(out)]) == 0
    summary = json.loads((out / "samples.json").read_text())
    assert summary["sample_entry"] == "avc1"
    assert summary["nal_length_size"] == 4
    assert [s["nal_count"] for s in summary["samples"]] == [3, 1]
    assert [s["size"] for s in summary["samples"]] == [len(s) for s in samples]
    for rec, sample in zip(summary["samples"], samples):
        assert (out / f"sample_{rec['index']}.bin").read_bytes() == sample
        assert mp4.read_bytes()[rec["offset"] : rec["offset"] + rec["size"]] == sample


def test_selection_and_two_byte_lengths(tmp_path: Path) -> None:
    samples = [length_prefixed([bytes([0x41, i])], 2) for i in range(5)]
    mp4 = tmp_path / "b.mp4"
    mp4.write_bytes(build_mp4(samples, length_size=2, chunks=[2, 3]))
    out = tmp_path / "out"
    assert extract_samples.main(["--track", "1", "--samples", "1,3-4", str(mp4), "-o", str(out)]) == 0
    summary = json.loads((out / "samples.json").read_text())
    assert [s["index"] for s in summary["samples"]] == [1, 3, 4]
    assert all(s["nal_length_size"] == 2 and s["nal_count"] == 1 for s in summary["samples"])
    assert (out / "sample_3.bin").read_bytes() == samples[3]


def test_errors_and_missing_corpus(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    mp4 = tmp_path / "c.mp4"
    mp4.write_bytes(build_mp4([length_prefixed([b"\x65"])]))
    assert extract_samples.main(["--track", "2", str(mp4), "-o", str(tmp_path / "o")]) == 2  # no such track
    assert extract_samples.main(["--track", "1", "--samples", "5", str(mp4), "-o", str(tmp_path / "o")]) == 2
    monkeypatch.setenv("OVT_CORPORA_DIR", str(tmp_path / "corpora"))
    assert extract_samples.main(["--track", "1", "--corpus-id", "nope", "-o", str(tmp_path / "o")]) == 77


def test_corpus_lookup_by_ok_marker(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    entry = tmp_path / "corpora" / "syn_x"
    entry.mkdir(parents=True)
    (entry / "x.mp4").write_bytes(build_mp4([length_prefixed([b"\x65\x01"])]))
    (entry / ".ok").write_text(json.dumps({"primary": "x.mp4"}))
    monkeypatch.setenv("OVT_CORPORA_DIR", str(tmp_path / "corpora"))
    out = tmp_path / "out"
    assert extract_samples.main(["--track", "1", "--corpus-id", "syn_x", "-o", str(out)]) == 0
    assert json.loads((out / "samples.json").read_text())["samples"][0]["nal_count"] == 1
