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

Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import argparse
import random
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--target", required=True)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--check", action="store_true", help="compare only; exit 1 on a difference")
    args = ap.parse_args()
    if args.target not in SEED_GENERATORS:
        print(
            f"fuzz_seed.py: target '{args.target}' has no seed generator yet",
            file=sys.stderr,
        )
        return 2
    out = args.out or Path(__file__).resolve().parent.parent / "core/tests/fuzz/corpus" / args.target
    seeds = SEED_GENERATORS[args.target]()
    if args.check:
        have = {p.name: p.read_bytes() for p in out.glob("*") if p.is_file()} if out.is_dir() else {}
        if have != seeds:
            print(f"fuzz_seed.py: {out} is stale; rerun without --check", file=sys.stderr)
            return 1
        return 0
    out.mkdir(parents=True, exist_ok=True)
    for stale in out.glob("seed_*.bin"):
        stale.unlink()
    for name, data in seeds.items():
        (out / name).write_bytes(data)
    print(f"wrote {len(seeds)} seeds to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
