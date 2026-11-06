# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of the §14.3.3 fixture mutators of WI-0.553 on hand-built inputs (no encoder needed).

Each mutator was also checked against real x264 / x265 / libaom / FFmpeg output when it was written (decode
with FFmpeg, ``trace_headers`` of the rewritten fields); these tests pin the byte-level behaviour.
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import av1_insert_hdr_obus  # noqa: E402
import av1_set_level  # noqa: E402
import bitstream as bs  # noqa: E402
import corrupt  # noqa: E402
import make_multi_pps  # noqa: E402
import ts_inject_errors  # noqa: E402
import y4m_poke  # noqa: E402

# --- bitstream ------------------------------------------------------------------------------------------------


def test_annexb_split_join_round_trip() -> None:
    data = b"\x00\x00\x00\x01\x67\xaa\x00\x00\x01\x68\xbb\x00\x00\x00\x00\x01\x65\x00\x00\x03\x01"
    nals = bs.split_annexb(data)
    assert [n.data[:1] for n in nals] == [b"\x67", b"\x68", b"\x65"]
    assert bs.join_annexb(nals) == data
    assert bs.unescape(nals[2].data) == b"\x65\x00\x00\x01"
    assert bs.escape(b"\x65\x00\x00\x01") == nals[2].data


def test_exp_golomb_round_trip() -> None:
    w = bs.BitWriter()
    for v in (0, 1, 2, 7, 300):
        w.ue(v)
    for v in (0, 1, -1, 5, -9):
        w.se(v)
    data = bs.with_trailing_bits("".join(w.bits))
    r = bs.BitReader(data)
    assert [r.ue() for _ in range(5)] == [0, 1, 2, 7, 300]
    assert [r.se() for _ in range(5)] == [0, 1, -1, 5, -9]
    assert r.pos == bs.payload_bits(data)


# --- y4m_poke -------------------------------------------------------------------------------------------------


def y4m(colour: str, frames: int, bps: int) -> bytes:
    w, h = 8, 4
    size = (w * h + 2 * (w // 2) * (h // 2)) * bps
    body = b"".join(b"FRAME\n" + bytes((f + i) % 200 for i in range(size)) for f in range(frames))
    return f"YUV4MPEG2 W{w} H{h} F30:1 Ip A1:1 C{colour}\n".encode() + body


def test_y4m_poke_8bit(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    src, dst = tmp_path / "a.y4m", tmp_path / "b.y4m"
    src.write_bytes(y4m("420jpeg", 3, 1))
    assert (
        y4m_poke.main(["--frame", "2", "--plane", "1", "--x", "3", "--y", "1", "--delta", "+1", str(src), str(dst)])
        == 0
    )
    record = json.loads(capsys.readouterr().out)
    a, b = src.read_bytes(), dst.read_bytes()
    diff = [i for i in range(len(a)) if a[i] != b[i]]
    assert len(diff) == 1 and b[diff[0]] == a[diff[0]] + 1
    frame2 = a.index(b"FRAME", a.index(b"FRAME", a.index(b"FRAME") + 1) + 1) + 6
    assert diff[0] == frame2 + 8 * 4 + 1 * 4 + 3
    assert record == {"frame": 2, "plane": 1, "x": 3, "y": 1, "a": a[diff[0]], "b": a[diff[0]] + 1}


def test_y4m_poke_16bit_and_range(tmp_path: Path) -> None:
    src = tmp_path / "a.y4m"
    src.write_bytes(y4m("420p10", 1, 2))
    out, a, b = y4m_poke.poke(src.read_bytes(), 0, 0, 1, 0, 5)
    assert b == a + 5 and len(out) == src.stat().st_size
    with pytest.raises(ValueError):
        y4m_poke.poke(src.read_bytes(), 0, 0, 1, 0, -a - 1)
    with pytest.raises(ValueError):
        y4m_poke.poke(src.read_bytes(), 0, 0, 8, 0, 1)
    assert (
        y4m_poke.main(["--frame", "4", "--plane", "0", "--x", "0", "--y", "0", "--delta", "1", str(src), str(src)]) == 2
    )


def test_y4m_poke_mono_depths() -> None:
    assert y4m_poke.plane_geometry("YUV4MPEG2 W8 H4 Cmono") == ([(8, 4)], 1, 255)
    assert y4m_poke.plane_geometry("YUV4MPEG2 W8 H4 Cmono10") == ([(8, 4)], 2, 1023)
    assert y4m_poke.plane_geometry("YUV4MPEG2 W8 H4 Cmono16") == ([(8, 4)], 2, 65535)
    assert y4m_poke.plane_geometry("YUV4MPEG2 W8 H4 C420paldv")[1:] == (1, 255)


# --- AV1 ------------------------------------------------------------------------------------------------------

TAIL = "1011001110001111"  # stand-in for the rest of the sequence header


def seq_header(level: int) -> bytes:
    w = bs.BitWriter()
    w.u(3, 0)  # seq_profile
    w.u(2, 0)  # still_picture, reduced_still_picture_header
    w.u(1, 0)  # timing_info_present_flag (so no decoder_model_info_present_flag)
    w.u(1, 0)  # initial_display_delay_present_flag
    w.u(5, 0)  # operating_points_cnt_minus_1
    w.u(12, 0)  # operating_point_idc[0]
    w.u(5, level)
    if level > 7:
        w.u(1, 1)  # seq_tier[0] = High
    w.raw(TAIL)
    return bs.with_trailing_bits("".join(w.bits))


def ivf(*units: list[bs.Obu]) -> bytes:
    header = b"DKIF" + struct.pack("<HHIHHIIII", 0, 32, 0x31305641, 64, 64, 30, 1, len(units), 0)
    return bs.write_ivf(header, [(i, b"".join(o.encode() for o in tu)) for i, tu in enumerate(units)])


def tu(level: int = 0) -> list[bs.Obu]:
    return [bs.Obu(b"\x12", b""), bs.Obu(b"\x0a", seq_header(level)), bs.Obu(b"\x32", b"\x10\x20\x30")]


def test_av1_set_level_rewrites_level_and_tier() -> None:
    src = ivf(tu(13), tu(13)[2:])
    out, count = av1_set_level.rewrite(src, 0)
    assert count == 1
    _, frames = bs.read_ivf(out)
    obus = bs.split_obus(frames[0][1])
    assert obus[1].payload == seq_header(0)  # level 0 and the tier bit dropped, the rest unchanged
    assert obus[2].payload == b"\x10\x20\x30" and frames[1][1] == bs.read_ivf(src)[1][1][1]
    back, _ = av1_set_level.rewrite(out, 13)
    payload = bs.split_obus(bs.read_ivf(back)[1][0][1])[1].payload
    r = bs.BitReader(payload, 24)
    assert (r.u(5), r.u(1)) == (13, 0)  # the tier bit comes back as Main
    assert r.bits(r.pos, bs.payload_bits(payload)) == TAIL


def test_av1_insert_hdr_obus(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    src, dst = tmp_path / "a.ivf", tmp_path / "b.ivf"
    src.write_bytes(ivf(tu(), tu()[2:]))
    mdcv = "G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)"
    assert av1_insert_hdr_obus.main(["--mdcv", mdcv, "--cll", "1000,400", str(src), str(dst)]) == 0
    record = json.loads(capsys.readouterr().out)
    assert record["mdcv"]["primaries"][0] == [44564, 20972]  # red first: 0.68 * 65536, 0.32 * 65536
    assert record["mdcv"]["luminance_max"] == 1000 * 256 and record["mdcv"]["luminance_min"] == 2
    _, frames = bs.read_ivf(dst.read_bytes())
    obus = bs.split_obus(frames[0][1])
    assert [o.type for o in obus] == [2, 1, 5, 5, 6]
    assert obus[2].payload[0] == 2 and len(obus[2].payload) == 1 + 24 + 1
    assert obus[3].payload == b"\x01" + struct.pack(">2H", 1000, 400) + b"\x80"
    assert [o.type for o in bs.split_obus(frames[1][1])] == [6]
    assert av1_insert_hdr_obus.main(["--mdcv", "G(1,2)", str(src), str(dst)]) == 2


# --- MPEG-TS --------------------------------------------------------------------------------------------------


def ts_packet(pid: int, cc: int, payload: bytes, pusi: bool = False, pcr: int | None = None) -> bytes:
    head = bytes([0x47, (0x40 if pusi else 0) | pid >> 8, pid & 0xFF])
    if pcr is None:
        return head + bytes([0x10 | cc]) + payload.ljust(184, b"\xff")
    base, ext = divmod(pcr, 300)
    field = bytes([0x10]) + struct.pack(">IH", base >> 1, (base & 1) << 15 | 0x7E00 | ext)
    af = bytes([len(field)]) + field
    return head + bytes([0x30 | cc]) + af + payload.ljust(184 - len(af), b"\xff")


def section_packet(pid: int, body: bytes) -> bytes:
    return ts_packet(pid, 0, b"\x00" + body + b"\x00" * 4, pusi=True)  # CRC not checked by the tool


def make_ts(seconds: int = 4) -> bytes:
    pat = bytes([0x00, 0xB0, 13, 0, 1, 0xC1, 0, 0, 0, 1, 0xE1, 0x00])  # program 1 -> PMT PID 0x100
    pmt = bytes([0x02, 0xB0, 18, 0, 1, 0xC1, 0, 0, 0xE1, 0x01, 0xF0, 0, 0x1B, 0xE1, 0x01, 0xF0, 0])
    out = [section_packet(0, pat), section_packet(0x100, pmt)]
    cc = 0
    for i in range(seconds * 100):
        pcr = i * 270_000 if i % 10 == 0 else None  # 0.1 s apart
        out.append(ts_packet(0x101, cc, b"\x00\x00\x01\xe0" if i % 25 == 0 else b"", pusi=i % 25 == 0, pcr=pcr))
        cc = (cc + 1) & 15
    return b"".join(out)


def test_ts_video_pid_skips_the_nit_entry() -> None:
    pat = bytes([0x00, 0xB0, 17, 0, 1, 0xC1, 0, 0, 0, 0, 0xE0, 0x10, 0, 1, 0xE1, 0x00])  # program 0 = NIT PID 0x10
    pmt = bytes([0x02, 0xB0, 18, 0, 1, 0xC1, 0, 0, 0xE1, 0x01, 0xF0, 0, 0x1B, 0xE1, 0x01, 0xF0, 0])
    data = section_packet(0, pat) + section_packet(0x100, pmt) + make_ts()[2 * 188 :]
    assert ts_inject_errors.video_pid(ts_inject_errors.packets(data)) == 0x101


def test_ts_stripped_pcr_packets_are_not_dropped() -> None:
    _, report = ts_inject_errors.inject(make_ts(), 100, 1)
    assert report["pcr_removed"] and not set(report["dropped"]) & set(report["pcr_removed"])


def test_ts_inject_errors() -> None:
    out, report = ts_inject_errors.inject(make_ts(), 7, 1)
    assert report["video_pid"] == 0x101 and len(report["dropped"]) == 7
    assert all(b - a > 1 for a, b in zip(report["dropped"], report["dropped"][1:]))
    pkts = ts_inject_errors.packets(out)
    assert len(pkts) == len(make_ts()) // 188 - 7
    ccs = [p[3] & 15 for p in pkts if ts_inject_errors.pid(p) == 0x101]
    assert sum((b - a) & 15 != 1 for a, b in zip(ccs, ccs[1:])) == 7
    pcrs = [v for p in pkts if (v := ts_inject_errors.pcr(p)) is not None]
    gaps = [(b - a) / 27e6 for a, b in zip(pcrs, pcrs[1:])]
    assert max(gaps) > 1 and sorted(gaps)[-2] == pytest.approx(0.1)
    assert report["pcr_removed"] and all(len(p) == 188 for p in pkts)
    with pytest.raises(ValueError):
        ts_inject_errors.inject(make_ts(1), 0, 1)


# --- H.264 / H.265 Annex B ------------------------------------------------------------------------------------


def nal(header: bytes, bits: str) -> bs.Nal:
    return bs.Nal(b"\x00\x00\x00\x01", bs.escape(header + bs.with_trailing_bits(bits)))


def pps(pps_id: int = 0) -> bs.Nal:
    w = bs.BitWriter()
    w.ue(pps_id)
    w.ue(0)  # sps id
    w.u(7, 0)  # dependent_slice .. cabac_init_present_flag
    w.ue(0)
    w.ue(0)
    w.se(0)  # init_qp_minus26
    w.raw("110101")  # rest of the PPS
    return nal(b"\x44\x01", "".join(w.bits))


def slice_(irap: bool, first: bool = True) -> bs.Nal:
    w = bs.BitWriter()
    w.u(1, int(first))
    if irap:
        w.u(1, 0)
    w.ue(0)  # slice_pic_parameter_set_id
    w.ue(1 if irap else 0)  # slice_type
    w.raw("0110100")  # rest of the header ends with byte_alignment(): pad to a byte
    bits = "".join(w.bits)
    bits += "1" + "0" * (-(len(bits) + 1) % 8)
    header = bytes([0x26 if irap else 0x02, 0x01]) + int(bits, 2).to_bytes(len(bits) // 8, "big")
    return bs.Nal(b"\x00\x00\x01", header + b"\xc3\x5a\x01\x80")  # + stand-in slice data


def sei_hash() -> bs.Nal:
    return bs.Nal(b"\x00\x00\x01", b"\x50\x01" + bytes([132, 49, 0]) + bytes(range(16, 64)) + b"\x80")


def h265_stream(pictures: int = 6) -> bytes:
    nals = [nal(b"\x40\x01", "1010"), nal(b"\x42\x01", "110011"), pps()]
    for i in range(pictures):
        nals += [slice_(i == 0), sei_hash()]
    return bs.join_annexb(nals)


def run_corrupt(tmp_path: Path, data: bytes, *args: str, suffix: str = ".265") -> tuple[int, bytes]:
    src, dst = tmp_path / f"in{suffix}", tmp_path / f"out{suffix}"
    src.write_bytes(data)
    rc = corrupt.main([*args, str(src), str(dst)])
    return rc, dst.read_bytes() if dst.exists() else b""


def test_corrupt_truncate_and_drop(tmp_path: Path) -> None:
    data = h265_stream()
    rc, out = run_corrupt(tmp_path, data, "--truncate-nal", "sps", "--keep-bytes", "2")
    assert rc == 0 and bs.split_annexb(out)[1].data == bs.split_annexb(data)[1].data[:2]
    rc, out = run_corrupt(tmp_path, data, "--drop-nal", "pps")
    assert rc == 0 and 34 not in [bs.h265_type(n.data) for n in bs.split_annexb(out)]
    assert run_corrupt(tmp_path, data, "--drop-nal", "aud")[0] == 2
    h264 = bs.join_annexb([nal(b"\x67", "1100"), nal(b"\x68", "11"), nal(b"\x65", "1000")])
    rc, out = run_corrupt(tmp_path, h264, "--drop-nal", "pps", suffix=".264")
    assert rc == 0 and [bs.h264_type(n.data) for n in bs.split_annexb(out)] == [7, 5]


def test_corrupt_flip_slice_and_hash(tmp_path: Path) -> None:
    data = h265_stream()
    rc, out = run_corrupt(tmp_path, data, "--flip-slice-data", "--frame", "3", "--byte", "4")
    before, after = bs.split_annexb(data), bs.split_annexb(out)
    changed = [i for i, (a, b) in enumerate(zip(before, after)) if a.data != b.data]
    assert rc == 0 and changed == [9] and after[9].data[4] == before[9].data[4] ^ 0xFF
    rc, out = run_corrupt(tmp_path, data, "--flip-hash-sei", "--frame", "4")
    after = bs.split_annexb(out)
    changed = [i for i, (a, b) in enumerate(zip(before, after)) if a.data != b.data]
    assert rc == 0 and changed == [12]  # the suffix SEI that follows picture 4's slice
    assert after[12].data[5] == before[12].data[5] ^ 0x01  # first MD5 byte, after type, size, hash_type
    assert run_corrupt(tmp_path, data, "--flip-hash-sei", "--frame", "9")[0] == 2
    rc, out = run_corrupt(tmp_path, data, "--byte-xor", "5")
    assert rc == 0 and out[5] == data[5] ^ 0xFF and out[:5] == data[:5]


def test_make_multi_pps_keeps_alignment_and_data() -> None:
    data = h265_stream(6)
    out, per_picture = make_multi_pps.multi_pps(data, [0, 1, 2], {3})
    assert per_picture == [0, 1, 2, 0, 1, 2]
    nals = bs.split_annexb(out)
    types = [bs.h265_type(n.data) for n in nals]
    assert types.count(34) == 6 and types[2:5] == [34, 34, 34]
    assert nals[2].data == pps(0).data  # the first PPS is unchanged
    assert len({n.data[2:] for n in nals[2:5]}) == 3
    slices_in = [n for n in bs.split_annexb(data) if bs.h265_type(n.data) < 32]
    slices_out = [n for n in nals if bs.h265_type(n.data) < 32]
    for i, (a, b) in enumerate(zip(slices_in, slices_out)):
        assert b.data[-4:] == a.data[-4:]  # the byte-aligned slice data is untouched
        r = bs.BitReader(bs.unescape(b.data), 16 + 1 + (1 if i == 0 else 0))
        assert r.ue() == per_picture[i]
    for k, p in enumerate(nals[2:5]):
        r = bs.BitReader(bs.unescape(p.data), 16)
        assert r.ue() == k
    with pytest.raises(ValueError):
        make_multi_pps.multi_pps(bs.join_annexb([pps(), slice_(True, first=False)]), [0, 1], set())
