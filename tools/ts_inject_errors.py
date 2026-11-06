#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Inject TR 101 290 errors into an MPEG-2 transport stream (§14.3.3 recipe 33; WI-0.553).

    ts_inject_errors.py [--cc-errors N] [--pcr-gap SECONDS] in.ts out.ts

``--cc-errors N`` drops N packets of the first video PID of the first PMT, spread evenly over the stream and
never adjacent, so each drop is exactly one ``continuity_count_error``; packets that carry a PCR or start a PES
are never dropped. ``--pcr-gap S`` removes every PCR of that window of S seconds that starts one second after
the first PCR, turning its 6 bytes into adaptation-field stuffing (packet length and payload unchanged), so the
PCR interval there exceeds S. The packet indices touched are printed as JSON on stdout. Exit 0 ok, 2 bad input.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

PACKET, SYNC, PCR_HZ = 188, 0x47, 27_000_000
VIDEO_TYPES = {0x01, 0x02, 0x10, 0x1B, 0x24, 0x33}  # MPEG-1/2, MPEG-4 Part 2, H.264, H.265, H.266


def packets(data: bytes) -> list[bytearray]:
    if len(data) % PACKET or any(data[i] != SYNC for i in range(0, len(data), PACKET)):
        raise ValueError("not a 188-byte transport stream")
    return [bytearray(data[i : i + PACKET]) for i in range(0, len(data), PACKET)]


def pid(p: bytes | bytearray) -> int:
    return (p[1] & 0x1F) << 8 | p[2]


def payload(p: bytes | bytearray) -> bytes:
    afc = (p[3] >> 4) & 3
    start = 4 + (1 + p[4] if afc & 2 else 0)
    return bytes(p[start:]) if afc & 1 else b""


def section(p: bytes | bytearray) -> bytes:
    data = payload(p)
    return data[1 + data[0] :] if p[1] & 0x40 else b""  # skip pointer_field


def video_pid(pkts: list[bytearray]) -> int:
    pmt = None
    for p in pkts:
        sec = section(p)
        if pmt is None and pid(p) == 0 and sec[:1] == b"\x00":
            pmt = (sec[10] & 0x1F) << 8 | sec[11]  # first program after the 8-byte header
        elif pmt is not None and pid(p) == pmt and sec[:1] == b"\x02":
            length = (sec[1] & 0x0F) << 8 | sec[2]
            pos = 12 + ((sec[10] & 0x0F) << 8 | sec[11])
            while pos < 3 + length - 4:
                stype, epid = sec[pos], (sec[pos + 1] & 0x1F) << 8 | sec[pos + 2]
                if stype in VIDEO_TYPES:
                    return epid
                pos += 5 + ((sec[pos + 3] & 0x0F) << 8 | sec[pos + 4])
    raise ValueError("no video stream in the first PMT")


def pcr(p: bytes | bytearray) -> int | None:
    if (p[3] >> 4) & 2 and p[4] >= 7 and p[5] & 0x10:
        b = p[6:12]
        base = b[0] << 25 | b[1] << 17 | b[2] << 9 | b[3] << 1 | b[4] >> 7
        return base * 300 + ((b[4] & 1) << 8 | b[5])
    return None


def strip_pcr(p: bytearray) -> None:
    end = 5 + p[4]  # first byte after the adaptation field
    p[5] &= ~0x10 & 0xFF
    p[6:end] = p[12:end] + b"\xff" * 6


def inject(data: bytes, cc_errors: int, pcr_gap: float) -> tuple[bytes, dict]:
    pkts = packets(data)
    vpid = video_pid(pkts)
    report: dict = {"video_pid": vpid, "dropped": [], "pcr_removed": []}
    pcrs = [(i, v) for i, p in enumerate(pkts) if (v := pcr(p)) is not None]
    if pcr_gap:
        if not pcrs:
            raise ValueError("no PCR to remove")
        t0 = pcrs[0][1] + PCR_HZ
        report["pcr_removed"] = [i for i, v in pcrs if t0 <= v < t0 + pcr_gap * PCR_HZ]
        if not report["pcr_removed"] or pcrs[-1][1] < t0 + pcr_gap * PCR_HZ:
            raise ValueError(f"the stream is too short for a {pcr_gap} s PCR gap after 1 s")
        for i in report["pcr_removed"]:
            strip_pcr(pkts[i])
    if cc_errors:
        candidates = [i for i, p in enumerate(pkts) if pid(p) == vpid and not p[1] & 0x40 and pcr(p) is None]
        if len(candidates) < 3 * cc_errors:
            raise ValueError(f"too few video packets for {cc_errors} separate drops")
        step = len(candidates) / (cc_errors + 1)
        report["dropped"] = [candidates[int(step * (k + 1))] for k in range(cc_errors)]
    drop = set(report["dropped"])
    return b"".join(bytes(p) for i, p in enumerate(pkts) if i not in drop), report


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--cc-errors", type=int, default=0, help="video packets to drop")
    ap.add_argument("--pcr-gap", type=float, default=0, help="seconds without PCR")
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args(argv)
    if args.cc_errors <= 0 and args.pcr_gap <= 0:
        ap.error("give --cc-errors, --pcr-gap or both")
    try:
        out, report = inject(args.input.read_bytes(), args.cc_errors, args.pcr_gap)
    except (ValueError, IndexError, OSError) as e:
        print(f"ts_inject_errors: {e}", file=sys.stderr)
        return 2
    args.output.write_bytes(out)
    print(json.dumps(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
