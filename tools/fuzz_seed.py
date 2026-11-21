#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Regenerate the committed seed corpus of a fuzz target (chapter 14 §14.7.2; WI-0.557).

    fuzz_seed.py --target bitreader [--out core/tests/fuzz/corpus/bitreader] [--check]

`bitreader` has no media input: its eight seeds are opcode tapes (the layout is in
core/tests/fuzz/fuzz_bitreader.cpp) built from a fixed pseudo-random generator, so the output is
byte-identical on every run. With --check nothing is written; exit 1 if the directory differs.
The corpus-extracting targets (syntax, container, ...) arrive with the work items that own them
(WI-1.238 and the syntax WIs) and exit 2 here until then. `user_data` seeds are hand-built valid
messages (T.35 HDR10+/GA94/Dolby/Vivid, x264/x265 UUID payloads).

WI-1.129 (chapter 7 WI-1.129, chapter 14 §14.7.2) adds the stream-based targets:

    fuzz_seed.py --make-streams DIR                 encode the 30 seed streams of STREAMS with ffmpeg
    fuzz_seed.py --target h264_units --streams DIR  one seed per unit type and stream (also h265_units, av1_obus)
    fuzz_seed.py --target index_builder --streams DIR  framing selector + the first 64 KiB of every stream
    fuzz_seed.py --contexts --streams DIR           rewrite core/tests/fuzz/fuzz_unit_contexts.inc

The streams are encoded from FFmpeg's testsrc2 source by libx264, libx265, libaom, SVT-AV1 and rav1e, so every
byte is generated here (no third-party test material). The recipes switch on the header features the parsers
branch on (profiles, bit depths, chroma formats, interlace, HRD, SEI kinds, tiles, film grain, ...). Encoder
output differs between encoder versions, so --check applies to the generated targets only.

Run with `python -I`; the standard library only (and ffmpeg on PATH for --make-streams).
"""

from __future__ import annotations

import argparse
import os
import random
import re
import subprocess
import sys
from pathlib import Path

OPCODES = 27  # fuzz_bitreader.cpp kOpcodes
SEEDS = 8
MAX_SEED_BYTES = 4096  # keeps the 60 s smoke run reaching the parsers (WI-0.557 pitfalls)


def bitreader_seeds() -> dict[str, bytes]:
    rng = random.Random(557)
    # Opcodes grouped so that a seed concentrates on one descriptor family.
    families = {
        "expgolomb": [2, 3, 4, 13, 19],  # ue se te more_rbsp_data rbsp_trailing_bits
        "fixed": [0, 1, 12, 14, 22],  # u i byte_align peek span
        "av1": [
            5,
            6,
            7,
            8,
            9,
            16,
            20,
            21,
        ],  # leb128 uvlc su ns le delta_q trailing_bits byte_alignment
        "position": [10, 11, 15, 17, 23, 24],  # skip seek b8 remaining bytes
        "string": [18, 12, 17, 13],  # st byte_align b8 more_rbsp_data
    }
    out: dict[str, bytes] = {}
    names = [
        "expgolomb",
        "fixed",
        "av1",
        "position",
        "string",
        "mixed",
        "mixed",
        "mixed",
    ]
    for i, fam in enumerate(names):
        pool = families.get(fam, list(range(OPCODES)))
        tape = bytes(rng.choice(pool) | (rng.randrange(8) << 5) for _ in range(rng.randrange(24, 64)))
        payload = bytearray(rng.randrange(256) for _ in range(rng.randrange(48, 160)))
        if fam == "string":
            payload[rng.randrange(8, 24)] = 0  # a terminator for st()
        out[f"seed_{i}_{fam}.bin"] = bytes([len(tape)]) + tape + bytes(payload)
    assert all(len(v) <= MAX_SEED_BYTES for v in out.values())
    return out


def _bits(fields: list[tuple[int, int]]) -> bytes:
    acc = "".join(format(v, f"0{n}b") for v, n in fields)
    acc += "0" * (-len(acc) % 8)
    return bytes(int(acc[i : i + 8], 2) for i in range(0, len(acc), 8))


def user_data_seeds() -> dict[str, bytes]:
    """Hand-built valid messages for fuzz_user_data.cpp: byte 0 is the entry-point selector."""
    hdr10plus = _bits(
        [
            (4, 8),
            (1, 8),
            (1, 2),
            (4000, 27),
            (0, 1),
            (1000, 17),
            (2000, 17),
            (3000, 17),
            (1500, 17),
        ]
        + [
            (3, 4),
            (1, 7),
            (100, 17),
            (50, 7),
            (900, 17),
            (99, 7),
            (2800, 17),
            (25, 10),
            (0, 1),
        ]
        + [
            (1, 1),
            (0x123, 12),
            (0x456, 12),
            (2, 4),
            (0x155, 10),
            (0x2AA, 10),
            (1, 1),
            (33, 6),
        ]
    )
    ga94 = (
        bytes([0xB5, 0x00, 0x31])
        + b"GA94"
        + bytes([0x03, 0x42, 0xFF])
        + bytes([0xFC, 0x80, 0x80, 0xFF, 0xFA, 0x00, 0x00])
    )
    x264 = bytes.fromhex("DC45E9BDE6D948B7962CD820D923EEEF") + (
        b"x264 - core 164 - options: cabac=1 ref=3 deblock=1:0:0 wpp\x00"
    )
    x265 = bytes.fromhex("2CA2DE09B51747DBBB55A4FE7FC2FC4E") + b"x265 - options: wpp no-sao ctu=64\x00"
    return {
        "seed_0_hdr10plus_sei.bin": bytes([0, 0xB5, 0x00, 0x3C, 0x00, 0x01]) + hdr10plus,
        "seed_1_hdr10plus_av1.bin": bytes([1, 0xB5, 0x00, 0x3C, 0x00, 0x01]) + hdr10plus + b"\x80",
        "seed_2_ga94.bin": bytes([0]) + ga94,
        "seed_3_dolby_av1.bin": bytes([1, 0xB5, 0x00, 0x3B, 0x00, 0x00, 0x08, 0x00, 0x12, 0x34, 0x80]),
        "seed_4_vivid.bin": bytes([0, 0x26, 0x00, 0x04, 0x00, 0x05, 0x01, 0x02]),
        "seed_5_x264.bin": bytes([2]) + x264,
        "seed_6_x265.bin": bytes([2]) + x265,
        "seed_7_options_text.bin": bytes([3]) + b"x264 - options: a=1 b=2:3 flag c=",
        "seed_8_trailing.bin": bytes([4, 1, 2, 3, 0x80, 0, 0]),
    }


SEED_GENERATORS = {"bitreader": bitreader_seeds, "user_data": user_data_seeds}

# ---- WI-1.129: seed streams, per-unit seeds, index seeds and the canned contexts --------------------------------

STREAM_PREFIX_BYTES = 65536  # "first 64 KiB of 10 streams each" (WI-1.129)
MAX_PER_TYPE = 3  # units of one type kept per stream (§14.7.2 --max-per-type 3)
MAX_UNIT_BYTES = 4096  # a per-unit seed; larger units are dropped (the 60 s smoke run must reach the parsers)
VCL_SEED_BYTES = 256  # H.26x slice seeds keep their start only: the header parsers stop at slice_data()
CONTEXT_SLICE_BYTES = 64  # the H.265 context keeps only the start of its first slice segment (the header)
CONTEXT_FRAME_BYTES = 128  # the AV1 context keeps only the start of its first frame OBU payload (the header)

_X265 = "pools=1:frame-threads=1:log-level=error"
_MD = "G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)"
_PQ = ["-colorspace", "bt2020nc", "-color_trc", "smpte2084", "-color_primaries", "bt2020"]


def _src(size: str = "96x64", frames: int = 12, extra: str = "") -> list[str]:
    graph = f"testsrc2=size={size}:rate=25" + (f",{extra}" if extra else "")
    return ["-f", "lavfi", "-i", graph, "-frames:v", str(frames), "-an", "-threads", "1"]


def _x264(params: str, *opts: str) -> list[str]:
    return [*opts, "-c:v", "libx264", "-x264-params", f"threads=1:{params}"]


def _x265(params: str, *opts: str) -> list[str]:
    return [*opts, "-c:v", "libx265", "-x265-params", f"{_X265}:{params}"]


def _aom(*opts: str) -> list[str]:
    return ["-c:v", "libaom-av1", "-cpu-used", "8", "-row-mt", "0", *opts]


# (stream name, ffmpeg arguments between the input and the output). x264 gets a distinct sps-id per stream, so
# the H.264 contexts could share one ActiveSets; the targets still give every context its own.
STREAMS: list[tuple[str, list[str]]] = [
    (
        "h264_high_bpyramid",
        _src() + _x264("b-pyramid=normal:sps-id=0", "-profile:v", "high", "-bf", "3", "-refs", "3", "-weightp", "2"),
    ),
    ("h264_baseline", _src() + _x264("sps-id=1", "-profile:v", "baseline")),
    ("h264_mbaff", _src() + _x264("interlaced=1:tff=1:sps-id=2", "-flags", "+ildct")),
    ("h264_fake_interlaced", _src() + _x264("fake-interlaced=1:pic-struct=1:sps-id=3")),
    ("h264_high422_cqm", _src() + _x264("cqm=jvt:sps-id=4", "-pix_fmt", "yuv422p10le", "-profile:v", "high422")),
    ("h264_high444_lossless", _src() + _x264("sps-id=5", "-pix_fmt", "yuv444p", "-qp", "0")),
    (
        "h264_cbr_hrd_filler",
        _src()
        + _x264("nal-hrd=cbr:filler=1:force-cfr=1:sps-id=6", "-b:v", "100k", "-maxrate", "100k", "-bufsize", "100k"),
    ),
    ("h264_slices_aud_intra_refresh", _src() + _x264("slices=4:aud=1:intra-refresh=1:keyint=6:sps-id=7")),
    (
        "h264_cavlc_open_gop",
        _src() + _x264("open-gop=1:keyint=4:constrained-intra=1:no-8x8dct=1:no-deblock=1:cabac=0:sps-id=8"),
    ),
    (
        "h264_pq_cropped",
        _src("70x46")
        + _x264(
            f"mastering-display={_MD}:cll=1000,400:chromaloc=2:sps-id=9",
            "-pix_fmt",
            "yuv420p10le",
            *_PQ,
            "-color_range",
            "pc",
        ),
    ),
    ("h265_main_bframes", _src() + _x265("bframes=3:ref=3")),
    (
        "h265_main10_hdr10",
        _src()
        + _x265(
            f"hdr10=1:master-display={_MD}:max-cll=1000,400:repeat-headers=1:hdr10-opt=1",
            "-pix_fmt",
            "yuv420p10le",
            *_PQ,
        ),
    ),
    ("h265_444_tskip", _src() + _x265("ctu=16:min-cu-size=8:tskip=1:cu-lossless=1", "-pix_fmt", "yuv444p")),
    ("h265_wpp_slices", _src() + _x265("wpp=1:slices=2:ctu=16:sao=1:deblock=2,-2:signhide=0")),
    ("h265_hrd_aud", _src() + _x265("hrd=1:vbv-maxrate=100:vbv-bufsize=100:info=1:aud=1:hrd-concat=1", "-b:v", "100k")),
    ("h265_md5_temporal_layers", _src() + _x265("hash=1:ctu=32:temporal-layers=1:bframes=2:b-pyramid=1")),
    (
        "h265_scaling_list_amp",
        _src() + _x265("scaling-list=default:amp=1:rect=1:ctu=32:strong-intra-smoothing=0:constrained-intra=1"),
    ),
    (
        "h265_open_gop_cra_eos",
        _src() + _x265("open-gop=1:keyint=5:min-keyint=5:bframes=2:radl=0:idr-recovery-sei=1:hash=2:eos=1"),
    ),
    (
        "h265_weighted_fade",
        _src(extra="fade=in:0:12")
        + _x265("weightp=1:weightb=1:bframes=2:ref=4:hash=3:opt-qp-pps=1:opt-ref-list-length-pps=1"),
    ),
    (
        "h265_422_10_intra_cropped",
        _src("70x46") + _x265("keyint=1:ctu=16:single-sei=1:cbqpoffs=-2:crqpoffs=3", "-pix_fmt", "yuv422p10le"),
    ),
    ("av1_aom_420", _src() + _aom("-tiles", "1x1", "-crf", "40")),
    (
        "av1_aom_10bit_screen_deltaq",
        _src()
        + _aom(
            "-pix_fmt",
            "yuv420p10le",
            "-crf",
            "40",
            "-aq-mode",
            "1",
            "-aom-params",
            "tune-content=screen:deltaq-mode=1:enable-chroma-deltaq=1",
        ),
    ),
    (
        "av1_aom_444_restoration",
        _src() + _aom("-pix_fmt", "yuv444p", "-crf", "40", "-aom-params", "enable-restoration=1"),
    ),
    ("av1_aom_mono_sb128", _src() + _aom("-pix_fmt", "gray", "-crf", "40", "-aom-params", "sb-size=128")),
    ("av1_aom_film_grain", _src() + _aom("-crf", "40", "-denoise-noise-level", "20")),
    (
        "av1_aom_tiles_error_resilient",
        _src("256x128", 8)
        + _aom("-tiles", "2x2", "-error-resilient", "1", "-crf", "50", "-aom-params", "frame-parallel=1"),
    ),
    ("av1_aom_lossless", _src() + _aom("-crf", "0", "-aom-params", "lossless=1")),
    ("av1_svt", _src() + ["-c:v", "libsvtav1", "-crf", "50", "-preset", "12", "-svtav1-params", "lp=1"]),
    ("av1_rav1e", _src() + ["-c:v", "librav1e", "-qp", "200", "-speed", "10", "-rav1e-params", "threads=1"]),
    (
        "av1_aom_pq_no_order_hint",
        _src("70x46")
        + _aom(
            "-crf",
            "40",
            *_PQ,
            "-aom-params",
            "timing-info=constant:enable-order-hint=0:enable-cdef=0:reduced-tx-type-set=1",
        ),
    ),
]
_EXT = {"h264": ".264", "h265": ".265", "av1": ".obu"}


def _codec(name: str) -> str:
    return name.split("_", 1)[0]


def make_streams(out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, SVT_LOG="0")
    for name, args in STREAMS:
        dst = out / (name + _EXT[_codec(name)])
        fmt = ["-f", "obu"] if _codec(name) == "av1" else []
        cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *args, *fmt, str(dst)]
        subprocess.run(cmd, check=True, env=env, stdout=subprocess.DEVNULL)
        print(f"{dst.name}: {dst.stat().st_size} bytes")


def _annexb_units(data: bytes) -> list[bytes]:
    """NAL units (header + EBSP) of an Annex B stream; trailing zero bytes go with the next start code."""
    starts = [m.end() for m in re.finditer(b"\x00\x00\x01", data)]
    units = []
    for i, begin in enumerate(starts):
        end = starts[i + 1] - 3 if i + 1 < len(starts) else len(data)
        unit = data[begin:end]
        units.append(unit.rstrip(b"\x00") if i + 1 < len(starts) else unit)
    return [u for u in units if u]


def _leb128(data: bytes, pos: int) -> tuple[int, int]:
    value = 0
    for i in range(8):
        if pos >= len(data):
            raise ValueError("truncated leb128")
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7F) << (7 * i)
        if not byte & 0x80:
            break
    return value, pos


def _obus(data: bytes) -> list[tuple[bytes, int]]:
    """(whole OBU, header length incl. extension and obu_size) of a Section 5 stream; every OBU has obu_size."""
    out, pos = [], 0
    while pos < len(data):
        begin = pos
        pos += 2 if data[pos] & 0x04 else 1
        if not data[begin] & 0x02:
            raise ValueError("OBU without obu_size in a Section 5 stream")
        size, pos = _leb128(data, pos)
        out.append((data[begin : pos + size], pos - begin))
        pos += size
    return out


def _unit_type(codec: str, unit: bytes) -> int:
    if codec == "h264":
        return unit[0] & 0x1F
    if codec == "h265":
        return (unit[0] >> 1) & 0x3F
    return (unit[0] >> 3) & 0x0F


def _units(codec: str, data: bytes) -> list[bytes]:
    return [o for o, _ in _obus(data)] if codec == "av1" else _annexb_units(data)


def _streams_of(codec: str, streams: Path) -> list[tuple[str, bytes]]:
    out = []
    for name, _ in STREAMS:
        if _codec(name) == codec:
            path = streams / (name + _EXT[codec])
            if not path.is_file():
                raise FileNotFoundError(f"{path}: run fuzz_seed.py --make-streams {streams} first")
            out.append((name, path.read_bytes()[:STREAM_PREFIX_BYTES]))
    return out


CORPUS = Path(__file__).resolve().parent.parent / "core/tests/fuzz/corpus"


def harvested_units(codec: str) -> dict[str, bytes]:
    """Hand-made units of the older per-parser targets that the encoders never write (MVC / SVC subset SPS, prefix
    NAL, SPS extension, rare SEI payloads, AV1 metadata and tile lists), unwrapped from those targets' framing:
    h264_sei / h265_sei = one selector byte + the SEI NAL unit; h264_slice = SPS length, PPS length, SPS, PPS, unit;
    syntax_av1 = one whole OBU."""
    out: dict[str, bytes] = {}
    sources = {"h264": ["h264_sei", "h264_slice"], "h265": ["h265_sei"], "av1": ["syntax_av1"]}[codec]
    for target in sources:
        for path in sorted((CORPUS / target).glob("seed_*.bin")):
            data = path.read_bytes()
            if target.endswith("_sei"):
                unit = data[1:]
            elif target == "h264_slice":
                unit = data[2 + data[0] + data[1] :]
                if not unit or unit[0] & 0x1F not in (9, 12, 13, 14, 15, 20):
                    continue  # its slices need the seed's own SPS / PPS
            else:
                unit = data
            if unit:
                out[f"seed_s_h_{target}_{path.stem[len('seed_') :]}.bin"] = unit
    return out


def unit_seeds(codec: str, streams: Path) -> dict[str, bytes]:
    seeds: dict[str, bytes] = harvested_units(codec)
    seen: set[bytes] = set(seeds.values())
    for name, data in _streams_of(codec, streams):
        units = _units(codec, data) if codec != "av1" else [o for o, _ in _obus(_whole_obus(data))]
        per_type: dict[int, int] = {}
        for unit in units:
            t = _unit_type(codec, unit)
            if (codec == "h264" and t in (1, 2, 3, 4, 5, 20, 21)) or (codec == "h265" and t < 32):
                unit = unit[:VCL_SEED_BYTES]
            if len(unit) > MAX_UNIT_BYTES or unit in seen or per_type.get(t, 0) >= MAX_PER_TYPE:
                continue
            seen.add(unit)
            seeds[f"seed_s_{name.split('_', 1)[1]}_t{t}_{per_type.get(t, 0)}.bin"] = unit
            per_type[t] = per_type.get(t, 0) + 1
    return seeds


def _whole_obus(data: bytes) -> bytes:
    """The prefix of a Section 5 stream up to its last whole OBU (the 64 KiB cut may split one)."""
    pos = 0
    while pos < len(data):
        begin = pos
        try:
            pos += 2 if data[pos] & 0x04 else 1
            size, pos = _leb128(data, pos)
        except (ValueError, IndexError):
            return data[:begin]
        if pos + size > len(data):
            return data[:begin]
        pos += size
    return data


INDEX_SELECTOR = {"av1": 0, "h264": 2, "h265": 3}  # fuzz_index_builder.cpp byte 0


def index_seeds(streams: Path) -> dict[str, bytes]:
    seeds = {}
    for codec in ("h264", "h265", "av1"):
        for name, data in _streams_of(codec, streams):
            seeds[f"seed_s_{name}.bin"] = bytes([INDEX_SELECTOR[codec]]) + data
    return seeds


def _context(codec: str, data: bytes) -> bytes:
    """The canned context of one stream: H.264 first SPS + PPS; H.265 first VPS + SPS + PPS + the start of the
    first slice segment; AV1 the first sequence header OBU + the start of the first frame (header) OBU. H.26x
    units are joined with 4-byte start codes; the AV1 OBUs keep their headers (the last one is cut short and
    keeps its original obu_size, so the target reads it to the end of the buffer)."""
    if codec == "av1":
        seq = frame = None
        for obu, hlen in _obus(_whole_obus(data)):
            t = _unit_type(codec, obu)
            if t == 1 and seq is None:
                seq = obu
            elif t in (3, 6) and frame is None:
                frame = obu[: hlen + CONTEXT_FRAME_BYTES]
        assert seq is not None and frame is not None
        return seq + frame
    want = {"h264": [7, 8], "h265": [32, 33, 34, -1]}[codec]
    picked: dict[int, bytes] = {}
    for unit in _units(codec, data):
        t = _unit_type(codec, unit)
        key = -1 if codec == "h265" and t < 32 else t
        if key in want and key not in picked:
            picked[key] = unit[:CONTEXT_SLICE_BYTES] if key == -1 else unit
    assert len(picked) == len(want), sorted(picked)
    return b"".join(b"\x00\x00\x00\x01" + picked[k] for k in want)


CONTEXTS_INC = Path("core/tests/fuzz/fuzz_unit_contexts.inc")


def contexts_inc(streams: Path) -> str:
    lines = [
        "// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors",
        "// SPDX-License-Identifier: LGPL-2.1-or-later",
        "//",
        "// GENERATED by tools/fuzz_seed.py --contexts --streams DIR (WI-1.129); do not edit. The canned contexts of",
        "// fuzz_h264_units / fuzz_h265_units / fuzz_av1_obus: one per seed stream of tools/fuzz_seed.py STREAMS.",
        "",
    ]
    for codec in ("h264", "h265", "av1"):
        names = []
        for name, data in _streams_of(codec, streams):
            ident = "k_" + name
            names.append(ident)
            ctx = _context(codec, data)
            lines.append(f"// {name}")
            lines.append(f"inline constexpr unsigned char {ident}[] = {{")
            for i in range(0, len(ctx), 16):
                lines.append("    " + ", ".join(f"0x{b:02x}" for b in ctx[i : i + 16]) + ",")
            lines.append("};")
        lines.append(f"inline constexpr std::span<const unsigned char> k_{codec}_contexts[] = {{")
        lines.extend(f"    std::span<const unsigned char>({n})," for n in names)
        lines.append("};")
        lines.append("")
    return "\n".join(lines)


STREAM_GENERATORS = {
    "h264_units": lambda d: unit_seeds("h264", d),
    "h265_units": lambda d: unit_seeds("h265", d),
    "av1_obus": lambda d: unit_seeds("av1", d),
    "index_builder": index_seeds,
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--target")
    ap.add_argument("--out", type=Path)
    ap.add_argument("--check", action="store_true", help="compare only; exit 1 on a difference")
    ap.add_argument("--streams", type=Path, help="directory of the encoded seed streams (WI-1.129 targets)")
    ap.add_argument("--make-streams", type=Path, metavar="DIR", help="encode the seed streams into DIR")
    ap.add_argument("--contexts", action="store_true", help=f"rewrite {CONTEXTS_INC} from --streams")
    args = ap.parse_args()
    root = Path(__file__).resolve().parent.parent
    if args.make_streams:
        make_streams(args.make_streams)
        return 0
    if args.contexts:
        if not args.streams:
            ap.error("--contexts needs --streams")
        text = contexts_inc(args.streams)
        dst = args.out or root / CONTEXTS_INC
        if args.check:
            return 0 if dst.is_file() and dst.read_text() == text else 1
        dst.write_text(text)
        print(f"wrote {dst}")
        return 0
    if not args.target:
        ap.error("--target, --contexts or --make-streams is required")
    # The stream-derived seeds are named seed_s_*; the hand-made seeds of the same target (index_builder) are kept.
    if args.target in STREAM_GENERATORS:
        if not args.streams:
            ap.error(f"--target {args.target} needs --streams (see --make-streams)")
        seeds = STREAM_GENERATORS[args.target](args.streams)
        pattern = "seed_s_*"
    elif args.target in SEED_GENERATORS:
        seeds = SEED_GENERATORS[args.target]()
        pattern = "*"
    else:
        print(
            f"fuzz_seed.py: target '{args.target}' has no seed generator yet",
            file=sys.stderr,
        )
        return 2
    out = args.out or root / "core/tests/fuzz/corpus" / args.target
    if args.check:
        have = {p.name: p.read_bytes() for p in out.glob(pattern) if p.is_file()} if out.is_dir() else {}
        if have != seeds:
            print(f"fuzz_seed.py: {out} is stale; rerun without --check", file=sys.stderr)
            return 1
        return 0
    out.mkdir(parents=True, exist_ok=True)
    for stale in out.glob("seed_s_*.bin" if pattern == "seed_s_*" else "seed_*.bin"):
        stale.unlink()
    for name, data in seeds.items():
        (out / name).write_bytes(data)
    print(f"wrote {len(seeds)} seeds to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
