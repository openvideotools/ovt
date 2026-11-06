#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""JM ``ldecod`` TRACE output (``trace_dec.txt``) -> ``block-grid`` macroblock lists (chapter 14 §14.4.3).

JM 19.0 writes one line per syntax element, ``@<bit or symbol count> <name> [<bits>] (<value>)`` (CAVLC
through ``tracebits``, CABAC without the bit string), and before every macroblock a header
``*********** POC: <poc> (I/P) MB: <addr> Slice: <n> Type <slice_type> **********`` (JM's ``SliceType``:
0 P, 1 B, 2 I, 3 SP, 4 SI). Parameter-set and slice-header lines carry the prefixes ``SPS: ``, ``PPS: ``
and ``SH: ``.

Per frame (a new frame starts at ``SH: first_mb_in_slice`` = 0) the output lists the slices
``{first_mb_in_slice, slice_type, slice_qp_delta, num_ref_idx_active[2]}`` and the macroblocks
``{mb_addr, mb_type, sub_mb_type[], ref_idx_l0[], ref_idx_l1[], mvd_l0[[x,y]...], mvd_l1[...],
intra4x4_pred_mode[], intra_chroma_pred_mode, cbp, mb_qp_delta, transform_size_8x8_flag, qp}``. Arrays hold
the values in read order (only what the stream signals). ``mb_type`` is the value JM traces; a skipped
macroblock (no ``mb_type`` read: inside an ``mb_skip_run``, ``mb_skip_flag`` = 1, or an address gap
between two headers) gets the string ``"P_Skip"`` / ``"B_Skip"``. ``qp`` is the final QP_Y, accumulated
from ``pic_init_qp_minus26``, ``slice_qp_delta`` and every ``mb_qp_delta`` (H.264 equation 7-37).
Element names are mapped by ``jm_name_map.json``; names it does not list are ignored (coefficients, CABAC
engine state).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canon  # noqa: E402

MB_RE = re.compile(r"^\*+ POC: (-?\d+) \(I/P\) MB: (\d+) Slice: (\d+) Type (\d+) \*+$")
ELEM_RE = re.compile(r"^@(\d+)\s+(.+?)\s+(?:([01]+)\s+)?\(\s*(-?\d+)\)\s*$")
SKIP_NAME = {0: "P_Skip", 1: "B_Skip", 3: "P_Skip"}


# SPEC-ASSUMPTION(#93): per-stream block-grid {"frames": [...]} with mbs/slices; arrays in read order.
class Frame:
    def __init__(self, index: int) -> None:
        self.index = index
        self.poc: int | None = None
        self.slices: list[dict[str, Any]] = []
        self.mbs: list[dict[str, Any]] = []


def mb_record(addr: int) -> dict[str, Any]:
    return {"mb_addr": addr}


def convert(args: argparse.Namespace) -> dict[str, Any]:
    names = canon.load_map("jm_name_map")
    header_keys: dict[str, str] = names["header"]
    mb_keys: dict[str, str] = names["mb"]
    sps: dict[int, dict[str, int]] = {}
    pps: dict[int, dict[str, int]] = {}
    cur_sps: dict[str, int] = {}
    cur_pps: dict[str, int] = {}
    frames: list[Frame] = []
    slice_hdr: dict[str, int] = {}
    mb: dict[str, Any] | None = None
    slice_type = 2
    qp = qp_off = 0
    next_addr = 0
    pic_size: int | None = None

    def finish_mb() -> None:
        nonlocal mb, qp
        if mb is None:
            return
        if "mb_type" not in mb:
            mb["mb_type"] = SKIP_NAME.get(slice_type, "P_Skip")
        # I_PCM carries no mb_qp_delta: QP_Y = QP_Y,PRED (7.4.5), so the running QP is unchanged. The 0 that
        # H.264 8.7.2.2 uses for an I_PCM edge is a deblocking rule, not the macroblock's QP_Y.
        if "mb_qp_delta" in mb:
            qp = (qp + mb["mb_qp_delta"] + 52 + 2 * qp_off) % (52 + qp_off) - qp_off
        mb["qp"] = qp
        frames[-1].mbs.append(mb)
        mb = None

    def fill_skipped(upto: int) -> None:
        """Synthesise skipped MBs for addresses next_addr..upto-1 that have no trace lines at all."""
        nonlocal mb, next_addr
        for gap in range(next_addr, upto):
            mb = mb_record(gap)
            finish_mb()
        next_addr = max(next_addr, upto)

    def start_slice() -> None:
        nonlocal qp, qp_off, slice_type, pic_size
        p = pps.get(slice_hdr.get("pic_parameter_set_id", 0), {})
        s = sps.get(p.get("seq_parameter_set_id", 0), {})
        qp_off = 6 * s.get("bit_depth_luma_minus8", 0)
        if "pic_width_in_mbs_minus1" in s and "pic_height_in_map_units_minus1" in s:
            frame_mbs = 2 - s.get("frame_mbs_only_flag", 1) if not slice_hdr.get("field_pic_flag", 0) else 1
            pic_size = (s["pic_width_in_mbs_minus1"] + 1) * (s["pic_height_in_map_units_minus1"] + 1) * frame_mbs
        qp = 26 + p.get("pic_init_qp_minus26", 0) + slice_hdr.get("slice_qp_delta", 0)
        slice_type = slice_hdr.get("slice_type", 2) % 5
        active = [
            slice_hdr.get(f"num_ref_idx_l{k}_active_minus1", p.get(f"num_ref_idx_l{k}_default_active_minus1", 0)) + 1
            for k in (0, 1)
        ]
        if slice_type not in (0, 1, 3):
            active = [0, 0]
        elif slice_type != 1:
            active[1] = 0
        frames[-1].slices.append(
            {
                "first_mb_in_slice": slice_hdr["first_mb_in_slice"],
                "slice_type": slice_hdr.get("slice_type", 2),
                "slice_qp_delta": slice_hdr.get("slice_qp_delta", 0),
                "num_ref_idx_active": active,
            }
        )

    slice_open = False
    for line in canon.read_text(args.input).splitlines():
        line = line.strip()
        m = MB_RE.match(line)
        if m:
            if slice_hdr and not slice_open:
                start_slice()
                slice_open = True
            if not frames:
                raise canon.NormalizeError("macroblock before the first slice header")
            poc, addr = int(m.group(1)), int(m.group(2))
            finish_mb()
            fill_skipped(addr)
            frames[-1].poc = poc
            mb = mb_record(addr)
            next_addr = addr + 1
            continue
        e = ELEM_RE.match(line)
        if not e:
            continue
        name, value = e.group(2).strip(), int(e.group(4))
        if name.startswith(("SPS: ", "PPS: ", "SH: ")):
            section, key = name.split(": ", 1)
            if section == "SPS":
                if key == "seq_parameter_set_id":
                    cur_sps = sps.setdefault(value, {})
                cur_sps[key] = value
            elif section == "PPS":
                if key == "pic_parameter_set_id":
                    cur_pps = pps.setdefault(value, {})
                cur_pps[key] = value
            elif key in header_keys:
                if key == "first_mb_in_slice":
                    finish_mb()
                    if value == 0 and frames and pic_size is not None:
                        fill_skipped(pic_size)
                    slice_hdr, slice_open = {}, False
                    if value == 0 or not frames:
                        frames.append(Frame(len(frames)))
                        next_addr = 0
                slice_hdr[header_keys[key]] = value
            continue
        if mb is None:
            continue
        key = mb_keys.get(re.sub(r"\s+", " ", name))
        if key is None:
            if name.startswith("pcm_sample"):
                mb["pcm"] = 1
            continue
        if key == "mb_skip_flag":
            continue
        if key in names["arrays"]:
            mb.setdefault(key, []).append(value)
        elif key in names["vectors"]:
            vec = mb.setdefault(key, [])
            if not vec or len(vec[-1]) == 2:
                vec.append([])
            vec[-1].append(value)
        else:
            mb[key] = value
    finish_mb()
    if frames and pic_size is not None:
        fill_skipped(pic_size)
    out = []
    for f in frames:
        entry: dict[str, Any] = {"frame": f.index, "slices": f.slices, "mbs": f.mbs}
        if f.poc is not None:
            entry["poc"] = f.poc
        out.append(entry)
    return {"frames": out}


if __name__ == "__main__":
    sys.exit(canon.run("block-grid", __doc__.splitlines()[0], convert))
