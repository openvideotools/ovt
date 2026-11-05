#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Regenerate the committed seed corpus of a fuzz target (chapter 14 §14.7.2; WI-0.557).

    fuzz_seed.py --target bitreader [--out core/tests/fuzz/corpus/bitreader] [--check]

`bitreader` has no media input: its eight seeds are opcode tapes (the layout is in
core/tests/fuzz/fuzz_bitreader.cpp) built from a fixed pseudo-random generator, so the output is
byte-identical on every run. With --check nothing is written; exit 1 if the directory differs.
The corpus-extracting targets (syntax, container, ...) arrive with the work items that own them
(WI-1.238 and the syntax WIs) and exit 2 here until then.

Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path

OPCODES = 28  # fuzz_bitreader.cpp kOpcodes
SEEDS = 8
MAX_SEED_BYTES = (
    4096  # keeps the 60 s smoke run reaching the parsers (WI-0.557 pitfalls)
)


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
        "string": [18, 12, 17, 25],  # st byte_align b8 L
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
        tape = bytes(
            rng.choice(pool) | (rng.randrange(8) << 5)
            for _ in range(rng.randrange(24, 64))
        )
        payload = bytearray(rng.randrange(256) for _ in range(rng.randrange(48, 160)))
        if fam == "string":
            payload[rng.randrange(8, 24)] = 0  # a terminator for st()
        out[f"seed_{i}_{fam}.bin"] = bytes([len(tape)]) + tape + bytes(payload)
    assert all(len(v) <= MAX_SEED_BYTES for v in out.values())
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--target", required=True)
    ap.add_argument("--out", type=Path)
    ap.add_argument(
        "--check", action="store_true", help="compare only; exit 1 on a difference"
    )
    args = ap.parse_args()
    if args.target != "bitreader":
        print(
            f"fuzz_seed.py: target '{args.target}' has no seed generator yet",
            file=sys.stderr,
        )
        return 2
    out = (
        args.out
        or Path(__file__).resolve().parent.parent / "core/tests/fuzz/corpus/bitreader"
    )
    seeds = bitreader_seeds()
    if args.check:
        have = (
            {p.name: p.read_bytes() for p in out.glob("*") if p.is_file()}
            if out.is_dir()
            else {}
        )
        if have != seeds:
            print(
                f"fuzz_seed.py: {out} is stale; rerun without --check", file=sys.stderr
            )
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
