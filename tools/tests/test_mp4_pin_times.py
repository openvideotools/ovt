# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""tools/mp4_pin_times.py: zero the mvhd/tkhd/mdhd times Bento4 takes from the clock (WI-0.553)."""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import mp4_pin_times  # noqa: E402


def box(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I4s", 8 + len(payload), kind) + payload


def full(kind: bytes, version: int, times: tuple[int, int], rest: bytes) -> bytes:
    fmt = ">QQ" if version == 1 else ">II"
    return box(kind, bytes([version, 0, 0, 0]) + struct.pack(fmt, *times) + rest)


def sample(created: int, modified: int) -> bytes:
    mvhd = full(b"mvhd", 0, (created, modified), struct.pack(">II", 1000, 2000) + bytes(80))
    tkhd = full(b"tkhd", 0, (created, modified), struct.pack(">II", 1, 0) + bytes(68))
    mdhd = full(b"mdhd", 1, (created, modified), struct.pack(">IQ", 30, 60) + bytes(4))
    trak = box(b"trak", tkhd + box(b"mdia", mdhd + box(b"hdlr", bytes(24))))
    # moov with a 64-bit largesize header, as some muxers write
    moov_payload = mvhd + trak
    moov = struct.pack(">I4sQ", 1, b"moov", 16 + len(moov_payload)) + moov_payload
    return box(b"ftyp", b"isom" + bytes(4)) + moov + box(b"moof", bytes(16)) + box(b"mdat", b"\x01\x02\x03")


def test_two_runs_a_second_apart_become_identical(tmp_path):
    a, b = sample(3_845_000_000, 3_845_000_000), sample(3_845_000_001, 3_845_000_001)
    assert a != b and len(a) == len(b)
    outs = []
    for name, data in (("a", a), ("b", b)):
        src, dst = tmp_path / f"{name}.mp4", tmp_path / f"{name}.out.mp4"
        src.write_bytes(data)
        assert mp4_pin_times.main([str(src), str(dst)]) == 0
        outs.append(dst.read_bytes())
    assert outs[0] == outs[1] == sample(0, 0)


def test_only_the_time_fields_change_and_versions_are_reported(tmp_path, capsys):
    data = bytearray(sample(7, 9))
    before = bytes(data)
    report = mp4_pin_times.pin_times(data)
    assert [(r["box"], r["version"]) for r in report] == [("mvhd", 0), ("tkhd", 0), ("mdhd", 1)]
    changed = [i for i in range(len(data)) if data[i] != before[i]]
    assert len(changed) == 6  # 7 and 9 sit in the low byte of each of the three pairs
    src = tmp_path / "in.mp4"
    src.write_bytes(before)
    assert mp4_pin_times.main([str(src), str(tmp_path / "out.mp4")]) == 0
    assert json.loads(capsys.readouterr().out) == report


def test_bad_input_exits_2(tmp_path):
    no_moov = tmp_path / "no_moov.mp4"
    no_moov.write_bytes(box(b"ftyp", b"isom" + bytes(4)))
    bad_size = tmp_path / "bad.mp4"
    bad_size.write_bytes(struct.pack(">I4s", 999, b"moov"))
    for path in (no_moov, bad_size):
        assert mp4_pin_times.main([str(path), str(tmp_path / "o.mp4")]) == 2
        assert not (tmp_path / "o.mp4").exists()
