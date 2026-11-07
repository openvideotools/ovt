#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate and self-check the ISOBMFF oracle goldens (chapter 14 §14.4.6-§14.4.9, WI-0.564).

    python -I tools/goldens/gen_isobmff.py generate [--manifest M] [--cache-dir D] [--out tests/golden/isobmff]
                                                    [--only ID ...]
    python -I tools/goldens/gen_isobmff.py check    [--out tests/golden/isobmff]

``generate`` runs the five oracles on every ``fast`` ISOBMFF corpus file found in the corpus cache (fetch them
first with ``tools/fetch_corpora.py --tags fast``) and writes, per corpus id, the canonical JSON of each:

    <id>.mp4box-diso.json      MP4Box -diso -std       box-tree
    <id>.mp4box-dts.json       MP4Box -dts -std        sample-table
    <id>.mp4ff-info.json       mp4ff-info -l all:1     box-tree
    <id>.mediainfo-trace.json  mediainfo --Details=1   box-tree (offsets, header_length)
    <id>.mp4boxjs.json         mp4boxjs_dump.mjs       box-tree (offsets, header_length)

and ``ORACLES.json`` with the oracle versions. Run it inside the oracle image (it needs ``MP4Box``,
``mp4ff-info``, ``mediainfo`` and ``node`` with mp4box.js on ``NODE_PATH``).

``check`` needs no oracle tool: it loads the committed goldens and verifies the oracle self-consistency of
acceptance test 1 of WI-0.564: on ``path`` and ``size`` the four box-tree goldens agree, on ``offset`` and
``header_length`` MediaInfo and mp4box.js agree, and every ``-dts`` sample lies inside an ``mdat``. A
disagreement passes only when ``KNOWN_DIFFS.md`` lists it, and a ``KNOWN_DIFFS.md`` row nothing triggers
fails (so the file cannot rot). Exit: 0 consistent, 1 disagreement, 2 usage or missing input.

``KNOWN_DIFFS.md`` rows (a GitHub table; other lines are prose): ``| id | kind | path | reason |``, ``id`` and
``path`` are fnmatch patterns, ``kind`` one of ``no-golden:<oracle>`` (the oracle refused the file), ``only-in:<oracle>``, ``missing-in:<oracle>``, ``size``,
``offset``, ``header_length``, ``sample-outside-mdat``.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
NORMALIZE = ROOT / "tests" / "oracles" / "normalize"
DUMP = ROOT / "tests" / "oracles" / "mp4boxjs_dump.mjs"

BOX_ORACLES = ("mp4box-diso", "mp4ff-info", "mediainfo-trace", "mp4boxjs")
ALL_ORACLES = ("mp4box-diso", "mp4box-dts", "mp4ff-info", "mediainfo-trace", "mp4boxjs")
OFFSET_ORACLES = ("mediainfo-trace", "mp4boxjs")
# Goldens above this size are not committed (CLAUDE.md §5); the fast set must stay well below it.
MAX_GOLDEN_BYTES = 2_000_000


class ToolError(Exception):
    pass


def run(
    cmd: list[str], *, stdout_to: Path | None = None, env: dict[str, str] | None = None
) -> str:
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False, env=env)
    if proc.returncode != 0:
        raise ToolError(
            f"{cmd[0]} exited {proc.returncode}: {proc.stderr.strip()[:300]}"
        )
    if stdout_to is not None:
        stdout_to.write_text(proc.stdout, encoding="utf-8")
    return proc.stdout


def normalise(script: str, raw: Path, *extra: str) -> str:
    return run(
        [sys.executable, "-I", str(NORMALIZE / f"{script}.py"), str(raw), *extra]
    )


def versions() -> dict[str, str]:
    """The oracle versions in use (the goldens carry them in ORACLES.json)."""
    out: dict[str, str] = {}
    gpac = subprocess.run(
        ["MP4Box", "-version"], capture_output=True, text=True, check=False
    )
    m = re.search(r"GPAC version (\S+)", gpac.stdout + gpac.stderr)
    out["MP4Box"] = m.group(1) if m else "unknown"
    out["mediainfo"] = run(["mediainfo", "--Version"]).strip().splitlines()[-1]
    exe = shutil.which("mp4ff-info") or "mp4ff-info"
    info = subprocess.run(
        ["go", "version", "-m", exe], capture_output=True, text=True, check=False
    )
    m = re.search(r"mod\s+github.com/Eyevinn/mp4ff\s+(\S+)", info.stdout)
    out["mp4ff-info"] = m.group(1) if m else "unknown"
    pkg = next(
        (
            Path(p) / "mp4box" / "package.json"
            for p in os.environ.get("NODE_PATH", "").split(os.pathsep)
            if (Path(p) / "mp4box" / "package.json").exists()
        ),
        None,
    )
    out["mp4box.js"] = (
        json.loads(pkg.read_text(encoding="utf-8"))["version"] if pkg else "unknown"
    )
    out["node"] = run(["node", "--version"]).strip()
    return out


def corpus_file(cache: Path, entry: dict[str, Any]) -> Path | None:
    folder = cache / entry["id"]
    if not (folder / ".ok").exists():
        return None
    files = sorted(
        p for p in folder.iterdir() if p.is_file() and not p.name.startswith(".")
    )
    return files[0] if len(files) == 1 else None


def write(path: Path, text: str) -> None:
    if len(text.encode("utf-8")) > MAX_GOLDEN_BYTES:
        raise ToolError(
            f"{path.name}: {len(text)} bytes is over the {MAX_GOLDEN_BYTES} byte golden limit"
        )
    path.write_text(text, encoding="utf-8")


def generate_one(cid: str, src: Path, out: Path, work: Path) -> list[str]:
    """Run the five oracles; return the oracles that refused the file (their golden is then absent)."""
    steps = [
        (
            "mp4box-diso",
            "diso.xml",
            ["MP4Box", "-diso", "-std", str(src)],
            "mp4box_diso",
        ),
        ("mp4box-dts", "dts.txt", ["MP4Box", "-dts", "-std", str(src)], "mp4box_dts"),
        ("mp4ff-info", "ff.txt", ["mp4ff-info", "-l", "all:1", str(src)], "mp4ff_info"),
        (
            "mediainfo-trace",
            "mi.xml",
            ["mediainfo", "--Details=1", "--Output=XML", str(src)],
            "mediainfo_trace",
        ),
        ("mp4boxjs", "raw.json", ["node", str(DUMP), str(src)], "mp4boxjs"),
    ]
    refused: list[str] = []
    for oracle, name, cmd, script in steps:
        target = out / f"{cid}.{oracle}.json"
        target.unlink(missing_ok=True)
        try:
            run(cmd, stdout_to=work / name)
        except ToolError as exc:
            print(f"note {cid}: {oracle} refused the file: {exc}", file=sys.stderr)
            refused.append(oracle)
            continue
        write(target, normalise(script, work / name))
    return refused


def cmd_generate(args: argparse.Namespace) -> int:
    # SPEC-ASSUMPTION(#226): the WI names `gpac_counter...frag.mp4`, which is not in the manifest; every fetchable fast file is used.
    manifest = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
    wanted = [
        e
        for e in manifest["entries"]
        if "isobmff" in e.get("containers", [])
        and "fast" in e.get("tags", [])
        and (not args.only or e["id"] in args.only)
    ]
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    done: list[str] = []
    refused: list[tuple[str, str]] = []
    for entry in wanted:
        src = corpus_file(Path(args.cache_dir), entry)
        if src is None:
            print(f"skip {entry['id']}: not in the corpus cache", file=sys.stderr)
            continue
        try:
            with tempfile.TemporaryDirectory() as tmp:
                refused += [
                    (entry["id"], o)
                    for o in generate_one(entry["id"], src, out, Path(tmp))
                ]
        except ToolError as exc:
            print(f"error: {entry['id']}: {exc}", file=sys.stderr)
            return 2
        done.append(entry["id"])
        print(f"ok   {entry['id']}")
    if not done:
        print("error: no corpus file generated", file=sys.stderr)
        return 2
    (out / "ORACLES.json").write_text(
        json.dumps(
            {
                "versions": versions(),
                "ids": sorted(done),
                "refused": [f"{i}:{o}" for i, o in refused],
            },
            indent=1,
        )
        + "\n",
        encoding="utf-8",
    )
    return 0


# -- self-consistency ------------------------------------------------------------------------------------


def known_diffs(path: Path) -> list[tuple[str, str, str]]:
    rows: list[tuple[str, str, str]] = []
    if not path.exists():
        return rows
    for line in path.read_text(encoding="utf-8").splitlines():
        cells = [c.strip().strip("`") for c in line.strip().strip("|").split("|")]
        if (
            line.lstrip().startswith("|")
            and len(cells) == 4
            and cells[1] not in ("kind", "")
            and not set(cells[0]) <= set("-: ")
        ):
            rows.append((cells[0], cells[1], cells[2]))
    return rows


def load_box_trees(out: Path, cid: str) -> dict[str, dict[str, dict[str, Any]]]:
    trees: dict[str, dict[str, dict[str, Any]]] = {}
    for oracle in BOX_ORACLES:
        if not (out / f"{cid}.{oracle}.json").exists():
            continue
        doc = json.loads((out / f"{cid}.{oracle}.json").read_text(encoding="utf-8"))
        trees[oracle] = {b["path"]: b for b in doc}
    return trees


def fix_fourcc(path: str) -> str:
    # MediaInfo prints a non-ASCII fourcc byte as "?" (e.g. "?too" for the 0xA9 "too" tag of iTunes metadata).
    return path.replace("?", "©")


def diffs_for(cid: str, out: Path) -> list[tuple[str, str, str]]:
    """(kind, path, detail) for every disagreement between the goldens of one corpus id."""
    trees = load_box_trees(out, cid)
    found: list[tuple[str, str, str]] = []
    for o in ALL_ORACLES:
        if not (out / f"{cid}.{o}.json").exists():
            found.append((f"no-golden:{o}", "-", "the oracle refused the file"))
    if "mediainfo-trace" in trees:
        trees["mediainfo-trace"] = {
            fix_fourcc(p): b for p, b in trees["mediainfo-trace"].items()
        }
    present = [o for o in BOX_ORACLES if o in trees]
    paths = sorted(set().union(*(set(t) for t in trees.values())))
    for path in paths:
        have = [o for o in present if path in trees[o]]
        for o in present:
            if path not in trees[o]:
                kind = "only-in:" + have[0] if len(have) == 1 else f"missing-in:{o}"
                if len(have) == 1 and o != next(x for x in present if x != have[0]):
                    continue  # reported once, from the lone oracle
                found.append((kind, path, f"seen by {','.join(have)}"))
        sizes = {o: trees[o][path]["size"] for o in have}
        if len(set(sizes.values())) > 1:
            found.append(("size", path, str(sizes)))
        offs = {
            o: trees[o][path].get("offset")
            for o in OFFSET_ORACLES
            if o in trees and path in trees[o]
        }
        if len(set(offs.values())) > 1:
            found.append(("offset", path, str(offs)))
        hdrs = {
            o: trees[o][path].get("header_length")
            for o in OFFSET_ORACLES
            if o in trees and path in trees[o]
        }
        if len(set(hdrs.values())) > 1:
            found.append(("header_length", path, str(hdrs)))
    if "mp4boxjs" not in trees or not (out / f"{cid}.mp4box-dts.json").exists():
        return found
    mdats = [
        (b["offset"] + b.get("header_length", 8), b["offset"] + b["size"])
        for p, b in trees["mp4boxjs"].items()
        if p.split("/")[-1].split("[")[0] == "mdat"
    ]
    samples = json.loads((out / f"{cid}.mp4box-dts.json").read_text(encoding="utf-8"))
    for s in samples:
        if not any(
            lo <= s["offset"] and s["offset"] + s["size"] <= hi for lo, hi in mdats
        ):
            found.append(
                (
                    "sample-outside-mdat",
                    f"track{s.get('track', '')}/sample{s['i']}",
                    f"offset={s['offset']} size={s['size']}",
                )
            )
    return found


def cmd_check(args: argparse.Namespace) -> int:
    out = Path(args.out)
    ids = sorted({p.name.split(".")[0] for p in out.glob("*.mp4boxjs.json")})
    if not ids:
        print(f"error: no goldens in {out}", file=sys.stderr)
        return 2
    rules = known_diffs(out / "KNOWN_DIFFS.md")
    used: set[int] = set()
    bad = 0
    for cid in ids:
        for kind, path, detail in diffs_for(cid, out):
            hit = next(
                (
                    n
                    for n, (i, k, p) in enumerate(rules)
                    if fnmatch.fnmatchcase(cid, i)
                    and k == kind
                    and fnmatch.fnmatchcase(path, p)
                ),
                None,
            )
            if hit is None:
                bad += 1
                print(f"undocumented: {cid} {kind} {path}: {detail}")
            else:
                used.add(hit)
    for n, (i, k, p) in enumerate(rules):
        if n not in used:
            bad += 1
            print(f"stale KNOWN_DIFFS row: {i} | {k} | {p} matches no disagreement")
    print(f"{len(ids)} corpus ids, {bad} problem(s)")
    return 1 if bad else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    gen = sub.add_parser("generate")
    gen.add_argument(
        "--manifest", default=str(ROOT / "tests" / "corpora" / "manifest.json")
    )
    gen.add_argument(
        "--cache-dir",
        default=os.environ.get("OVT_CORPUS_CACHE", str(ROOT / ".corpus-cache")),
    )
    gen.add_argument("--out", default=str(ROOT / "tests" / "golden" / "isobmff"))
    gen.add_argument("--only", nargs="*")
    gen.set_defaults(func=cmd_generate)
    chk = sub.add_parser("check")
    chk.add_argument("--out", default=str(ROOT / "tests" / "golden" / "isobmff"))
    chk.set_defaults(func=cmd_check)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
