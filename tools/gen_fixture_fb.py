#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Write synthetic ``FrameAnalysis`` FlatBuffers for the overlay goldens (§14.3.3 non-stream fixtures; WI-0.553).

    gen_fixture_fb.py --kind grid|mv --size 1080p|4k|8k --out file.fb

* ``--kind grid``: an H.265 intra picture with a regular partition grid. CTU 64; the CU size of CTU (cx, cy)
  is ``64 >> ((cx + cy) % 4)`` (64, 32, 16, 8 in diagonal bands), and a CU that would cross the picture edge is
  split further, as HEVC's implicit split does. Cell grid: ``block_id``, ``pred`` (Intra), ``qp`` (a ramp).
* ``--kind mv``: an AV1 inter picture. SB 64, every 4×4 is a ``Block`` with one L0 ``MotionVector`` to LAST.
  The vectors form a rotation about the picture centre (1/16 sample units, clamped to ±32 samples).

The buffers follow the chapter 6 V-rules (§6.9). No generated code is needed: the tables and their field
order are read from ``core/schema/ovt_model.fbs`` and written with the ``flatbuffers`` Python runtime. Fields
are added the way flatc's C++ ``Create<Table>`` adds them (largest scalar first, reverse declaration order within
a size; defaults omitted), so the bytes are what a C++ builder writes. ``tools/gen_dense_fa.py`` uses this module.
Exit 0 ok, 2 usage.

SPEC-ASSUMPTION(#175): V-032's canonicalize() adds fields in that C++ ``Create<Table>`` order.
SPEC-ASSUMPTION(#173): ``--kind pattern`` (``pat_*.fb``) is not implemented; no schema table holds ``Picture`` planes.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import Any

import flatbuffers
import numpy as np

REPO = Path(__file__).resolve().parents[1]
SCHEMA = REPO / "core" / "schema" / "ovt_model.fbs"
SIZES = {"1080p": (1920, 1080), "4k": (3840, 2160), "8k": (7680, 4320)}
SCALARS = {
    "bool": "?",
    "byte": "b",
    "ubyte": "B",
    "short": "h",
    "ushort": "H",
    "int": "i",
    "uint": "I",
    "long": "q",
    "ulong": "Q",
    "float": "f",
    "double": "d",
}
NUMPY = {"bool": np.bool_, "byte": np.int8, "ubyte": np.uint8, "short": np.int16, "ushort": np.uint16}
NUMPY |= {"int": np.int32, "uint": np.uint32, "long": np.int64, "ulong": np.uint64, "float": np.float32}
AV1_REFS = ["INTRA", "LAST", "LAST2", "LAST3", "GOLDEN", "BWDREF", "ALTREF2", "ALTREF"]

_COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)
_DECL = re.compile(r"\b(table|struct|enum)\s+(\w+)\s*(?::\s*(\w+))?\s*(?:\([^)]*\))?\s*\{([^}]*)\}", re.DOTALL)
_FIELD = re.compile(r"^\s*(\w+)\s*:\s*(\[?\w+\]?)\s*(?:=\s*([-\w.]+))?")


class Schema:
    """Tables, structs and enums of one .fbs file: enough to build buffers in declaration (slot) order."""

    def __init__(self, text: str) -> None:
        self.tables: dict[str, list[tuple[str, str, Any]]] = {}
        self.structs: dict[str, list[tuple[str, str]]] = {}
        self.enums: dict[str, tuple[str, dict[str, int]]] = {}
        for kind, name, base, body in _DECL.findall(_COMMENT.sub("", text)):
            if kind == "enum":
                values, nxt = {}, 0
                for item in (i.strip() for i in body.split(",")):
                    if item:
                        key, _, val = (p.strip() for p in item.partition("="))
                        nxt = int(val, 0) if val else nxt
                        values[key], nxt = nxt, nxt + 1
                self.enums[name] = (base, values)
                continue
            fields = [m.groups() for m in map(_FIELD.match, body.split(";")) if m]
            if kind == "struct":
                self.structs[name] = [(f, t) for f, t, _ in fields]
            else:
                self.tables[name] = [(f, t, d) for f, t, d in fields]

    def scalar(self, t: str) -> str | None:
        """The scalar type name of ``t`` (enums resolve to their base), or None."""
        if t in SCALARS:
            return t
        return self.enums[t][0] if t in self.enums else None

    def default(self, t: str, text: str | None) -> Any:
        if text is None:
            return False if t == "bool" else 0
        if text in ("true", "false"):
            return text == "true"
        if t in self.enums and not re.match(r"-?\d", text):
            return self.enums[t][1][text]
        return float(text) if "." in text else int(text, 0)

    def struct_layout(self, name: str) -> tuple[str, int]:
        """The ``struct`` format (with FlatBuffers padding) and the alignment of a struct of scalars."""
        fmt, size, align = "<", 0, 1
        for _, t in self.structs[name]:
            scalar = self.scalar(t)
            if scalar is None:
                raise ValueError(f"nested struct in {name} is not supported")
            width = struct.calcsize(SCALARS[scalar])
            pad = -size % width
            fmt, size, align = fmt + "x" * pad + SCALARS[scalar], size + pad + width, max(align, width)
        return fmt + "x" * (-size % align), align

    def struct_bytes(self, name: str, value: dict[str, Any] | tuple[Any, ...]) -> bytes:
        vals = value if isinstance(value, tuple) else tuple(value[f] for f, _ in self.structs[name])
        return struct.pack(self.struct_layout(name)[0], *vals)


class Writer:
    """Builds tables from plain dicts: ``{field: value}``; a missing field or a default value is not written."""

    def __init__(self, schema: Schema, initial: int = 1 << 20) -> None:
        self.s = schema
        self.b = flatbuffers.Builder(initial)

    def string(self, text: str) -> int:
        return int(self.b.CreateString(text))

    def vector(self, elem: str, values: Any) -> int:
        scalar = self.s.scalar(elem)
        if scalar is not None:
            return int(self.b.CreateNumpyVector(np.ascontiguousarray(values, dtype=NUMPY[scalar])))
        if elem in self.s.structs:
            data = b"".join(self.s.struct_bytes(elem, v) for v in values)
            size = len(data) // max(1, len(values))
            self.b.StartVector(size, len(values), self.s.struct_layout(elem)[1])
            self.b.head -= len(data)
            self.b.Bytes[self.b.head : self.b.head + len(data)] = data
            return int(self.b.EndVector())
        offsets = [
            v if isinstance(v, int) else (self.string(v) if elem == "string" else self.table(elem, v)) for v in values
        ]
        return self.offsets(offsets)

    def offsets(self, offsets: list[int]) -> int:
        self.b.StartVector(4, len(offsets), 4)
        for off in reversed(offsets):
            self.b.PrependUOffsetTRelative(off)
        return int(self.b.EndVector())

    def table(self, name: str, value: dict[str, Any]) -> int:
        fields = self.s.tables[name]
        refs: dict[int, int] = {}
        for slot, (field, t, _) in enumerate(fields):  # children first, in declaration order
            if field not in value or self.s.scalar(t) or t in self.s.structs:
                continue
            v = value[field]
            if t == "string":
                refs[slot] = v if isinstance(v, int) else self.string(v)
            elif t.startswith("["):
                refs[slot] = v if isinstance(v, int) else self.vector(t[1:-1], v)
            else:
                refs[slot] = v if isinstance(v, int) else self.table(t, v)
        self.b.StartObject(len(fields))
        for size in (8, 4, 2, 1):
            for slot in reversed(range(len(fields))):
                field, t, default = fields[slot]
                scalar = self.s.scalar(t)
                width = struct.calcsize(SCALARS[scalar]) if scalar else 4
                if width != size or field not in value:
                    continue
                if slot in refs:
                    self.b.PrependUOffsetTRelativeSlot(slot, refs[slot], 0)
                elif scalar:
                    v = value[field]
                    if v != self.s.default(t, default):
                        self.b.PrependSlot(_flags(scalar), slot, v, None)
                else:  # struct, written inline
                    data = self.s.struct_bytes(t, value[field])
                    self.b.Prep(self.s.struct_layout(t)[1], len(data))
                    self.b.head -= len(data)
                    self.b.Bytes[self.b.head : self.b.head + len(data)] = data
                    self.b.Slot(slot)
        return int(self.b.EndObject())

    def finish(self, root: int, identifier: bytes) -> bytes:
        self.b.Finish(root, identifier)
        return bytes(self.b.Output())


def _flags(scalar: str) -> Any:
    from flatbuffers import number_types as n

    return {
        "bool": n.BoolFlags,
        "byte": n.Int8Flags,
        "ubyte": n.Uint8Flags,
        "short": n.Int16Flags,
        "ushort": n.Uint16Flags,
        "int": n.Int32Flags,
        "uint": n.Uint32Flags,
        "long": n.Int64Flags,
        "ulong": n.Uint64Flags,
        "float": n.Float32Flags,
        "double": n.Float64Flags,
    }[scalar]


# ------------------------------------------------------------------------------------------ frame content


def load_schema() -> Schema:
    return Schema(SCHEMA.read_text(encoding="utf-8"))


def frame_info(s: Schema, w: int, h: int, inter: bool, qp_base: int, av1: bool) -> dict[str, Any]:
    ftype = ("Inter" if inter else "Key") if av1 else ("P" if inter else "I")
    return {
        "id": (1 if inter else 0, 1 if inter else 0),
        "type": s.enums["FrameType"][1][ftype],
        "shown": True,
        "is_rap": not inter,
        "poc": 1 if inter else 0,
        "pts": 1 if inter else 0,
        "dts": 1 if inter else 0,
        "timebase_num": 1,
        "timebase_den": 30,
        "coded_size": (0, 0, w, h),
        "crop": (0, 0, w, h),
        "chroma_format": 1,
        "bit_depth_luma": 8,
        "bit_depth_chroma": 8,
        "qp_base": qp_base,
        "slice_count": 1,
    }


def quadtree(x: int, y: int, size: int, target: int, w: int, h: int) -> Iterator[tuple[int, int, int]]:
    """CU rects of one CTU: split to ``target``, and further wherever a CU would cross the picture edge."""
    if x >= w or y >= h:
        return
    if size > target or (size > 8 and (x + size > w or y + size > h)):
        half = size // 2
        for dy in (0, half):
            for dx in (0, half):
                yield from quadtree(x + dx, y + dy, half, target, w, h)
    else:
        yield x, y, size


def build_grid(s: Schema, w: int, h: int) -> bytes:
    cw, ch = -(-w // 4), -(-h // 4)
    kinds = s.enums["BlockKind"][1]
    intra = s.enums["PredKind"][1]["Intra"]
    blocks: list[dict[str, Any]] = [{"id": 0, "kind": kinds["Other"], "rect": (0, 0, w, h)}]
    block_id = np.zeros((ch, cw), np.uint32)
    qp = np.zeros((ch, cw), np.int16)
    for cy in range(-(-h // 64)):
        for cx in range(-(-w // 64)):
            ctu = len(blocks)
            blocks.append(
                {"id": ctu, "parent": 0, "kind": kinds["CTU"], "rect": (cx * 64, cy * 64, 64, 64), "level": 1}
            )
            children = []
            for x, y, size in quadtree(cx * 64, cy * 64, 64, 64 >> ((cx + cy) % 4), w, h):
                cu, value = len(blocks), 22 + (x * 30) // w
                blocks.append(
                    {"id": cu, "parent": ctu, "kind": kinds["CU"], "rect": (x, y, size, size), "level": 2}
                    | {"pred": intra, "qp": value}
                )
                children.append(cu)
                block_id[y // 4 : (y + size) // 4, x // 4 : (x + size) // 4] = cu
                qp[y // 4 : (y + size) // 4, x // 4 : (x + size) // 4] = value
            blocks[ctu]["children"] = children
    blocks[0]["children"] = [b["id"] for b in blocks if b.get("level") == 1]
    pred = np.full(cw * ch, intra, np.uint8)
    grid = {"cells_w": cw, "cells_h": ch, "block_id": block_id.ravel(), "pred": pred, "qp": qp.ravel()}
    return finish(s, frame_info(s, w, h, False, 22, av1=False), grid, blocks, [], [])


def rotation(cw: int, ch: int) -> tuple[np.ndarray, np.ndarray]:
    """Per-cell MV of a rotation about the picture centre, 1/16 sample units, within ±32 samples."""
    y4, x4 = np.mgrid[0:ch, 0:cw].astype(np.int64)
    mv_x = np.clip(-(y4 * 2 - ch) * 2, -512, 512).astype(np.int16)
    mv_y = np.clip((x4 * 2 - cw) * 2, -512, 512).astype(np.int16)
    return mv_x, mv_y


def build_mv(s: Schema, w: int, h: int, dense: bool = False) -> bytes:
    """AV1 inter frame, SB 64, a 4×4 ``Block`` and one L0 MV per cell. ``dense`` adds the QP ramp and tx sizes."""
    cw, ch = -(-w // 4), -(-h // 4)
    kinds = s.enums["BlockKind"][1]
    inter = s.enums["PredKind"][1]["Inter"]
    mv_x, mv_y = rotation(cw, ch)
    qindex = ((np.arange(cw, dtype=np.int64) * 256) // cw).astype(np.int16)  # 0..255 left to right
    wr = Writer(s, 64 << 20)
    blocks: list[int] = []
    mvs: list[int] = []
    block_id = np.zeros((ch, cw), np.uint32)
    sb_rows, sb_cols = -(-h // 64), -(-w // 64)
    leaves = []  # (id, x4, y4) in pre-order
    plan: list[tuple[int, int, int, list[int]]] = []  # (sb id, sbx, sby, child ids)
    nxt = 1
    for sy in range(sb_rows):
        for sx in range(sb_cols):
            sb, nxt, kids = nxt, nxt + 1, []
            for y4 in range(sy * 16, min(ch, sy * 16 + 16)):
                for x4 in range(sx * 16, min(cw, sx * 16 + 16)):
                    kids.append(nxt)
                    leaves.append((nxt, x4, y4))
                    block_id[y4, x4] = nxt
                    nxt += 1
            plan.append((sb, sx, sy, kids))
    # Blocks are written in id order (pre-order), each table built just before its offset is needed.
    root = {"id": 0, "kind": kinds["Other"], "rect": (0, 0, w, h), "children": [p[0] for p in plan]}
    blocks.append(wr.table("Block", root))
    leaf = iter(leaves)
    for sb, sx, sy, kids in plan:
        blocks.append(
            wr.table(
                "Block",
                {"id": sb, "parent": 0, "children": kids, "kind": kinds["SB"], "rect": (sx * 64, sy * 64, 64, 64)}
                | {"level": 1},
            )
        )
        for _ in kids:
            bid, x4, y4 = next(leaf)
            fields = {"id": bid, "parent": sb, "kind": kinds["Block"], "rect": (x4 * 4, y4 * 4, 4, 4), "level": 2}
            fields["pred"] = inter
            if dense:
                fields["qp"] = int(qindex[x4])
            blocks.append(wr.table("Block", fields))
            mv = {"block_id": bid, "ref": 1, "rect": (x4 * 4, y4 * 4, 4, 4)}
            mvs.append(wr.table("MotionVector", mv | {"mv_x": int(mv_x[y4, x4]), "mv_y": int(mv_y[y4, x4])}))
    n = cw * ch
    grid: dict[str, Any] = {"cells_w": cw, "cells_h": ch, "block_id": block_id.ravel()}
    grid |= {"pred": np.full(n, inter, np.uint8), "mv0_x": mv_x.ravel(), "mv0_y": mv_y.ravel()}
    grid |= {"ref0": np.full(n, 1, np.int8), "ref1": np.full(n, -1, np.int8)}
    extra: dict[str, Any] = {}
    if dense:
        qp = np.broadcast_to(qindex, (ch, cw)).ravel()
        grid |= {"qp": qp, "tx_log2_w": np.full(n, 2, np.uint8), "tx_log2_h": np.full(n, 2, np.uint8)}
        extra["qp_hist"] = np.bincount(qp, minlength=256).astype(np.uint32)
    refs = [
        {"index": k, "frame": (-1, -1) if k == 0 else (0, 0), "name": name, "usage_cells": n if k == 1 else 0}
        | {"slot": -1 if k == 0 else k - 1}
        for k, name in enumerate(AV1_REFS)
    ]
    return finish(s, frame_info(s, w, h, True, 128, av1=True), grid, blocks, mvs, refs, wr, extra)


def finish(
    s: Schema,
    frame: dict[str, Any],
    grid: dict[str, Any],
    blocks: list[Any],
    mvs: list[int],
    refs: list[dict[str, Any]],
    wr: Writer | None = None,
    extra: dict[str, Any] | None = None,
) -> bytes:
    wr = wr or Writer(s, 16 << 20)
    blocks = [b if isinstance(b, int) else wr.table("Block", b) for b in blocks]
    root: dict[str, Any] = {"schema_version": 1, "plugin_id": "gen_fixture_fb", "plugin_version": "1"}
    root |= {"frame": wr.table("FrameInfo", frame), "grid": wr.table("CellGrid", grid), "blocks": wr.offsets(blocks)}
    if mvs:
        root["mvs"] = wr.offsets(mvs)
    if refs:
        root["refs"] = wr.vector("RefSlot", refs)
    root |= extra or {}
    return wr.finish(wr.table("FrameAnalysis", root), b"OVTA")


KINDS: dict[str, Callable[[Schema, int, int], bytes]] = {"grid": build_grid, "mv": build_mv}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--kind", choices=sorted(KINDS), required=True)
    ap.add_argument("--size", choices=sorted(SIZES), required=True)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args(argv)
    args.out.write_bytes(KINDS[args.kind](load_schema(), *SIZES[args.size]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
