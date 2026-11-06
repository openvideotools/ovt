# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of sei_inject, make_mkv_lacing, gen_heif_items and make_broken (WI-0.553 part b2) on hand-built inputs.

The outputs were also checked with real tools when the scripts were written: FFmpeg ``trace_headers`` for every
``sei_payloads.json`` entry, FFmpeg decoding the laced WebM to the same PCM as its input, libheif decoding the
1 000-tile grid, and MP4Box/ffprobe on every ``make_broken`` mode.
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import bitstream as bs  # noqa: E402
import gen_heif_items  # noqa: E402
import make_broken  # noqa: E402
import make_mkv_lacing as mkl  # noqa: E402
import sei_inject  # noqa: E402

# --- helpers --------------------------------------------------------------------------------------------------


def boxes(data: bytes, start: int = 0, end: int | None = None) -> list[tuple[bytes, int, int]]:
    """(type, payload start, end) of the boxes in data[start:end]."""
    out, pos, end = [], start, len(data) if end is None else end
    while pos + 8 <= end:
        size, kind = struct.unpack_from(">I4s", data, pos)
        if size < 8:
            out.append((kind, pos + 8, pos + 8))
            break
        out.append((kind, pos + 8, pos + size))
        pos += size
    return out


def find(data: bytes, path: list[bytes], start: int = 0, end: int | None = None) -> tuple[int, int]:
    full_boxes = {b"meta": 4, b"stsd": 8, b"dref": 8, b"iref": 4, b"iinf": 6}
    for kind, a, b in boxes(data, start, end):
        if kind == path[0]:
            return (a, b) if len(path) == 1 else find(data, path[1:], a + full_boxes.get(kind, 0), b)
    raise KeyError(path[0])


H265 = bs.join_annexb(
    [
        bs.Nal(b"\x00\x00\x00\x01", bytes.fromhex("40010c01ffff03700000030090000003000003001e959809")),
        bs.Nal(b"\x00\x00\x00\x01", bytes.fromhex(make_broken.HEVC_PS[1].hex())),
        bs.Nal(b"\x00\x00\x00\x01", bytes.fromhex("4401c172b42240")),
        bs.Nal(b"\x00\x00\x01", b"\x26\x01\xaf\x10\x20\x80"),  # IDR, first_slice_segment_in_pic_flag = 1
        bs.Nal(b"\x00\x00\x01", b"\x02\x01\xd0\x30\x80"),  # TRAIL_R, first slice of picture 1
    ]
)

# --- sei_inject -----------------------------------------------------------------------------------------------


def test_sei_inject_seeded_payload(tmp_path: Path) -> None:
    src, dst = tmp_path / "a.265", tmp_path / "b.265"
    src.write_bytes(H265)
    assert sei_inject.main(["--codec", "h265", "--type", "200", "--size", "17", "--seed", "1", str(src), str(dst)]) == 0
    nals = bs.split_annexb(dst.read_bytes())
    assert [bs.h265_type(n.data) for n in nals] == [32, 33, 34, 39, 19, 1]
    [(ptype, payload)] = sei_inject.sei_messages(bs.unescape(nals[3].data), 2)
    assert ptype == 200 and payload == sei_inject.seeded_bytes(1, 17) and len(payload) == 17
    assert sei_inject.seeded_bytes(1, 17) != sei_inject.seeded_bytes(2, 17)


def test_sei_payload_table_and_t35_round_trip(tmp_path: Path) -> None:
    table = json.loads((REPO / "tests" / "corpora" / "sei_payloads.json").read_text(encoding="utf-8"))["payloads"]
    assert {"h264_ga94_cc", "h264_mastering_display_colour_volume", "h264_content_light_level_info"} <= set(table)
    for name, entry in table.items():
        assert name.startswith(entry["codec"] + "_") and bytes.fromhex(entry["hex"])
    ga94 = bytes.fromhex(table["h264_ga94_cc"]["hex"])
    assert ga94[:3] == b"\xb5\x00\x31" and ga94[3:7] == b"GA94"  # ATSC A/53 in ITU-T T.35
    t35 = sei_inject.insert(H265, "h265", 4, b"\xb5\x00\x3c\x00\x01\x04" + bytes(range(40)))
    found = sei_inject.extract_t35(t35, [0])
    assert found == {0: b"\xb5\x00\x3c\x00\x01\x04" + bytes(range(40))}
    out = tmp_path / "x"
    assert sei_inject.main(["--extract", "t35", "--frames", "0,1", "--out-dir", str(out), str(tmp_path / "nope")]) == 2
    (tmp_path / "t.265").write_bytes(t35)
    assert sei_inject.main(["--extract", "t35", "--frames", "0,1", "--out-dir", str(out), str(tmp_path / "t.265")]) == 2
    assert sei_inject.main(["--extract", "t35", "--frames", "0", "--out-dir", str(out), str(tmp_path / "t.265")]) == 0
    assert (out / "f000.bin").read_bytes() == found[0]


def test_sei_size_coding_over_255() -> None:
    nal = sei_inject.sei_nal("h264", 300, bytes(600))
    rbsp = bs.unescape(nal.data)
    assert rbsp[1:4] == b"\xff\x2d\xff" and sei_inject.sei_messages(rbsp, 1) == [(300, bytes(600))]


# --- make_mkv_lacing ------------------------------------------------------------------------------------------


def opus_webm(packets: list[bytes], discard: int | None = None) -> bytes:
    """A WebM in the shape FFmpeg writes: unlaced SimpleBlocks, the last one in a BlockGroup with discard."""
    track = {
        mkl.CODEC_ID: b"A_OPUS",
        mkl.CODEC_PRIVATE: b"OpusHead" + bytes(11),
        mkl.SAMPLING: struct.pack(">d", 48000),
    }
    track[mkl.CHANNELS] = b"\x01"
    entry = mkl.el(mkl.TRACK_ENTRY, *(mkl.el(k, v) for k, v in track.items()), mkl.uint(mkl.TRACK_NUMBER, 1))
    bodies = [b"\x81" + struct.pack(">hB", 20 * i, 0x80) + p for i, p in enumerate(packets)]
    blocks = [mkl.el(mkl.SIMPLE_BLOCK, body) for body in bodies]
    if discard is not None:
        blocks[-1] = mkl.el(mkl.BLOCK_GROUP, mkl.el(mkl.BLOCK, bodies[-1]), mkl.uint(mkl.DISCARD_PADDING, discard))
    return mkl.el(mkl.SEGMENT, mkl.el(mkl.TRACKS, entry), mkl.el(mkl.CLUSTER, mkl.uint(mkl.TIMECODE, 0), *blocks))


def laced_blocks(data: bytes) -> list[tuple[int, list[int]]]:
    """(element id, frame sizes) of every block of the written file."""
    out = []

    def walk(start: int, end: int) -> None:
        for eid, a, b in mkl.elements(data, start, end):
            if eid in mkl.MASTERS:
                walk(a, b)
            elif eid in (mkl.SIMPLE_BLOCK, mkl.BLOCK):
                flags = data[a + 3]
                laced = bool(flags & 0x06)
                count = data[a + 4] + 1 if laced else 1
                assert flags & 0x06 in (0x00, 0x02)  # unlaced or Xiph
                sizes, pos = [], a + 5
                for _ in range(count - 1):
                    n = 0
                    while data[pos] == 0xFF:
                        n, pos = n + 255, pos + 1
                    sizes.append(n + data[pos])
                    pos += 1
                start_data = pos if laced else a + 4
                sizes.append(b - start_data - sum(sizes))
                out.append((eid, sizes))

    walk(0, len(data))
    return out


def test_make_mkv_lacing_xiph(tmp_path: Path) -> None:
    packets = [bytes([0xFC]) + bytes([i]) * (100 + 97 * i) for i in range(10)]  # CELT 20 ms, sizes cross 255
    src, dst = tmp_path / "in.webm", tmp_path / "out.webm"
    src.write_bytes(opus_webm(packets, discard=13_500_000))
    assert mkl.main(["--lacing", "xiph", "--frames-per-block", "4", "--input", str(src), str(dst)]) == 0
    blocks = laced_blocks(dst.read_bytes())
    assert [len(s) for _, s in blocks] == [4, 4, 1, 1]  # 9 laced packets, the trimmed last one alone
    assert [eid for eid, _ in blocks] == [mkl.SIMPLE_BLOCK] * 3 + [mkl.BLOCK]
    assert [n for _, s in blocks for n in s] == [len(p) for p in packets]
    data = dst.read_bytes()
    assert data.startswith(bytes.fromhex("1a45dfa3")) and b"webm" in data[:64]
    assert mkl.main(["--lacing", "xiph", "--frames-per-block", "4", "--input", str(src), str(tmp_path / "o2")]) == 0
    assert (tmp_path / "o2").read_bytes() == data  # deterministic
    assert mkl.packet_ms(b"\x08") == 20 and mkl.packet_ms(b"\xf1") == 20  # SILK 20 ms x1, CELT 10 ms x2


# --- gen_heif_items -------------------------------------------------------------------------------------------


def test_gen_heif_items_structure(tmp_path: Path) -> None:
    src, dst = tmp_path / "t.265", tmp_path / "t.heic"
    src.write_bytes(H265)
    assert gen_heif_items.main(["--items", "12", "--tile", "64", "--hevc", str(src), "--out", str(dst)]) == 0
    data = dst.read_bytes()
    assert data[4:12] == b"ftypheic"
    a, b = find(data, [b"meta", b"pitm"])
    assert struct.unpack_from(">H", data, a + 4)[0] == 1
    a, b = find(data, [b"meta", b"iloc"])
    assert struct.unpack_from(">H", data, a + 6)[0] == 13
    entries = [struct.unpack_from(">HHHHII", data, a + 8 + 16 * k) for k in range(13)]
    grid = data[entries[0][4] : entries[0][4] + entries[0][5]]
    assert struct.unpack(">BBBBHH", grid) == (0, 0, 2, 3, 256, 192)  # 3 rows x 4 columns of 64 px
    sample = data[entries[1][4] : entries[1][4] + entries[1][5]]
    assert sample == struct.pack(">I", 6) + b"\x26\x01\xaf\x10\x20\x80"  # first picture only, length-prefixed
    assert all(data[e[4] : e[4] + e[5]] == sample for e in entries[1:])
    a, b = find(data, [b"meta", b"iref"])
    assert data[a + 4 : a + 8] == b"dimg" or data[a + 8 : a + 12] == b"dimg"
    a, b = find(data, [b"meta", b"iprp", b"ipco", b"hvcC"])
    assert data[a + 22] == 3 and data[a + 1 : a + 13] == bs.unescape(make_broken.HEVC_PS[1])[3:15]  # PTL from the SPS
    assert gen_heif_items.main(["--items", "1000", "--tile", "64", "--hevc", str(src), "--out", str(dst)]) == 0
    assert gen_heif_items.main(["--items", "1009", "--tile", "64", "--hevc", str(src), "--out", str(dst)]) == 2


# --- make_broken ----------------------------------------------------------------------------------------------

STBL = [b"moov", b"trak", b"mdia", b"minf", b"stbl"]


def broken(tmp_path: Path, *args: str) -> bytes:
    out = tmp_path / "b.mp4"
    assert make_broken.main([*args, "-o", str(out)]) == 0
    return out.read_bytes()


def test_make_broken_sample_table_modes(tmp_path: Path) -> None:
    base = make_broken.progressive("none")
    assert 19_000 < len(base) < 21_500
    a, b = find(base, [*STBL, b"stco"])
    offsets = struct.unpack_from(">3I", base, a + 8)
    assert offsets[-1] + 10 * make_broken.SAMPLE_SIZE == len(base)  # the base file is consistent
    data = broken(tmp_path, "--stco-beyond-eof")
    a, b = find(data, [*STBL, b"stco"])
    assert struct.unpack_from(">I", data, a + 16)[0] > len(data)
    data = broken(tmp_path, "--stsc-descending")
    a, b = find(data, [*STBL, b"stsc"])
    assert struct.unpack_from(">I", data, a + 8)[0] > struct.unpack_from(">I", data, a + 20)[0]
    data = broken(tmp_path, "--ctts-v0-negative")
    a, b = find(data, [*STBL, b"ctts"])
    assert data[a] == 0 and struct.unpack_from(">i", data, a + 12)[0] < 0


def test_make_broken_structure_modes(tmp_path: Path) -> None:
    data = broken(tmp_path, "--box-size", "7", "--in", "moov")
    a, b = find(data, [b"moov"])
    assert [k for k, _, _ in boxes(data, a, b)][:2] == [b"mvhd", b"free"]
    assert struct.unpack_from(">I", data, boxes(data, a, b)[1][1] - 8)[0] == 7
    data = broken(tmp_path, "--hvc1-strip-ps")
    a, b = find(data, [*STBL, b"stsd", b"hvc1"])
    hvcc = data.index(b"hvcC", a)
    assert data[hvcc + 4 + 22] == 0  # numOfArrays
    data = broken(tmp_path, "--cmfc-two-tracks")
    assert data[8:12] == b"cmfc" and [k for k, _, _ in boxes(data, *find(data, [b"moov"]))].count(b"trak") == 2
    data = broken(tmp_path, "--msix-no-sidx")
    assert data[8:12] == b"msix" and b"sidx" not in data and [k for k, _, _ in boxes(data)][2] == b"moof"
    with pytest.raises(SystemExit):
        make_broken.main(["--box-size", "7", "-o", str(tmp_path / "x")])
