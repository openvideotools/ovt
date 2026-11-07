#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Deterministic corruption of H.264 / H.265 Annex B streams (§14.3.3 recipes 49, 50; chapter 7; WI-0.553).

    corrupt.py [--codec h264|h265|av1|av1-annexb] MODE in out
    corrupt.py --condition CODE [--conditions FILE] in out
    corrupt.py --all-codes [--conditions FILE] --input-dir DIR --out-dir DIR

H.264 / H.265 modes (exactly one):

* ``--truncate-nal TYPE --keep-bytes N``: cut the first NAL unit of TYPE to its first N bytes (header included);
* ``--drop-nal TYPE``: remove every NAL unit of TYPE;
* ``--flip-slice-data --frame F --byte B``: invert byte B (counted from the NAL header) of the first slice of
  picture F (decode order, from 0);
* ``--flip-hash-sei --frame F``: invert the lowest bit of the first hash byte of the H.265 decoded picture hash
  SEI (payloadType 132) of picture F;
* ``--byte-xor OFFSET``: invert the byte at file offset OFFSET (chapter 7 ``tests/golden/h265/corrupt``);
* ``--set-bits TYPE --bit B --width W --value V``: write V into W bits at RBSP bit B (from the NAL header);
* ``--set-ue TYPE --bit B --value V``: re-encode the ``ue(v)`` that starts at RBSP bit B (the rest of the RBSP
  moves with it; trailing bits are rewritten);
* ``--poke TYPE --byte B --hex HEX``: overwrite NAL bytes from byte B with HEX, without emulation prevention;
* ``--duplicate-nal TYPE``: insert a copy of the NAL unit right after it;
* ``--move-nal TYPE --before TYPE2``: move the TYPE units of the first access unit before the first TYPE2 unit;
* ``--drop-picture F``: remove the slices of picture F.

``--drop-nal`` takes a comma list or ``all``. TYPE is a name (``vps``, ``sps``, ``pps``, ``sei``, ``aud``,
``slice`` for any VCL unit) or a ``nal_unit_type`` number; ``--nth N`` (default 0) picks the N-th unit of TYPE.

AV1 modes (``--codec av1``: low-overhead OBUs, ``.obu``/``.ivf``; ``av1-annexb``: Annex B), on temporal unit
``--tu T``: ``--strip-obu-size`` (clear ``obu_has_size_field`` of OBU ``--nth N`` and drop its size),
``--drop-obu TYPE`` (``obu_type`` number), ``--truncate-obu TYPE`` (keep half of its payload, sizes kept
consistent) and ``--merge-tu`` (move TU T+1 into TU T: low-overhead drops T+1's temporal delimiter, Annex B
joins the frame units under one ``temporal_unit_size``).

``--condition CODE`` applies the edit that the condition table ``tests/corpora/corrupt_conditions.json`` gives
for a chapter 7 §7.9 diagnostic code; ``--all-codes`` writes ``syn_broken_syntax_<code>.<ext>`` for every code
of the table, reading each input from ``--input-dir`` (§14.3.3 recipe 49).

The codec defaults from the input extension (``.265``/``.hevc``/``.h265`` H.265, ``.obu``/``.ivf`` AV1). A JSON
record of the edit goes to stdout. Exit 0 ok, 2 bad input or nothing to corrupt.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from bitstream import (
    BitReader,
    Nal,
    Obu,
    escape,
    h264_type,
    h265_type,
    join_annexb,
    leb128,
    payload_bits,
    read_ivf,
    read_leb128,
    split_annexb,
    split_obus,
    unescape,
    with_trailing_bits,
    write_ivf,
)

CONDITIONS = Path(__file__).resolve().parents[1] / "tests" / "corpora" / "corrupt_conditions.json"

NAMES = {
    "h264": {"sps": 7, "pps": 8, "sei": 6, "aud": 9},
    "h265": {"vps": 32, "sps": 33, "pps": 34, "aud": 35, "sei": 39, "suffix_sei": 40},
}
DECODED_PICTURE_HASH = 132
EMULATION = re.compile(b"\x00\x00[\x00-\x03]")


def nal_type(codec: str, nal: Nal) -> int:
    return h265_type(nal.data) if codec == "h265" else h264_type(nal.data)


def is_vcl(codec: str, t: int) -> bool:
    return t < 32 if codec == "h265" else 1 <= t <= 5


def first_slice_of_picture(codec: str, nal: Nal) -> bool:
    rbsp = unescape(nal.data[:16])
    if codec == "h265":
        return bool(rbsp[2] & 0x80)  # first_slice_segment_in_pic_flag
    return BitReader(rbsp, 8).ue() == 0  # first_mb_in_slice


def picture_index(codec: str, nals: list[Nal]) -> list[int]:
    """Per NAL, the index of the picture it belongs to (non-VCL units count towards the next picture)."""
    out, current = [], -1
    for nal in nals:
        t = nal_type(codec, nal)
        if is_vcl(codec, t) and first_slice_of_picture(codec, nal):
            current += 1
        suffix = codec == "h265" and t in (38, 40)  # FD and suffix SEI follow their picture
        out.append(current if is_vcl(codec, t) or suffix else current + 1)
    return out


def type_number(codec: str, name: str) -> int:
    return int(name) if name.isdigit() else NAMES[codec][name]


def matches(codec: str, nal: Nal, name: str) -> bool:
    t = nal_type(codec, nal)
    return is_vcl(codec, t) if name == "slice" else t == type_number(codec, name)


def nth(codec: str, nals: list[Nal], name: str, n: int) -> int:
    found = [i for i, nal in enumerate(nals) if matches(codec, nal, name)]
    if len(found) <= n:
        raise ValueError(f"no NAL unit {name} number {n}")
    return found[n]


def set_bits(rbsp: bytes, bit: int, width: int, value: int) -> bytes:
    if bit + width > 8 * len(rbsp) or value >> width:
        raise ValueError(f"{width} bits at {bit} do not fit the unit or the value")
    n = int.from_bytes(rbsp, "big")
    shift = 8 * len(rbsp) - bit - width
    n = n & ~(((1 << width) - 1) << shift) | value << shift
    return n.to_bytes(len(rbsp), "big")


def set_ue(rbsp: bytes, bit: int, value: int) -> bytes:
    r = BitReader(rbsp, bit)
    r.ue()
    end, stop = r.pos, payload_bits(rbsp)
    code = format(value + 1, "b")
    reader = BitReader(rbsp)
    return with_trailing_bits(reader.bits(0, bit) + "0" * (len(code) - 1) + code + reader.bits(end, stop))


def edit_nals(nals: list[Nal], codec: str, args: argparse.Namespace) -> dict | None:
    """The NAL-level modes added for the condition table; None when another mode is selected."""
    if args.set_bits or args.set_ue or args.poke or args.duplicate_nal:
        i = nth(codec, nals, args.set_bits or args.set_ue or args.poke or args.duplicate_nal, args.nth)
        if args.set_bits:
            nals[i].data = escape(set_bits(unescape(nals[i].data), args.bit, args.width, args.value))
        elif args.set_ue:
            nals[i].data = escape(set_ue(unescape(nals[i].data), args.bit, args.value))
        elif args.poke:
            patch = bytes.fromhex(args.hex)
            if not 0 < args.byte <= len(nals[i].data) - len(patch):
                raise ValueError(f"byte {args.byte} is outside NAL unit {i}")
            nals[i].data = nals[i].data[: args.byte] + patch + nals[i].data[args.byte + len(patch) :]
        else:
            nals.insert(i + 1, Nal(nals[i].prefix, nals[i].data))
        return {"nal": i}
    if args.move_nal:
        first = [
            i for i, n in enumerate(nals) if picture_index(codec, nals)[i] == 0 and matches(codec, n, args.move_nal)
        ]
        target = nth(codec, nals, args.before, 0)
        if not first or target > first[0]:
            raise ValueError(f"no {args.move_nal} unit after the first {args.before} in the first access unit")
        moved = [nals[i] for i in first]
        rest = [n for i, n in enumerate(nals) if i not in first]
        rest[target:target] = moved
        nals[:] = rest
        return {"moved": first, "before": target}
    if args.drop_picture is not None:
        pics = picture_index(codec, nals)
        drop = [i for i, n in enumerate(nals) if pics[i] == args.drop_picture and is_vcl(codec, nal_type(codec, n))]
        if not drop:
            raise ValueError(f"no picture {args.drop_picture}")
        nals[:] = [n for i, n in enumerate(nals) if i not in drop]
        return {"dropped": drop}
    return None


# ------------------------------------------------------------------------------------------------------ AV1


def av1_units(data: bytes, annexb: bool) -> list[list[list[Obu]]]:
    """Temporal units, each a list of frame units (one for the low-overhead format), each a list of OBUs."""
    if not annexb:
        tus: list[list[list[Obu]]] = []
        for obu in split_obus(data):
            if obu.type == 2 or not tus:
                tus.append([[]])
            tus[-1][0].append(obu)
        return tus
    tus, pos = [], 0
    while pos < len(data):
        size, pos = read_leb128(data, pos)
        end, frames = pos + size, []
        while pos < end:
            fsize, pos = read_leb128(data, pos)
            fend, obus = pos + fsize, []
            while pos < fend:
                osize, pos = read_leb128(data, pos)
                obus += split_obus(data[pos : pos + osize])
                pos += osize
            frames.append(obus)
        tus.append(frames)
    return tus


def av1_join(tus: list[list[list[Obu]]], annexb: bool) -> bytes:
    if not annexb:
        return b"".join(o.encode() for tu in tus for fu in tu for o in fu)
    out = b""
    for tu in tus:
        body = b""
        for fu in tu:
            units = b"".join(leb128(len(o.encode())) + o.encode() for o in fu)
            body += leb128(len(units)) + units
        out += leb128(len(body)) + body
    return out


def corrupt_av1(data: bytes, annexb: bool, args: argparse.Namespace) -> tuple[bytes, dict]:
    ivf = data[:4] == b"DKIF"
    header, frames = read_ivf(data) if ivf else (b"", [])
    tus = [[split_obus(tu)] for _, tu in frames] if ivf else av1_units(data, annexb)
    t = args.tu
    if not 0 <= t < len(tus) - (1 if args.merge_tu else 0):
        raise ValueError(f"no temporal unit {t}")
    obus = [o for fu in tus[t] for o in fu]
    if args.strip_obu_size:
        if annexb or args.nth >= len(obus) - 1:
            raise ValueError("--strip-obu-size needs a low-overhead OBU that is not the last of its unit")
        o = obus[args.nth]
        o.header = bytes([o.header[0] & ~0x02]) + o.header[1:]
    elif args.drop_obu is not None or args.truncate_obu is not None:
        kind = args.drop_obu if args.drop_obu is not None else args.truncate_obu
        hit = [o for o in obus if o.type == kind]
        if not hit:
            raise ValueError(f"temporal unit {t} has no OBU of type {kind}")
        if args.drop_obu is not None:
            tus[t] = [[o for o in fu if o.type != kind] for fu in tus[t]]
        else:
            hit[0].payload = hit[0].payload[: len(hit[0].payload) // 2]
    else:  # merge
        nxt = tus.pop(t + 1)
        if annexb:
            tus[t] += nxt
        else:
            tus[t][-1] += [o for fu in nxt for o in fu if o.type != 2]
        if ivf:
            frames.pop(t + 1)
    if ivf:
        return write_ivf(header, [(pts, av1_join([tu], False)) for (pts, _), tu in zip(frames, tus)]), {"tu": t}
    return av1_join(tus, annexb), {"tu": t}


# -------------------------------------------------------------------------------------------- conditions


def condition_argv(code: str, path: Path = CONDITIONS) -> tuple[list[str], dict]:
    table = json.loads(path.read_text(encoding="utf-8"))
    entry = next((c for c in table["conditions"] if c["code"] == code), None)
    if entry is None:
        raise ValueError(f"{code} is not in {path.name}")
    return list(entry["edit"]), entry


def output_name(entry: dict) -> str:
    return f"syn_broken_syntax_{entry['code'].replace('-', '_')}{Path(entry['input_file']).suffix}"


def flip_hash(rbsp: bytes) -> bytes | None:
    pos = 2
    while pos < len(rbsp) and rbsp[pos:] != b"\x80":
        ptype = psize = 0
        while rbsp[pos] == 0xFF:
            ptype, pos = ptype + 255, pos + 1
        ptype, pos = ptype + rbsp[pos], pos + 1
        while rbsp[pos] == 0xFF:
            psize, pos = psize + 255, pos + 1
        psize, pos = psize + rbsp[pos], pos + 1
        if ptype == DECODED_PICTURE_HASH:
            target = pos + 1  # after hash_type
            return rbsp[:target] + bytes([rbsp[target] ^ 0x01]) + rbsp[target + 1 :]
        pos += psize
    return None


def corrupt(data: bytes, codec: str, args: argparse.Namespace) -> tuple[bytes, dict]:
    if codec.startswith("av1"):
        return corrupt_av1(data, codec == "av1-annexb", args)
    if args.byte_xor is not None:
        if not 0 <= args.byte_xor < len(data):
            raise ValueError(f"offset {args.byte_xor} is outside the file")
        return data[: args.byte_xor] + bytes([data[args.byte_xor] ^ 0xFF]) + data[args.byte_xor + 1 :], {
            "offset": args.byte_xor
        }
    nals = split_annexb(data)
    if (record := edit_nals(nals, codec, args)) is not None:
        return join_annexb(nals), record
    if args.drop_nal:
        names = args.drop_nal.split(",")
        dropped = [i for i, n in enumerate(nals) if "all" in names or any(matches(codec, n, x) for x in names)]
        if not dropped:
            raise ValueError(f"no NAL unit of type {args.drop_nal}")
        return join_annexb([n for i, n in enumerate(nals) if i not in dropped]), {"dropped": dropped}
    if args.truncate_nal:
        t = type_number(codec, args.truncate_nal)
        i = next((i for i, n in enumerate(nals) if nal_type(codec, n) == t), None)
        if i is None or args.keep_bytes is None or not 0 < args.keep_bytes < len(nals[i].data):
            raise ValueError(f"no NAL unit of type {args.truncate_nal} longer than --keep-bytes")
        nals[i].data = nals[i].data[: args.keep_bytes]
        return join_annexb(nals), {"nal": i, "keep_bytes": args.keep_bytes}
    pics = picture_index(codec, nals)
    if args.flip_slice_data:
        i = next((i for i, n in enumerate(nals) if pics[i] == args.frame and is_vcl(codec, nal_type(codec, n))), None)
        if i is None or not 2 <= args.byte < len(nals[i].data):
            raise ValueError(f"picture {args.frame} has no slice with byte {args.byte}")
        d = nals[i].data
        nals[i].data = d[: args.byte] + bytes([d[args.byte] ^ 0xFF]) + d[args.byte + 1 :]
        window = slice(max(0, args.byte - 2), args.byte + 3)
        if EMULATION.search(nals[i].data[window]) and not EMULATION.search(d[window]):
            raise ValueError(f"inverting byte {args.byte} would create a start code or emulation sequence")
        return join_annexb(nals), {"nal": i, "frame": args.frame, "byte": args.byte}
    if codec != "h265":
        raise ValueError("--flip-hash-sei needs an H.265 stream")
    for i, n in enumerate(nals):
        if pics[i] == args.frame and nal_type(codec, n) in (39, 40):
            flipped = flip_hash(unescape(n.data))
            if flipped is not None:
                nals[i].data = escape(flipped)
                return join_annexb(nals), {"nal": i, "frame": args.frame}
    raise ValueError(f"picture {args.frame} has no decoded picture hash SEI")


def parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--codec", choices=("h264", "h265", "av1", "av1-annexb"))
    mode = ap.add_mutually_exclusive_group(required=True)
    for flag in ("truncate-nal", "drop-nal", "set-bits", "set-ue", "poke", "duplicate-nal", "move-nal"):
        mode.add_argument(f"--{flag}", metavar="TYPE")
    for flag in ("flip-slice-data", "flip-hash-sei", "strip-obu-size", "merge-tu", "all-codes"):
        mode.add_argument(f"--{flag}", action="store_true")
    mode.add_argument("--byte-xor", type=int, metavar="OFFSET")
    mode.add_argument("--drop-picture", type=int, metavar="F")
    mode.add_argument("--drop-obu", type=int, metavar="TYPE")
    mode.add_argument("--truncate-obu", type=int, metavar="TYPE")
    mode.add_argument("--condition", metavar="CODE")
    for flag in ("keep-bytes", "frame", "byte", "bit", "width", "value"):
        ap.add_argument(f"--{flag}", type=int)
    ap.add_argument("--nth", type=int, default=0)
    ap.add_argument("--tu", type=int, default=0)
    ap.add_argument("--hex", default="")
    ap.add_argument("--before", default="sps")
    ap.add_argument("--conditions", type=Path, default=CONDITIONS)
    ap.add_argument("--input-dir", type=Path)
    ap.add_argument("--out-dir", type=Path)
    ap.add_argument("input", type=Path, nargs="?")
    ap.add_argument("output", type=Path, nargs="?")
    return ap


def run(args: argparse.Namespace) -> dict:
    codec = args.codec or (
        "h265"
        if args.input.suffix.lower() in (".265", ".hevc", ".h265")
        else "av1"
        if args.input.suffix.lower() in (".obu", ".ivf")
        else "h264"
    )
    out, record = corrupt(args.input.read_bytes(), codec, args)
    args.output.write_bytes(out)
    return record


def main(argv: list[str] | None = None) -> int:
    ap = parser()
    args = ap.parse_args(argv)
    try:
        if args.all_codes:
            if not (args.input_dir and args.out_dir):
                ap.error("--all-codes needs --input-dir and --out-dir")
            table = json.loads(args.conditions.read_text(encoding="utf-8"))
            for entry in table["conditions"]:
                sub = ap.parse_args(
                    [*entry["edit"], str(args.input_dir / entry["input_file"]), str(args.out_dir / output_name(entry))]
                )
                print(json.dumps({"code": entry["code"], **run(sub)}))
            return 0
        if args.input is None or args.output is None:
            ap.error("input and output are required")
        if args.condition:
            edit, _ = condition_argv(args.condition, args.conditions)
            args = ap.parse_args([*edit, str(args.input), str(args.output)])
        if (args.flip_slice_data or args.flip_hash_sei) and args.frame is None:
            ap.error("--frame is required")
        if args.flip_slice_data and args.byte is None:
            ap.error("--byte is required with --flip-slice-data")
        print(json.dumps(run(args)))
    except (ValueError, KeyError, IndexError, OSError, TypeError) as e:
        print(f"corrupt: {e}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
