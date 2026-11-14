#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate the AV1 oracle goldens from libaom ``inspect`` (chapter 14 §14.4.5, WI-0.566).

    python -I tools/goldens/gen_av1.py --manifest tests/corpora/manifest.json --out tests/golden/ [--only ID[,ID...]]
        [--corpora-dir DIR] [--generate-syn] [--jobs N] [--work-dir DIR] [--max-bytes N] [--max-tus N]
        [--keep-going] [--list]

Runs inside the oracle image (chapter 14 §14.2.6; ``inspect``, ``aomdec`` and ``ffmpeg`` on ``PATH``, each
overridable with ``$OVT_ORACLE_<NAME>``). For every ``fast`` ``aom`` manifest entry and every verified ``syn_av1_*``
entry it writes ``av1/<id>.aom-inspect.json.gz`` and ``av1-index.json`` below ``--out``:

1. the stream becomes an IVF file (an IVF file is used as it is; Matroska and OBU files are copied with
   ``ffmpeg -c copy -f ivf``, §14.4.5);
2. the IVF is cut after ``K`` temporal units, starting with ``--max-tus`` and halving ``K`` until the normalised
   golden is at most ``--max-bytes`` gzip-compressed (CLAUDE.md section 5: no golden over 2 MB as committed) and
   ``--max-raw-bytes`` of JSON. A stream whose first temporal
   unit alone is larger is not a failure: it is left out and the reason is recorded in the index;
3. ``inspect -A -a`` (``-x`` stays off) runs on the cut file and ``normalize/aom_inspect.py`` writes the
   ``block-grid`` + ``symbols`` document, stored gzip-compressed without name or time (reproducible bytes);
4. ``av1_selfcheck.py`` compares the golden with ``aomdec --summary`` on the same cut file.

Exit status: 0 all done, 1 a step failed (unless listed in ``av1_skips.json`` or ``--keep-going``), 2 usage,
4 a tool or corpus is missing.
"""

from __future__ import annotations

import argparse
import gzip
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import av1_selfcheck  # noqa: E402
import gen_h26x as common  # noqa: E402

StepError = common.StepError


class TooLarge(StepError):
    """Even one temporal unit exceeds the size limit: not a failure, the golden is left out."""


MAX_BYTES = 2_000_000  # gzip-compressed golden as committed: the CLAUDE.md rule (section 5)
MAX_RAW_BYTES = 40_000_000  # normalised JSON: bounds the memory of the tests and tools that load a golden
MAX_TUS = 8
IVF_HEADER = 32
EXIT_OK, EXIT_STEP, EXIT_USAGE, EXIT_MISSING = 0, 1, 2, 4
SYN_PREFIX = "syn_av1_"


# ------------------------------------------------------------------------------------------------ selection


def select(manifest: dict[str, Any], only: list[str] | None) -> tuple[list[dict[str, Any]], dict[str, str]]:
    """The entries to process and the ones skipped (id -> reason)."""
    chosen, skipped = [], {}
    for entry in manifest["entries"]:
        cid = entry["id"]
        if only is not None:
            if cid in only:
                chosen.append(entry)
            continue
        if entry.get("source") == "aom":
            if "fast" not in entry.get("tags", []):
                continue
            if entry.get("unverified"):
                skipped[cid] = "no recorded hash: tools/fetch_corpora.py cannot fetch it"
            else:
                chosen.append(entry)
        elif cid.startswith(SYN_PREFIX):
            if entry.get("unverified") or entry.get("local_only"):
                skipped[cid] = "local_only perf stream: not generated in CI"
            else:
                chosen.append(entry)
    if only is not None:
        missing = set(only) - {e["id"] for e in chosen}
        if missing:
            raise StepError(f"not manifest entries: {', '.join(sorted(missing))}")
    return chosen, skipped


# ------------------------------------------------------------------------------------------------ IVF


def to_ivf(src: Path, work: Path) -> Path:
    """``src`` as IVF: itself, or its first video stream copied (Matroska, Section 5 / Annex B OBU files)."""
    with src.open("rb") as f:
        if f.read(4) == b"DKIF":
            return src
    out = work / "full.ivf"
    common.run(
        [common.tool("ffmpeg"), "-hide_banner", "-v", "error", "-y", *common.PROBE, "-i", str(src), "-map", "0:v:0",
         "-c:v", "copy", "-f", "ivf", str(out)],
        work, "ffmpeg (to IVF)",
    )  # fmt: skip
    return out


def ivf_frames(data: bytes) -> list[tuple[int, int]]:
    """``(start, end)`` of every frame record (12-byte header + payload) of an IVF file."""
    if data[:4] != b"DKIF":
        raise StepError("not an IVF file")
    pos, frames = IVF_HEADER, []
    while pos + 12 <= len(data):
        size = struct.unpack_from("<I", data, pos)[0]
        if pos + 12 + size > len(data):
            break
        frames.append((pos, pos + 12 + size))
        pos += 12 + size
    return frames


def ivf_prefix(data: bytes, frames: list[tuple[int, int]], count: int) -> bytes:
    """The IVF header (frame count patched) and the first ``count`` frame records."""
    head = bytearray(data[:IVF_HEADER])
    struct.pack_into("<I", head, 24, count)
    return bytes(head) + data[IVF_HEADER : frames[count - 1][1]]


# ------------------------------------------------------------------------------------------------ oracle


def gz_bytes(text: str) -> bytes:
    """What ``common.write_gz`` writes: gzip level 9 without name or time."""
    buf = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=buf, compresslevel=9, mtime=0) as f:
        f.write(text.encode("utf-8"))
    return buf.getvalue()


def produce(
    ivf: Path, work: Path, max_bytes: int, max_raw_bytes: int, max_tus: int
) -> tuple[str, int, tuple[int, int]]:  # (golden text, temporal units, (decoded, showed))
    data = ivf.read_bytes()
    frames = ivf_frames(data)
    if not frames:
        raise StepError("the IVF file has no frame")
    count = min(max_tus, len(frames))
    while True:
        cut = work / f"cut{count}.ivf"
        cut.write_bytes(ivf_prefix(data, frames, count))
        raw = work / f"inspect{count}.json"
        with raw.open("w", encoding="utf-8") as out:
            common.run([common.tool("inspect"), "-A", "-a", str(cut)], work, "inspect", stdout=out)
        text = common.normalise("aom_inspect.py", raw, [], work)
        raw.unlink()
        packed = len(gz_bytes(text))
        if len(text.encode()) <= max_raw_bytes and packed <= max_bytes:
            return text, count, summary_of(cut, work)
        if count == 1:
            raise TooLarge(
                f"the golden of one temporal unit is {len(text.encode())} bytes, {packed} gzip-compressed "
                f"(limits {max_raw_bytes} and {max_bytes})"
            )
        count = max(1, count // 2)


def summary_of(cut: Path, work: Path) -> tuple[int, int]:
    """``aomdec --summary`` counts; ``--all-layers`` because ``inspect`` lists the frame of every spatial layer."""
    res = subprocess.run(  # noqa: S603
        [common.tool("aomdec"), "--all-layers", "--summary", "--rawvideo", "-o", os.devnull, str(cut)],
        cwd=work, capture_output=True, text=True, check=False, stdin=subprocess.DEVNULL,
    )  # fmt: skip
    if res.returncode != 0:
        raise StepError(f"aomdec exited {res.returncode}: {res.stderr.strip()[-300:]}")
    try:
        return av1_selfcheck.parse_summary(res.stdout + res.stderr)
    except ValueError as exc:
        raise StepError(str(exc)) from exc


def process(entry: dict[str, Any], args: argparse.Namespace, skips: dict[str, str], work_root: Path) -> dict[str, Any]:
    cid = entry["id"]
    work = work_root / cid
    work.mkdir(parents=True, exist_ok=True)
    record: dict[str, Any] = {"goldens": {}, "skipped": {}, "failed": {}}
    src = common.corpus_file(args.corpora_dir, cid)
    record["input_sha256"] = common.sha256_file(src)
    try:
        text, tus, (decoded, showed) = produce(
            to_ivf(src, work), work, args.max_bytes, args.max_raw_bytes, args.max_tus
        )
    except TooLarge as exc:
        record["skipped"]["aom-inspect"] = str(exc)
        return record
    except StepError as exc:
        key = f"{cid}:aom-inspect"
        if key in skips:
            record["skipped"]["aom-inspect"] = skips[key]
        else:
            record["failed"]["aom-inspect"] = str(exc)
        return record
    name = f"{cid}.aom-inspect.json.gz"
    (args.out / "av1").mkdir(parents=True, exist_ok=True)
    common.write_gz(args.out / "av1" / name, text)
    report = av1_selfcheck.check(json.loads(text), tus, decoded, showed)
    problems = report.pop("problems")
    record["goldens"]["aom-inspect"] = {"file": f"av1/{name}", "temporal_units": tus, **report}
    if problems:
        if f"{cid}:selfcheck" in skips:
            record["selfcheck_known"] = {"reason": skips[f"{cid}:selfcheck"], "problems": problems}
        else:
            record["selfcheck_problems"] = problems
    shutil.rmtree(work, ignore_errors=True)
    return record


# ------------------------------------------------------------------------------------------------ main


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--manifest", type=Path, default=REPO / "tests" / "corpora" / "manifest.json")
    p.add_argument("--out", type=Path, default=REPO / "tests" / "golden")
    p.add_argument("--only", help="comma-separated corpus ids (any manifest entry)")
    p.add_argument("--corpora-dir", type=Path, default=Path(os.environ.get("OVT_CORPORA_DIR", "corpora")))
    p.add_argument("--generate-syn", action="store_true", help="run generate.py for missing syn_ ids")
    p.add_argument("--jobs", type=int, default=4)
    p.add_argument("--work-dir", type=Path)
    p.add_argument(
        "--max-bytes", type=int, default=MAX_BYTES, help="largest gzip-compressed golden (default %(default)s)"
    )
    p.add_argument(
        "--max-raw-bytes", type=int, default=MAX_RAW_BYTES, help="largest normalised JSON (default %(default)s)"
    )
    p.add_argument("--max-tus", type=int, default=MAX_TUS, help="most temporal units per golden (default %(default)s)")
    p.add_argument("--skips", type=Path, help="av1_skips.json (default: next to this script)")
    p.add_argument("--keep-going", action="store_true", help="exit 0 even when a step failed (CI report mode)")
    p.add_argument("--list", action="store_true", help="print the plan and exit")
    args = p.parse_args(argv)
    args.only = [i for i in args.only.split(",") if i] if args.only else None
    args.out = args.out.resolve()
    args.corpora_dir = args.corpora_dir.resolve()
    args.skips = args.skips or HERE / "av1_skips.json"
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    try:
        entries, skipped = select(manifest, args.only)
    except StepError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return EXIT_USAGE
    skips = json.loads(args.skips.read_text(encoding="utf-8")) if args.skips.is_file() else {}
    if args.list:
        for e in entries:
            print(e["id"])
        for cid, reason in skipped.items():
            print(f"{cid} skipped: {reason}")
        return EXIT_OK
    if args.generate_syn:
        wanted = [
            e["id"]
            for e in entries
            if e["id"].startswith("syn_") and not (args.corpora_dir / e["id"] / ".ok").is_file()
        ]
        if wanted:
            common.run([sys.executable, "-I", str(common.SYNTH), "--only", ",".join(wanted), "--out",
                        str(args.corpora_dir)], REPO, "generate.py", stdout=sys.stderr)  # fmt: skip
    missing = sorted(t for t in ("inspect", "aomdec", "ffmpeg") if not common._have(t))
    if missing:
        print(f"error: not on PATH (run this in the oracle image): {', '.join(missing)}", file=sys.stderr)
        return EXIT_MISSING
    args.out.mkdir(parents=True, exist_ok=True)
    work_root = args.work_dir or Path(tempfile.mkdtemp(prefix="gen_av1_"))
    results: dict[str, dict[str, Any]] = {}

    def one(entry: dict[str, Any]) -> tuple[str, dict[str, Any]]:
        try:
            return entry["id"], process(entry, args, skips, work_root)
        except (StepError, OSError) as exc:
            reason = skips.get(f"{entry['id']}:entry")
            if reason:
                return entry["id"], {"failed": {}, "goldens": {}, "skipped": {"entry": reason}}
            return entry["id"], {"failed": {"entry": str(exc)}, "goldens": {}, "skipped": {}}

    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for cid, record in pool.map(one, entries):
            results[cid] = record
            status = "FAILED " + ",".join(record["failed"]) if record["failed"] else "ok"
            print(f"{cid}: {status} ({len(record['goldens'])} goldens)", file=sys.stderr)
    index_path = args.out / "av1-index.json"
    index: dict[str, Any] = {}
    if args.only and index_path.is_file():
        index = json.loads(index_path.read_text(encoding="utf-8")).get("entries", {})
    index.update(results)
    doc = {
        "max_bytes": args.max_bytes,
        "max_raw_bytes": args.max_raw_bytes,
        "skipped_entries": skipped,
        "entries": dict(sorted(index.items())),
    }
    index_path.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    bad = {c: r for c, r in results.items() if r["failed"] or r.get("selfcheck_problems")}
    for cid, r in bad.items():
        for step, why in r["failed"].items():
            print(f"FAILED {cid} {step}: {why}", file=sys.stderr)
        for line in r.get("selfcheck_problems", []):
            print(f"DISAGREEMENT {cid}: {line}", file=sys.stderr)
    if args.work_dir is None:
        shutil.rmtree(work_root, ignore_errors=True)
    return EXIT_OK if args.keep_going or not bad else EXIT_STEP


if __name__ == "__main__":
    sys.exit(main())
