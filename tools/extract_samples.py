#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Extract container samples of an MP4 track (chapter 08 WI-1.231; D27).

    extract_samples.py --track N [--samples SEL] FILE.mp4 -o DIR
    extract_samples.py --track N [--samples SEL] --corpus-id ID -o DIR

Writes each selected sample's raw bytes as ``DIR/sample_<index>.bin`` and ``DIR/samples.json``:

    {"file": "...", "track": N, "sample_entry": "avc1", "nal_length_size": 4,
     "samples": [{"index": 0, "offset": 48, "size": 2876, "nal_length_size": 4, "nal_count": 5}, ...]}

``index`` is 0-based (OVT's ``sample_index``). ``nal_length_size`` is ``lengthSizeMinusOne + 1`` from ``avcC`` /
``hvcC`` (0 for other sample entries); ``nal_count`` counts the length-prefixed NAL units of a sample with a
plain Python walk, written independently of the C++ splitter so the unit-splitter test has an oracle. ``SEL`` is a
comma-separated list of indices and ``a-b`` ranges (default: all samples).

The sample table is read from ``stsz``/``stz2``, ``stsc`` and ``stco``/``co64`` of the ``trak`` whose ``tkhd``
has ``track_ID == N``. Fragmented files (samples in ``moof``) are rejected.
SPEC-ASSUMPTION(#338): WI-1.231 names ``MP4Box -dts``/``-dnal`` as the source of the offsets and NAL counts;
MP4Box is not installed on the CI runners, so this tool reads the sample table itself.

``--corpus-id`` resolves the file like the C++ test support (``$OVT_CORPORA_DIR``, else ``--corpora-dir``, a
verified ``<id>/.ok``); a missing corpus exits 77 so that CTest reports a skip. Exit 0 ok, 2 bad input.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

SKIP = 77
CONTAINERS = {b"moov", b"trak", b"mdia", b"minf", b"stbl"}
LENGTH_PREFIXED_ENTRIES = {b"avc1": b"avcC", b"avc3": b"avcC", b"hvc1": b"hvcC", b"hev1": b"hvcC"}
VISUAL_SAMPLE_ENTRY_HEADER = 8 + 70  # SampleEntry (8) + VisualSampleEntry fields (70), after the box header


class InputError(Exception):
    pass


@dataclass
class Box:
    type: bytes
    start: int  # offset of the box header
    payload: int  # offset of the payload
    end: int


def boxes(data: bytes, start: int, end: int) -> list[Box]:
    out: list[Box] = []
    pos = start
    while pos + 8 <= end:
        size, btype = struct.unpack_from(">I4s", data, pos)
        header = 8
        if size == 1:
            if pos + 16 > end:
                raise InputError(f"truncated largesize box at {pos}")
            size = struct.unpack_from(">Q", data, pos + 8)[0]
            header = 16
        elif size == 0:
            size = end - pos
        if size < header or pos + size > end:
            raise InputError(f"box {btype!r} at {pos} has bad size {size}")
        out.append(Box(btype, pos, pos + header, pos + size))
        pos += size
    return out


def child(data: bytes, parent: Box, btype: bytes) -> Box | None:
    return next((b for b in boxes(data, parent.payload, parent.end) if b.type == btype), None)


def full_box_payload(box: Box) -> int:
    return box.payload + 4  # version + flags


def track_id(data: bytes, trak: Box) -> int:
    tkhd = child(data, trak, b"tkhd")
    if tkhd is None:
        raise InputError("trak without tkhd")
    version = data[tkhd.payload]
    return struct.unpack_from(">I", data, full_box_payload(tkhd) + (16 if version == 1 else 8))[0]


def sample_sizes(data: bytes, stbl: Box) -> list[int]:
    stsz = child(data, stbl, b"stsz")
    if stsz is not None:
        p = full_box_payload(stsz)
        fixed, count = struct.unpack_from(">II", data, p)
        if fixed:
            return [fixed] * count
        return list(struct.unpack_from(f">{count}I", data, p + 8))
    stz2 = child(data, stbl, b"stz2")
    if stz2 is None:
        raise InputError("stbl without stsz/stz2")
    p = full_box_payload(stz2)
    field_size = data[p + 3]
    count = struct.unpack_from(">I", data, p + 4)[0]
    raw = data[p + 8 :]
    if field_size == 16:
        return list(struct.unpack_from(f">{count}H", raw))
    if field_size == 8:
        return list(raw[:count])
    if field_size == 4:
        return [(raw[i // 2] >> (4 if i % 2 == 0 else 0)) & 0xF for i in range(count)]
    raise InputError(f"stz2 field_size {field_size}")


def chunk_offsets(data: bytes, stbl: Box) -> list[int]:
    stco = child(data, stbl, b"stco")
    if stco is not None:
        p = full_box_payload(stco)
        count = struct.unpack_from(">I", data, p)[0]
        return list(struct.unpack_from(f">{count}I", data, p + 4))
    co64 = child(data, stbl, b"co64")
    if co64 is None:
        raise InputError("stbl without stco/co64")
    p = full_box_payload(co64)
    count = struct.unpack_from(">I", data, p)[0]
    return list(struct.unpack_from(f">{count}Q", data, p + 4))


def sample_offsets(data: bytes, stbl: Box, sizes: list[int]) -> list[int]:
    stsc = child(data, stbl, b"stsc")
    if stsc is None:
        raise InputError("stbl without stsc")
    p = full_box_payload(stsc)
    count = struct.unpack_from(">I", data, p)[0]
    runs = [struct.unpack_from(">III", data, p + 4 + 12 * i) for i in range(count)]
    chunks = chunk_offsets(data, stbl)
    offsets: list[int] = []
    sample = 0
    for r, (first_chunk, per_chunk, _desc) in enumerate(runs):
        last_chunk = runs[r + 1][0] - 1 if r + 1 < len(runs) else len(chunks)
        for chunk in range(first_chunk, last_chunk + 1):
            pos = chunks[chunk - 1]
            for _ in range(per_chunk):
                if sample >= len(sizes):
                    return offsets
                offsets.append(pos)
                pos += sizes[sample]
                sample += 1
    if len(offsets) != len(sizes):
        raise InputError(f"stsc/stco place {len(offsets)} of {len(sizes)} samples")
    return offsets


def sample_entry(data: bytes, stbl: Box) -> tuple[str, int]:
    """(sample entry four-cc, nal_length_size or 0)."""
    stsd = child(data, stbl, b"stsd")
    if stsd is None:
        raise InputError("stbl without stsd")
    entries = boxes(data, full_box_payload(stsd) + 4, stsd.end)
    if not entries:
        raise InputError("empty stsd")
    entry = entries[0]
    name = entry.type.decode("latin-1")
    config_type = LENGTH_PREFIXED_ENTRIES.get(entry.type)
    if config_type is None:
        return name, 0
    config = next(
        (b for b in boxes(data, entry.start + VISUAL_SAMPLE_ENTRY_HEADER + 8, entry.end) if b.type == config_type),
        None,
    )
    if config is None:
        raise InputError(f"{name} without {config_type.decode()}")
    field = config.payload + (4 if config_type == b"avcC" else 21)  # lengthSizeMinusOne
    return name, (data[field] & 0x3) + 1


def count_nals(sample: bytes, length_size: int) -> int:
    """NAL units of a length-prefixed sample (zero-length NAL units are not counted)."""
    count = 0
    pos = 0
    while pos + length_size <= len(sample):
        n = int.from_bytes(sample[pos : pos + length_size], "big")
        pos += length_size + n
        if n:
            count += 1
    return count


def parse_selection(sel: str | None, total: int) -> list[int]:
    if not sel:
        return list(range(total))
    picked: list[int] = []
    for part in sel.split(","):
        a, _, b = part.partition("-")
        lo, hi = int(a), int(b) if b else int(a)
        if lo < 0 or hi >= total or lo > hi:
            raise InputError(f"sample selection {part} outside 0..{total - 1}")
        picked.extend(range(lo, hi + 1))
    return picked


def extract(path: Path, track: int, selection: str | None, out_dir: Path) -> dict:
    data = path.read_bytes()
    top = boxes(data, 0, len(data))
    if any(b.type == b"moof" for b in top):
        raise InputError("fragmented MP4 (moof) is not supported")
    moov = next((b for b in top if b.type == b"moov"), None)
    if moov is None:
        raise InputError("no moov")
    trak = next((t for t in boxes(data, moov.payload, moov.end) if t.type == b"trak" and track_id(data, t) == track),
                None)
    if trak is None:
        raise InputError(f"no trak with track_ID {track}")
    stbl = None
    mdia = child(data, trak, b"mdia")
    minf = child(data, mdia, b"minf") if mdia else None
    stbl = child(data, minf, b"stbl") if minf else None
    if stbl is None:
        raise InputError("trak without mdia/minf/stbl")
    entry, length_size = sample_entry(data, stbl)
    sizes = sample_sizes(data, stbl)
    offsets = sample_offsets(data, stbl, sizes)
    out_dir.mkdir(parents=True, exist_ok=True)
    records = []
    for index in parse_selection(selection, len(sizes)):
        offset, size = offsets[index], sizes[index]
        if offset + size > len(data):
            raise InputError(f"sample {index} ({offset}+{size}) runs past the end of the file")
        sample = data[offset : offset + size]
        (out_dir / f"sample_{index}.bin").write_bytes(sample)
        record = {"index": index, "offset": offset, "size": size, "nal_length_size": length_size}
        if length_size:
            record["nal_count"] = count_nals(sample, length_size)
        records.append(record)
    summary = {"file": path.name, "track": track, "sample_entry": entry, "nal_length_size": length_size,
               "samples": records}
    (out_dir / "samples.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return summary


def corpus_file(corpus_id: str, default_root: Path) -> Path | None:
    root = Path(os.environ.get("OVT_CORPORA_DIR") or default_root)
    entry = root / corpus_id
    ok = entry / ".ok"
    if not ok.is_file():
        return None
    try:
        primary = json.loads(ok.read_text(encoding="utf-8") or "{}").get("primary")
    except json.JSONDecodeError:
        primary = None
    if primary:
        return entry / primary
    files = [f for f in entry.iterdir() if f.is_file() and f.name != ".ok"]
    return files[0] if len(files) == 1 else None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("file", nargs="?", type=Path)
    parser.add_argument("--corpus-id")
    parser.add_argument("--corpora-dir", type=Path, default=Path("build/corpora"),
                        help="corpus cache used when $OVT_CORPORA_DIR is unset (default build/corpora)")
    parser.add_argument("--track", type=int, required=True)
    parser.add_argument("--samples")
    parser.add_argument("-o", "--output", type=Path, required=True)
    args = parser.parse_args(argv)
    path = args.file
    if args.corpus_id:
        path = corpus_file(args.corpus_id, args.corpora_dir)
        if path is None:
            print(f"skip: corpus {args.corpus_id} not present", file=sys.stderr)
            return SKIP
    if path is None:
        parser.error("give FILE or --corpus-id")
    try:
        summary = extract(path, args.track, args.samples, args.output)
    except (InputError, OSError, struct.error) as exc:
        print(f"{path}: {exc}", file=sys.stderr)
        return 2
    print(f"{len(summary['samples'])} sample(s) of track {args.track} ({summary['sample_entry']}) -> {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
