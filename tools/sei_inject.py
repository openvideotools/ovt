#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Insert or extract H.264 / H.265 SEI messages (§14.3.3 recipe 47, non-stream fixtures; WI-0.553).

    sei_inject.py --codec h264|h265 --type T --size N --seed S in out
    sei_inject.py --codec h264|h265 --payload NAME [--payloads FILE] in out
    sei_inject.py --extract t35 --frames 0,30,59 --out-dir DIR in

The first two forms add one SEI NAL unit (H.264 type 6, H.265 prefix SEI 39) carrying one ``sei_message`` right
before the first VCL unit, i.e. after the parameter sets and SEI of the first access unit. Its payload is either
N pseudo-random bytes from seed S (SHA-256 counter stream, identical on every platform) or the hand-encoded
bytes of entry NAME of ``tests/corpora/sei_payloads.json`` (``{"payloads": {NAME: {"codec", "type", "hex"}}}``).
The third form writes the payload of the first ``user_data_registered_itu_t_t35`` SEI (type 4) of each listed
picture (decode order) to ``DIR/fNNN.bin``. Exit 0 ok, 2 bad input.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

from bitstream import Nal, escape, h264_type, h265_type, join_annexb, split_annexb, unescape

REPO = Path(__file__).resolve().parents[1]
PAYLOADS = REPO / "tests" / "corpora" / "sei_payloads.json"
T35 = 4


def seeded_bytes(seed: int, size: int) -> bytes:
    out = b""
    counter = 0
    while len(out) < size:
        out += hashlib.sha256(f"{seed}:{counter}".encode()).digest()
        counter += 1
    return out[:size]


def ff_coded(value: int) -> bytes:
    return b"\xff" * (value // 255) + bytes([value % 255])


def sei_nal(codec: str, ptype: int, payload: bytes) -> Nal:
    header = b"\x06" if codec == "h264" else b"\x4e\x01"
    rbsp = header + ff_coded(ptype) + ff_coded(len(payload)) + payload + b"\x80"
    return Nal(b"\x00\x00\x00\x01", escape(rbsp))


def is_vcl(codec: str, nal: Nal) -> bool:
    return 1 <= h264_type(nal.data) <= 5 if codec == "h264" else h265_type(nal.data) < 32


def insert(data: bytes, codec: str, ptype: int, payload: bytes) -> bytes:
    nals = split_annexb(data)
    first = next((i for i, n in enumerate(nals) if is_vcl(codec, n)), None)
    if first is None:
        raise ValueError("no VCL NAL unit")
    return join_annexb(nals[:first] + [sei_nal(codec, ptype, payload)] + nals[first:])


def sei_messages(rbsp: bytes, header_len: int) -> list[tuple[int, bytes]]:
    out, pos = [], header_len
    while pos < len(rbsp) and rbsp[pos:] != b"\x80":
        values = []
        for _ in range(2):
            v = 0
            while rbsp[pos] == 0xFF:
                v, pos = v + 255, pos + 1
            values.append(v + rbsp[pos])
            pos += 1
        out.append((values[0], rbsp[pos : pos + values[1]]))
        pos += values[1]
    return out


def extract_t35(data: bytes, frames: list[int]) -> dict[int, bytes]:
    """Payload of the first T.35 SEI of each wanted picture (H.265: prefix SEI before the picture's slice)."""
    found: dict[int, bytes] = {}
    picture = 0
    pending: bytes | None = None
    for nal in split_annexb(data):
        t = h265_type(nal.data)
        if t == 39 and pending is None:
            pending = next((p for k, p in sei_messages(unescape(nal.data), 2) if k == T35), None)
        elif t < 32 and unescape(nal.data[:3])[2] & 0x80:  # first slice segment of a picture
            if picture in frames and pending is not None:
                found[picture] = pending
            picture, pending = picture + 1, None
    return found


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--codec", choices=("h264", "h265"))
    ap.add_argument("--type", type=int)
    ap.add_argument("--size", type=int)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--payload", help="entry of --payloads")
    ap.add_argument("--payloads", type=Path, default=PAYLOADS)
    ap.add_argument("--extract", choices=("t35",))
    ap.add_argument("--frames", default="0")
    ap.add_argument("--out-dir", type=Path)
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path, nargs="?")
    args = ap.parse_args(argv)
    try:
        data = args.input.read_bytes()
        if args.extract:
            if args.out_dir is None:
                ap.error("--extract needs --out-dir")
            frames = [int(v) for v in args.frames.split(",")]
            found = extract_t35(data, frames)
            missing = [f for f in frames if f not in found]
            if missing:
                raise ValueError(f"no T.35 SEI for pictures {missing}")
            args.out_dir.mkdir(parents=True, exist_ok=True)
            for f in frames:
                (args.out_dir / f"f{f:03d}.bin").write_bytes(found[f])
            print(json.dumps({f"f{f:03d}.bin": len(found[f]) for f in frames}))
            return 0
        if args.output is None:
            ap.error("give the output file")
        if args.payload:
            entry = json.loads(args.payloads.read_text(encoding="utf-8"))["payloads"][args.payload]
            codec, ptype, payload = entry["codec"], entry["type"], bytes.fromhex(entry["hex"])
        elif args.codec and args.type is not None and args.size is not None:
            codec, ptype, payload = args.codec, args.type, seeded_bytes(args.seed, args.size)
        else:
            ap.error("give --payload, or --codec, --type and --size")
        args.output.write_bytes(insert(data, codec, ptype, payload))
    except (ValueError, KeyError, IndexError, OSError) as e:
        print(f"sei_inject: {e}", file=sys.stderr)
        return 2
    print(json.dumps({"codec": codec, "type": ptype, "size": len(payload)}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
