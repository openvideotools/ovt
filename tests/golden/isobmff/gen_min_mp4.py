#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Writes the structural MP4 fixture of WI-0.556 and its box-tree golden.

    python3 tests/golden/isobmff/gen_min_mp4.py [--check]

Output (next to this script): ovt_min_structural.mp4 (2048 bytes: ftyp, moov with one video track, mdat of
zeros) and ovt_min_structural.pybox.json, the box-tree golden. The golden is produced by this script's own
independent box walker, standing in for an oracle (MP4Box, mp4ff) until the oracle image generates goldens (WI-0.564).
The golden lists only the fields the oracle decodes; OVT may report more (chapter 14 §14.4.0 step 3).
With --check nothing is written; exit 1 if the committed files differ from what the script generates.
Deterministic: stdlib only, no timestamps.
"""

import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONTAINERS = {"moov", "trak", "mdia", "minf", "stbl", "dinf", "edts", "mvex", "moof", "traf"}
TARGET_SIZE = 2048


def box(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I4s", 8 + len(payload), kind) + payload


def full(kind: bytes, version: int, flags: int, payload: bytes) -> bytes:
    return box(kind, struct.pack(">I", (version << 24) | flags) + payload)


def build() -> bytes:
    ftyp = box(b"ftyp", b"isom" + struct.pack(">I", 512) + b"isomiso2mp41")
    mvhd = full(b"mvhd", 0, 0, struct.pack(">IIII", 0, 0, 1000, 4000) + bytes(80))
    tkhd = full(b"tkhd", 0, 3, struct.pack(">IIII", 0, 0, 1, 0) + bytes(68))
    mdhd = full(b"mdhd", 0, 0, struct.pack(">IIII", 0, 0, 90000, 360000) + bytes(4))
    hdlr = full(b"hdlr", 0, 0, struct.pack(">I4s", 0, b"vide") + bytes(12) + b"VideoHandler\0")
    stsz = full(b"stsz", 0, 0, struct.pack(">IIIIII", 0, 4, 100, 200, 300, 400))
    stbl = box(b"stbl", stsz)
    minf = box(b"minf", stbl)
    mdia = box(b"mdia", mdhd + hdlr + minf)
    trak = box(b"trak", tkhd + mdia)
    moov = box(b"moov", mvhd + trak)
    head = ftyp + moov
    mdat = box(b"mdat", bytes(TARGET_SIZE - len(head) - 8))
    data = head + mdat
    assert len(data) == TARGET_SIZE
    return data


def decode(data: bytes, begin: int, end: int, parent: str, out: list) -> None:
    pos = begin
    siblings = []
    scan = begin
    while scan + 8 <= end:
        size, kind = struct.unpack_from(">I4s", data, scan)
        siblings.append(kind.decode("latin-1"))
        scan += size
    seen: dict = {}
    while pos + 8 <= end:
        size, kind_b = struct.unpack_from(">I4s", data, pos)
        kind = kind_b.decode("latin-1")
        path = f"{parent}/{kind}" if parent else kind
        if siblings.count(kind) > 1:
            seen[kind] = seen.get(kind, 0) + 1
            path += f"[{seen[kind]}]"
        payload = pos + 8
        body = payload + 4
        fields = {}
        if kind == "ftyp":
            fields = {
                "major_brand": data[payload : payload + 4].decode(),
                "minor_version": struct.unpack_from(">I", data, payload + 4)[0],
            }
        elif kind in ("mvhd", "mdhd"):
            fields = dict(zip(("timescale", "duration"), struct.unpack_from(">II", data, body + 8)))
        elif kind == "tkhd":
            fields = {"track_id": struct.unpack_from(">I", data, body + 8)[0]}
        elif kind == "hdlr":
            fields = {"handler_type": data[body + 4 : body + 8].decode()}
        elif kind == "stsz":
            fields = dict(zip(("sample_size", "sample_count"), struct.unpack_from(">II", data, body)))
        out.append({"path": path, "offset": pos, "size": size, "fields": fields})
        if kind in CONTAINERS:
            decode(data, payload, pos + size, path, out)
        pos += size


def golden_text(data: bytes) -> str:
    rows: list = []
    decode(data, 0, len(data), "", rows)
    return json.dumps(rows, indent=2, sort_keys=True) + "\n"


def main() -> int:
    data = build()
    text = golden_text(data)
    mp4 = HERE / "ovt_min_structural.mp4"
    golden = HERE / "ovt_min_structural.pybox.json"
    if "--check" in sys.argv:
        ok = mp4.read_bytes() == data and golden.read_text() == text
        print("up to date" if ok else "OUT OF DATE: run gen_min_mp4.py", file=sys.stderr)
        return 0 if ok else 1
    mp4.write_bytes(data)
    golden.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
