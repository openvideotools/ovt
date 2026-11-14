#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate the elementary-stream unit goldens ``<id>.ovt-units.json`` (chapter 14 WI-1.570).

    python -I tools/goldens/gen_units.py FILE --codec h264|h265|av1 --framing annexb|obu|ivf|y4m   # one file to stdout
    python -I tools/goldens/gen_units.py --out tests/golden --corpora-dir DIR [--only ID[,ID...]]  # every entry

The oracle is FFmpeg: ``ffprobe -show_packets`` gives the position and size of every packet (an access unit of an
Annex B stream, a temporal unit of an OBU stream, a frame of an IVF or Y4M file); the ``trace_headers`` filter, run
with ``-loglevel repeat+trace``, logs the ``nal_unit_type`` of every NAL unit of each packet (H.264/HEVC only). Compare
per packet, not per NAL unit: FFmpeg's parsers merge AUD, SEI and parameter sets into the following slice's packet.
Per-NAL sizes are not oracle output; ``tools/ref_annexb.py`` covers them. Golden format: one packet per line,
``{"pos":0,"size":1500,"types":[7,8,6,5]}``; ``pos`` is absent when ffprobe has none, ``types`` for AV1 and Y4M. The IVF
position is the frame header (payload 12 bytes later); an Annex B packet includes its start codes. Self check: for
Annex B H.264/HEVC the per-packet NAL types must equal ``tools/ref_annexb.py``'s, else the entry fails.

Exit status: 0 ok, 1 a step failed, 2 usage, 4 a tool is missing. In bulk mode a failing entry is listed in
``units-index.json`` and the status stays 0 unless ``--strict``.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(REPO / "tools"))
import ref_annexb

MAX_BYTES = 1_900_000  # CLAUDE.md caps goldens at 2 MB gzip-compressed; the plain JSON stays below that
EXIT_OK, EXIT_STEP, EXIT_USAGE, EXIT_MISSING = 0, 1, 2, 4
PROBE = ["-probesize", "50M", "-analyzeduration", "50M"]
FRAMINGS = ("annexb", "obu", "ivf", "y4m")
CODECS = ("h264", "h265", "av1", "rawvideo")
TRACE_PACKET = re.compile(r"^\[trace_headers @ [0-9a-fx]+\] Packet: (\d+) bytes")
TRACE_NAL = re.compile(r"^\[trace_headers @ [0-9a-fx]+\]\s+\d+\s+nal_unit_type\s+[01]+ = (\d+)$")


class StepError(Exception):
    """One oracle step of one entry failed."""


def tool(name: str) -> str:
    path = os.environ.get(f"OVT_ORACLE_{name.upper()}") or shutil.which(name)
    if not path:
        print(f"gen_units.py: {name} not found (set $OVT_ORACLE_{name.upper()})", file=sys.stderr)
        raise SystemExit(EXIT_MISSING)
    return path


def run(cmd: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, capture_output=True, text=True, check=False, encoding="utf-8", errors="replace")


def ffprobe_packets(path: Path) -> list[dict[str, Any]]:
    """[{pos?, size}] of the first video stream."""
    cmd = [
        tool("ffprobe"),
        "-v",
        "error",
        *PROBE,
        "-select_streams",
        "v:0",
        "-show_entries",
        "packet=pos,size",
        "-of",
        "json",
        str(path),
    ]
    res = run(cmd)
    if res.returncode != 0:
        raise StepError(f"ffprobe failed: {res.stderr.strip()[:300]}")
    out = []
    for pkt in json.loads(res.stdout).get("packets", []):
        entry: dict[str, Any] = {}
        if str(pkt.get("pos", "N/A")).isdigit():
            entry["pos"] = int(pkt["pos"])
        entry["size"] = int(pkt["size"])
        out.append(entry)
    return out


def parse_trace(log: str) -> list[tuple[int, list[int]]]:
    """[(packet size, [nal_unit_type, ...])] from the ``-v trace`` log of the trace_headers filter. Lines before the
    first ``Packet:`` (the extradata) and lines of other FFmpeg components (the parsers log NAL types too) are ignored;
    only lines of the trace_headers instance count."""
    packets: list[tuple[int, list[int]]] = []
    for line in log.splitlines():
        # ffmpeg's progress line is written without a newline and glues itself to the next log line
        line = line[line.rfind("[trace_headers @") :] if "[trace_headers @" in line else line
        m = TRACE_PACKET.match(line)
        if m:
            packets.append((int(m.group(1)), []))
            continue
        m = TRACE_NAL.match(line)
        if m and packets:
            packets[-1][1].append(int(m.group(1)))
    return packets


def ffmpeg_trace(path: Path) -> list[tuple[int, list[int]]]:
    cmd = [
        tool("ffmpeg"),
        "-nostdin",
        "-loglevel",
        "repeat+trace",
        *PROBE,
        "-i",
        str(path),
        "-map",
        "0:v:0",
        "-c",
        "copy",
        "-bsf:v",
        "trace_headers",
        "-f",
        "null",
        "-",
    ]
    res = run(cmd)
    if res.returncode != 0:
        tail = re.sub(r"0x[0-9a-f]+", "0x0", res.stderr.strip()[-300:])  # addresses differ between runs
        raise StepError(f"ffmpeg trace_headers failed: {tail}")
    return parse_trace(res.stderr)


def build(path: Path, entry_id: str, codec: str, framing: str) -> dict[str, Any]:
    if codec not in CODECS or framing not in FRAMINGS:
        raise StepError(f"unsupported codec/framing {codec}/{framing}")
    packets = ffprobe_packets(path)
    if not packets:
        raise StepError("ffprobe reports no packets")
    if codec in ("h264", "h265") and framing == "annexb":
        traced = ffmpeg_trace(path)
        sizes = [p["size"] for p in packets]
        if [s for s, _ in traced] != sizes:
            raise StepError(f"trace_headers saw {len(traced)} packets, ffprobe {len(sizes)}, or their sizes differ")
        for pkt, (_, types) in zip(packets, traced):
            pkt["types"] = types
        reference = [ref_annexb.nal_type(h, codec) for _, _, h in ref_annexb.split(path.read_bytes())]
        flat = [t for _, types in traced for t in types]
        if flat != reference:
            raise StepError(f"NAL unit types disagree with tools/ref_annexb.py: {len(flat)} vs {len(reference)} units")
    return {"id": entry_id, "codec": codec, "framing": framing, "packets": packets}


def render(doc: dict[str, Any]) -> str:
    head = {
        "family": "units",
        "id": doc["id"],
        "codec": doc["codec"],
        "framing": doc["framing"],
        "oracle": "ffprobe+trace_headers",
    }
    text = json.dumps(head, separators=(",", ":"))[:-1] + ',"packets":[\n'
    text += ",\n".join(json.dumps(p, separators=(",", ":")) for p in doc["packets"])
    text += "\n]}\n"
    if len(text.encode()) > MAX_BYTES:
        raise StepError(f"golden would be {len(text)} bytes (cap {MAX_BYTES})")
    return text


def classify(entry: dict[str, Any]) -> tuple[str, str] | None:
    """(codec, framing) of a manifest entry that has a unit golden, else None."""
    containers = entry.get("containers") or []
    codecs = entry.get("codecs") or []
    if "annexb" in containers and codecs and codecs[0] in ("h264", "h265"):
        return codecs[0], "annexb"
    if "ivf" in containers and codecs and codecs[0] == "av1":
        return "av1", "ivf"
    if "annexb" in containers and codecs and codecs[0] == "av1":
        return "av1", "obu"
    return None


def corpus_file(corpora_dir: Path, entry_id: str) -> Path | None:
    ok = corpora_dir / entry_id / ".ok"
    if not ok.is_file():
        return None
    try:
        primary = json.loads(ok.read_text()).get("primary")
    except (OSError, ValueError):
        primary = None
    if primary:
        return corpora_dir / entry_id / primary
    files = [p for p in (corpora_dir / entry_id).iterdir() if p.is_file() and p.name != ".ok"]
    return files[0] if len(files) == 1 else None


def bulk(args: argparse.Namespace) -> int:
    manifest = json.loads(args.manifest.read_text())
    only = set(args.only.split(",")) if args.only else None
    written: list[str] = []
    failed: dict[str, str] = {}
    skipped: dict[str, str] = {}
    for entry in manifest["entries"]:
        kind = classify(entry)
        if kind is None:
            continue
        if only is not None:
            if entry["id"] not in only:
                continue
        elif "fast" not in entry.get("tags", []) and entry["id"] not in EXTRA_IDS:
            continue
        if entry.get("unverified") and only is None:
            skipped[entry["id"]] = "no recorded hash: tools/fetch_corpora.py cannot fetch it"
            continue
        path = corpus_file(args.corpora_dir, entry["id"])
        if path is None:
            failed[entry["id"]] = "corpus file not fetched"
            continue
        try:
            text = render(build(path, entry["id"], *kind))
        except StepError as e:
            failed[entry["id"]] = str(e)
            continue
        target = args.out / kind[0] / f"{entry['id']}.ovt-units.json"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8", newline="\n")
        written.append(f"{kind[0]}/{entry['id']}.ovt-units.json")
    index = {
        "written": sorted(written),
        "failed": dict(sorted(failed.items())),
        "skipped": dict(sorted(skipped.items())),
    }
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "units-index.json").write_text(json.dumps(index, indent=1) + "\n", encoding="utf-8", newline="\n")
    for cid, why in failed.items():
        print(f"gen_units.py: {cid}: {why}", file=sys.stderr)
    print(f"gen_units.py: wrote {len(written)} goldens, {len(failed)} failed, {len(skipped)} skipped", file=sys.stderr)
    return EXIT_STEP if failed and args.strict else EXIT_OK


# Entries outside the `fast` tag that WI-1.570 acceptance test 1 names.
EXTRA_IDS = {"jvt_CABA3_TOSHIBA_E"}


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("file", nargs="?", type=Path, help="one stream: print its golden to stdout")
    p.add_argument("--codec", choices=CODECS)
    p.add_argument("--framing", choices=FRAMINGS)
    p.add_argument("--id", default="stdin")
    p.add_argument("--manifest", type=Path, default=REPO / "tests" / "corpora" / "manifest.json")
    p.add_argument("--out", type=Path, default=REPO / "tests" / "golden")
    p.add_argument("--corpora-dir", type=Path)
    p.add_argument("--only", help="comma-separated corpus ids")
    p.add_argument(
        "--strict", action="store_true", help="exit 1 when an entry failed (default: list it in units-index.json)"
    )
    args = p.parse_args(argv)
    if args.file is not None:
        if not args.codec or not args.framing:
            p.error("--codec and --framing are required with FILE")
        try:
            sys.stdout.write(render(build(args.file, args.id, args.codec, args.framing)))
        except StepError as e:
            print(f"gen_units.py: {e}", file=sys.stderr)
            return EXIT_STEP
        return EXIT_OK
    if args.corpora_dir is None:
        p.error("FILE or --corpora-dir is required")
    return bulk(args)


if __name__ == "__main__":
    sys.exit(main())
