#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Give an H.265 stream several PPS and switch between them (§14.3.3 recipe 45; WI-0.553).

    make_multi_pps.py in.265 --pps-ids 0,1,2 [--resend-at 20,40] -o out.265

The first PPS of the input (id = the first of ``--pps-ids``) is cloned once per further id; the clones follow
the original. Every slice of picture n (decode order) is rewritten to ``slice_pic_parameter_set_id`` =
``pps_ids[n % len(pps_ids)]`` (H.265 §7.3.6.1), and all PPS are sent again before each picture in ``--resend-at``.

SPEC-ASSUMPTION(#159): §14.3.3 asks for clones with a different ``init_qp_minus26``, but that changes SliceQpY
and with it the CABAC initialisation, so the slice data no longer decodes. Instead each PPS differs in
``num_extra_slice_header_bits`` and ``output_flag_present_flag``, chosen so that the slice header length changes
by whole bytes only: the Exp-Golomb code of the new id plus the added zero ``slice_reserved_flag`` bits plus the
added ``pic_output_flag`` (= 1, the inferred value) is congruent modulo 8 to the original id's code length. The
CABAC data keeps its byte alignment, the first id's PPS is unchanged and the decoded pictures are unchanged. Only
streams with one slice segment per picture are supported. The per-picture PPS id is printed as JSON on stdout for
the ``expected.json`` sidecar.
Exit 0 ok, 2 bad input.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from bitstream import BitReader, BitWriter, Nal, escape, h265_type, join_annexb, payload_bits, split_annexb, unescape
from bitstream import with_trailing_bits

PPS_NUT = 34


def ue_len(v: int) -> int:
    return 2 * (v + 1).bit_length() - 1


def pps_extra(nal: bytes) -> int:
    """``num_extra_slice_header_bits`` of a PPS."""
    r = BitReader(unescape(nal), 16)
    r.ue()
    r.ue()
    r.u(2)
    return r.u(3)


def assign_layout(ids: list[int], extra: int) -> dict[int, tuple[int, int]]:
    """Per id, (added slice_reserved_flag bits, output_flag_present_flag) such that the slice header length
    changes by a multiple of 8 bits only and no two PPS are alike; the first id keeps (0, 0)."""
    used: set[tuple[int, int]] = set()
    layout = {}
    for k, v in enumerate(ids):
        total = ue_len(ids[0])
        while total < ue_len(v):
            total += 8
        options = [(total - ue_len(v) - out, out) for out in (k % 2, 1 - k % 2)]
        choice = next((o for o in options if 0 <= o[0] <= 7 - extra and o not in used), None)
        if choice is None:
            raise ValueError(f"PPS id {v} cannot keep the slice header byte alignment")
        used.add(choice)
        layout[v] = choice
    return layout


def rewrite_pps(nal: bytes, pps_id: int, extra_bits: int, output_flag: int) -> bytes:
    """The PPS with a new id, ``extra_bits`` more extra slice header bits and ``output_flag_present_flag`` set
    to ``output_flag``."""
    rbsp = unescape(nal)
    r, w = BitReader(rbsp, 16), BitWriter()
    r.ue()  # pps_pic_parameter_set_id
    w.ue(pps_id)
    w.ue(r.ue())  # pps_seq_parameter_set_id
    w.u(1, r.u(1))  # dependent_slice_segments_enabled_flag
    if r.u(1):
        raise ValueError("the input PPS already has output_flag_present_flag = 1")
    w.u(1, output_flag)
    extra = r.u(3)  # num_extra_slice_header_bits
    if extra + extra_bits > 7:
        raise ValueError("num_extra_slice_header_bits would exceed 7")
    w.u(3, extra + extra_bits)
    w.raw(r.bits(r.pos, payload_bits(rbsp)))
    return escape(rbsp[:2] + with_trailing_bits("".join(w.bits)))


def rewrite_slice(nal: bytes, pps_id: int, extra: int, extra_bits: int, output_flag: int) -> bytes:
    rbsp = unescape(nal)
    r, w = BitReader(rbsp, 16), BitWriter()
    if not r.u(1):
        raise ValueError("only streams with one slice segment per picture are supported")
    w.u(1, 1)  # first_slice_segment_in_pic_flag
    if 16 <= h265_type(nal) <= 23:
        w.u(1, r.u(1))  # no_output_of_prior_pics_flag
    r.ue()
    w.ue(pps_id)
    w.u(extra, r.u(extra))  # the original slice_reserved_flag bits
    w.u(extra_bits, 0)  # the added ones
    w.ue(r.ue())  # slice_type
    w.u(output_flag, output_flag)  # pic_output_flag = 1 when present
    w.raw(r.bits(r.pos, payload_bits(rbsp)))
    return escape(rbsp[:2] + with_trailing_bits("".join(w.bits)))


def multi_pps(data: bytes, ids: list[int], resend_at: set[int]) -> tuple[bytes, list[int]]:
    nals = split_annexb(data)
    original = next((n for n in nals if h265_type(n.data) == PPS_NUT), None)
    if original is None:
        raise ValueError("no PPS")
    if BitReader(unescape(original.data), 16).ue() != ids[0]:
        raise ValueError(f"the first PPS does not have id {ids[0]}")
    layout = assign_layout(ids, pps_extra(original.data))
    extra = pps_extra(original.data)
    all_pps = [rewrite_pps(original.data, v, *layout[v]) for v in ids]
    out: list[Nal] = []
    per_picture: list[int] = []
    for nal in nals:
        t = h265_type(nal.data)
        if t == PPS_NUT:
            if nal is original:
                out += [Nal(nal.prefix, d) for d in all_pps]
                continue
            raise ValueError("the input has more than one PPS")
        if t < 32:
            pid = ids[len(per_picture) % len(ids)]
            if len(per_picture) in resend_at:
                out += [Nal(b"\x00\x00\x00\x01", d) for d in all_pps]
            per_picture.append(pid)
            nal = Nal(nal.prefix, rewrite_slice(nal.data, pid, extra, *layout[pid]))
        out.append(nal)
    return join_annexb(out), per_picture


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("input", type=Path)
    ap.add_argument("--pps-ids", required=True, help="comma-separated, the first is the input PPS id")
    ap.add_argument("--resend-at", default="", help="comma-separated picture indices")
    ap.add_argument("-o", "--output", type=Path, required=True)
    args = ap.parse_args(argv)
    try:
        ids = [int(v) for v in args.pps_ids.split(",")]
        resend = {int(v) for v in args.resend_at.split(",") if v}
        if len(set(ids)) != len(ids) or not all(0 <= v <= 63 for v in ids):
            raise ValueError("--pps-ids must be distinct values in 0..63")
        out, per_picture = multi_pps(args.input.read_bytes(), ids, resend)
    except (ValueError, IndexError, OSError) as e:
        print(f"make_multi_pps: {e}", file=sys.stderr)
        return 2
    args.output.write_bytes(out)
    print(json.dumps({"pps_ids": ids, "resend_at": sorted(resend), "pps_per_picture": per_picture}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
