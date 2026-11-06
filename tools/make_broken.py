#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Write deliberately broken ISOBMFF files (§14.3.3 recipes 34-37, 72; chapter 8 §8.9.3; WI-0.553).

    make_broken.py MODE -o out.mp4

Every file starts from a pure-Python base MP4 of about 20 KiB: one 320x240 video track of 30 samples of
filler data (the sample bytes are not a decodable picture; these files test the container reader only), with
a real H.264 (or H.265) parameter set in its sample entry. Modes, one per file:

* ``--stco-beyond-eof``: the last ``stco`` offset points past the end of the file (``CNT-2007``);
* ``--stsc-descending``: ``stsc`` ``first_chunk`` values that decrease (``CNT-2003``);
* ``--ctts-v0-negative``: a version-0 ``ctts`` whose offsets have the sign bit set (``CNT-2002``);
* ``--box-size 7 --in moov``: a 7-byte box header inside ``moov`` before ``trak`` (``CNT-0106``);
* ``--hvc1-strip-ps``: an ``hvc1`` sample entry whose ``hvcC`` has no VPS/SPS/PPS arrays (``CNT-7005``);
* ``--cmfc-two-tracks``: a fragmented file branded ``cmfc`` with two video tracks (``CNT-7010``);
* ``--msix-no-sidx``: a fragmented file branded ``msix`` without a ``sidx`` (``CNT-7031``).

WI-1.201 extends this script with the rest of the §8.9.3 broken corpus. Exit 0 ok, 2 usage.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

from bitstream import unescape

SAMPLES, SAMPLE_SIZE, W, H = 30, 640, 320, 240
AVC_SPS = bytes.fromhex("6764000dacd94141fb0110000003001000000303c8f1429960")
AVC_PPS = bytes.fromhex("68ebe3cb22c0")
HEVC_PS = [
    bytes.fromhex("40010c01ffff03700000030090000003000003001e959809"),
    bytes.fromhex("42010103700000030090000003000003001ea020810596566924caf01010000003001000000301e080"),
    bytes.fromhex("4401c172b42240"),
]


def box(kind: bytes, *body: bytes) -> bytes:
    payload = b"".join(body)
    return struct.pack(">I", 8 + len(payload)) + kind + payload


def full(kind: bytes, version: int, flags: int, *body: bytes) -> bytes:
    return box(kind, struct.pack(">I", version << 24 | flags), *body)


def sample_entry(hevc: bool, strip_ps: bool) -> bytes:
    if hevc:
        arrays = (
            []
            if strip_ps
            else [bytes([0x80 | (32 + i)]) + struct.pack(">HH", 1, len(n)) + n for i, n in enumerate(HEVC_PS)]
        )
        config = box(
            b"hvcC",
            bytes([1]) + unescape(HEVC_PS[1])[3:15],
            bytes.fromhex("f000fcfdf8f800000f"),
            bytes([len(arrays)]),
            *arrays,
        )
    else:
        config = box(
            b"avcC",
            bytes([1, 0x64, 0x00, 0x0D, 0xFF, 0xE1]),
            struct.pack(">H", len(AVC_SPS)),
            AVC_SPS,
            bytes([1]),
            struct.pack(">H", len(AVC_PPS)),
            AVC_PPS,
        )
    visual = bytes(6) + struct.pack(">H", 1) + bytes(16) + struct.pack(">HHIIIH", W, H, 0x480000, 0x480000, 0, 1)
    visual += bytes(32) + struct.pack(">Hh", 0x18, -1)
    return box(b"hvc1" if hevc else b"avc1", visual, config)


def trak(track_id: int, stbl: list[bytes], hevc: bool = False, strip_ps: bool = False) -> bytes:
    tkhd = full(
        b"tkhd",
        0,
        3,
        struct.pack(">IIIII", 0, 0, track_id, 0, SAMPLES * 1000 // 30),  # movie timescale 1000: 1 s
        bytes(8),
        struct.pack(">hhhH", 0, 0, 0, 0),
        struct.pack(">9I", 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000),
        struct.pack(">II", W << 16, H << 16),
    )
    mdhd = full(b"mdhd", 0, 0, struct.pack(">IIIIHH", 0, 0, 30000, SAMPLES * 1000, 0x55C4, 0))
    hdlr = full(b"hdlr", 0, 0, b"\0\0\0\0vide", bytes(12), b"VideoHandler\0")
    dinf = box(b"dinf", full(b"dref", 0, 0, struct.pack(">I", 1), full(b"url ", 0, 1)))
    stsd = full(b"stsd", 0, 0, struct.pack(">I", 1), sample_entry(hevc, strip_ps))
    minf = box(b"minf", full(b"vmhd", 0, 1, bytes(8)), dinf, box(b"stbl", stsd, *stbl))
    return box(b"trak", tkhd, box(b"mdia", mdhd, hdlr, minf))


def mvhd(next_id: int) -> bytes:
    return full(
        b"mvhd",
        0,
        0,
        struct.pack(">IIIIIH", 0, 0, 1000, SAMPLES * 1000 // 30, 0x10000, 0x0100),
        bytes(10),
        struct.pack(">9I", 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000),
        bytes(24),
        struct.pack(">I", next_id),
    )


def progressive(mode: str) -> bytes:
    """The base file with one mutation applied to its sample table or moov."""
    ftyp = box(b"ftyp", b"isom", struct.pack(">I", 0x200), b"isomiso2avc1mp41")
    chunks = [10, 10, 10]  # samples per chunk
    stts = full(b"stts", 0, 0, struct.pack(">III", 1, SAMPLES, 1000))
    stsz = full(b"stsz", 0, 0, struct.pack(">II", SAMPLE_SIZE, SAMPLES))
    if mode == "stsc-descending":
        entries = [struct.pack(">III", c, 10, 1) for c in (1, 3, 2)]  # first_chunk 1, then 3 > 2: not increasing
        stsc = full(b"stsc", 0, 0, struct.pack(">I", 3), *entries)
    else:
        stsc = full(b"stsc", 0, 0, struct.pack(">I", 1), struct.pack(">III", 1, 10, 1))
    extra = []
    if mode == "ctts-v0-negative":
        extra = [
            full(
                b"ctts", 0, 0, struct.pack(">I", SAMPLES), *(struct.pack(">II", 1, 0xFFFFFC18) for _ in range(SAMPLES))
            )
        ]

    def moov(offsets: list[int]) -> bytes:
        stco = full(b"stco", 0, 0, struct.pack(">I", len(offsets)), *(struct.pack(">I", o) for o in offsets))
        body = [mvhd(2)]
        if mode == "box-size-7":
            body.append(struct.pack(">I", 7) + b"free")  # size 7 < 8: an impossible box header
        body.append(trak(1, [stts, *extra, stsc, stsz, stco], hevc=mode == "hvc1-strip-ps", strip_ps=True))
        return box(b"moov", *body)

    data_start = len(ftyp) + len(moov([0] * len(chunks))) + 8
    offsets = [data_start + sum(chunks[:i]) * SAMPLE_SIZE for i in range(len(chunks))]
    mdat = box(b"mdat", bytes((i * 7) & 0xFF for i in range(SAMPLES * SAMPLE_SIZE)))
    if mode == "stco-beyond-eof":
        offsets[-1] = data_start + len(mdat) + 4096
    return ftyp + moov(offsets) + mdat


def fragmented(brand: bytes, tracks: int) -> bytes:
    ftyp = box(b"ftyp", brand, struct.pack(">I", 0), b"iso6" + brand)
    empty = [full(k, 0, 0, struct.pack(">I", 0)) for k in (b"stts", b"stsc")] + [
        full(b"stsz", 0, 0, bytes(8)),
        full(b"stco", 0, 0, bytes(4)),
    ]
    trex = [full(b"trex", 0, 0, struct.pack(">IIIII", t, 1, 1000, 0, 0)) for t in range(1, tracks + 1)]
    moov = box(b"moov", mvhd(tracks + 1), *(trak(t, empty) for t in range(1, tracks + 1)), box(b"mvex", *trex))

    def moof(offset: int) -> bytes:
        trafs = []
        for t in range(1, tracks + 1):
            tfhd = full(b"tfhd", 0, 0x020000, struct.pack(">I", t))  # default-base-is-moof
            tfdt = full(b"tfdt", 1, 0, struct.pack(">Q", 0))
            data_offset = offset + (t - 1) * SAMPLES * SAMPLE_SIZE // tracks
            n = SAMPLES // tracks
            trun = full(
                b"trun",
                0,
                0x000301,
                struct.pack(">Ii", n, data_offset),
                *(struct.pack(">II", 1000, SAMPLE_SIZE) for _ in range(n)),
            )
            trafs.append(box(b"traf", tfhd, tfdt, trun))
        return box(b"moof", full(b"mfhd", 0, 0, struct.pack(">I", 1)), *trafs)

    first = moof(0)
    mdat = box(b"mdat", bytes((i * 11) & 0xFF for i in range(SAMPLES * SAMPLE_SIZE)))
    return ftyp + moov + moof(len(first) + 8) + mdat


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    mode = ap.add_mutually_exclusive_group(required=True)
    for flag in (
        "stco-beyond-eof",
        "stsc-descending",
        "ctts-v0-negative",
        "hvc1-strip-ps",
        "cmfc-two-tracks",
        "msix-no-sidx",
    ):
        mode.add_argument(f"--{flag}", action="store_true")
    mode.add_argument("--box-size", type=int, choices=(7,))
    ap.add_argument("--in", dest="parent", choices=("moov",), help="parent of the --box-size box")
    ap.add_argument("-o", "--output", type=Path, required=True)
    args = ap.parse_args(argv)
    if args.box_size and args.parent != "moov":
        ap.error("--box-size 7 needs --in moov")
    if args.cmfc_two_tracks:
        data = fragmented(b"cmfc", 2)
    elif args.msix_no_sidx:
        data = fragmented(b"msix", 1)
    elif args.box_size:
        data = progressive("box-size-7")
    else:
        flag = next(
            f for f in ("stco_beyond_eof", "stsc_descending", "ctts_v0_negative", "hvc1_strip_ps") if getattr(args, f)
        )
        data = progressive(flag.replace("_", "-"))
    args.output.write_bytes(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
