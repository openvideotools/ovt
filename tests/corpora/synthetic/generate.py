#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""OVT synthetic stream generator (chapter 14 §14.3.3; WI-0.553).

    generate.py [--only ID[,ID...]] [--out DIR] [--check] [--fixtures] [--local-only]
                [--update-manifest] [--sync-manifest] [--recipes FILE] [--manifest FILE] [--list]

Runs the recipes of ``recipes.json`` with the pinned encoders of the oracle image and stores every corpus id
in the fetcher's cache layout, ``<out>/<id>/<files>`` plus ``<out>/<id>/.ok``, so tests find the files with
``ovt::test::corpus_path(id)`` exactly as if ``tools/fetch_corpora.py`` had downloaded them. ``--out``
defaults to the fetcher's cache dir (``$OVT_CORPORA_DIR`` or the platform cache root).

Every command is an argv list (no shell). Placeholders: ``{python}`` (this interpreter), ``{repo}``,
``{hm_cfg}`` / ``{jm_cfg}`` (HM and JM encoder config dirs of the oracle image, overridable with
``$OVT_HM_CFG_DIR`` / ``$OVT_JM_CFG_DIR``), ``{corpus:ID}`` (a fetched or generated corpus file),
``{glob:PATTERN}`` (the matching work-dir files in natural order, one argument each) and ``{out}`` (fixture
path). Intermediates live in ``<out>/.work``. Every step runs with ``SOURCE_DATE_EPOCH=0``, ``TZ=UTC`` and
stdin closed.

Before anything is written, the version probe of every tool the selected recipes use is compared with the
``pins`` of ``recipes.json``; a difference exits 5 and writes no file (acceptance test 2).

``--check`` compares every produced hash with ``tests/corpora/manifest.json`` and exits 5 on a difference
or on an entry whose hash is not recorded yet; ``--update-manifest`` writes the produced hashes into the
manifest instead (the maintainer's regeneration mode). ``--sync-manifest`` rewrites the ``syn`` stream entries
of the manifest from ``recipes.json`` (ids, features, tags, aliases), keeping recorded hashes. ``local_only`` recipes (the perf files) run only with
``--local-only`` or when named by ``--only``. A recipe whose ``{corpus:ID}`` input is a ``manual_download``
corpus missing from the cache is skipped, with the reason logged, unless ``--only`` names it. ``--fixtures`` runs the non-stream fixture generators instead of
the stream recipes.

Exit codes: 0 success; 1 a step failed; 2 usage; 4 invalid recipes, unknown id, missing tool or helper;
5 pin mismatch or hash mismatch.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
DEFAULT_RECIPES = HERE / "recipes.json"
DEFAULT_MANIFEST = REPO / "tests" / "corpora" / "manifest.json"
UNPINNED = {"{python}", "cp", "cat", "ovt"}  # interpreters and coreutils; `ovt` is the tool under test
STAMPS = os.environ.get("OVT_ORACLES_STAMPS", "/opt/ovt-oracles/share/ovt-oracles/stamps")
SYN_LICENCE = {
    "licence": "OVT synthetic stream generated from lavfi sources (CC0)",
    "licence_spdx": "CC0-1.0",
    "redistributable": True,
}

EXIT_OK, EXIT_STEP, EXIT_INVALID, EXIT_MISMATCH = 0, 1, 4, 5
UNSEEDED_LAVFI = re.compile(r"(?:^|[,;\]])\s*(life|cellauto)(?:=([^,;\[\]]*))?\s*(?=$|[,;\[])")


class GenError(Exception):
    def __init__(self, code: int, message: str) -> None:
        super().__init__(message)
        self.code = code


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        while chunk := fh.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def natural_key(name: str) -> list[Any]:
    return [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", name)]


# --------------------------------------------------------------------------------------------- recipes


def step_argvs(step: dict[str, Any]) -> list[list[str]]:
    return [step["cmd"]] if "cmd" in step else step["pipe"]


# SPEC-ASSUMPTION(#119): recipe commands that cannot run verbatim (no shell, missing arguments, random seeds or
# IVs) carry the proposed fixes of the issue; each affected recipe has a "note".
def wrap_step(es_file: str) -> dict[str, Any]:
    """The §14.3.3 MP4 wrapping step; raw Annex B input gets -framerate 30 (every source is 30 fps)."""
    rate = ["-framerate", "30"] if Path(es_file).suffix in (".264", ".265") else []
    out = Path(es_file).with_suffix(".mp4").name
    return {"cmd": ["ffmpeg", *rate, "-i", es_file, "-c", "copy", "-movflags", "+faststart", out]}


def aom_option(args: list[str], name: str, default: int) -> int:
    for arg in args:
        if arg.startswith(f"--{name}="):
            try:
                return int(arg.split("=", 1)[1])
            except ValueError:
                return default
    return default


BENTO4_CLOCK_TOOLS = {"mp4fragment", "mp4encrypt"}  # write the wall-clock time into mvhd
PIN_TIMES = "{repo}/tools/mp4_pin_times.py"


def bento4_time_problem(steps: list[dict[str, Any]]) -> str | None:
    """A Bento4 step whose output carries the clock must be followed by tools/mp4_pin_times.py."""
    last_bento4 = last_pin = -1
    for i, step in enumerate(steps):
        for argv in step_argvs(step):
            if argv[0] in BENTO4_CLOCK_TOOLS:
                last_bento4 = i
            elif PIN_TIMES in argv:
                last_pin = i
    if last_bento4 >= 0 and last_pin < last_bento4:
        return "Bento4 mp4fragment/mp4encrypt write the current time into mvhd; pin it with tools/mp4_pin_times.py"
    return None


def av1_scale_problem(argv: list[str]) -> str | None:
    """Why an aomenc command would fail with "Can't find at least one reference frame with valid size", or None.

    AV1 lets a reference frame be at most 2x the coded width of the frame predicting from it (the reference-scaling limit).
    aomenc's frame resize (--resize-mode, denominator R/8) and superres (--superres-mode, coded width 8/S of the
    upscaled width) multiply, while key frames keep the full size, so (R/8) * (S/8) must stay at or under 2
    (R * S <= 128). Recipe 19 used R=S=12 (2.25x); it now uses R=9, S=12 (1.6875x).
    """
    if argv[0] != "aomenc":
        return None
    args = argv[1:]
    resize = aom_option(args, "resize-denominator", 8) if aom_option(args, "resize-mode", 0) else 8
    superres = aom_option(args, "superres-denominator", 8) if aom_option(args, "superres-mode", 0) else 8
    if resize * superres > 128:
        scale = resize * superres / 64
        return f"resize {resize}/8 x superres {superres}/8 = {scale:g}x exceeds AV1's 2x reference scaling"
    return None


def determinism_problem(argv: list[str]) -> str | None:
    """Why ``argv`` would give different bytes on another machine or run, or None.

    The x264/x265/libaom output depends on the thread count, which defaults to the CPU count; MP4Box and
    mkvmerge stamp the wall-clock time and random UIDs unless told not to; lavfi ``life``/``cellauto`` start
    from a random grid and ``mp4encrypt --key …:random`` draws a random IV (#119, §14.3.3 rules 1-2).
    """
    tool, args = argv[0], argv[1:]
    for arg in args:  # lavfi sources whose first frame is random unless seeded
        for name, opts in UNSEEDED_LAVFI.findall(arg):
            if not re.search(r"(^|:)seed=", opts):
                return f"lavfi {name} needs seed=N"
    if tool == "mp4encrypt" and any(a.endswith(":random") for a in args):
        return "mp4encrypt needs a fixed IV, not :random"
    if tool == "x264" and "--threads" not in args:
        return "x264 needs --threads N"
    if tool == "x265" and not {"--frame-threads", "--pools"} <= set(args):
        return "x265 needs --frame-threads N --pools N"
    if tool == "aomenc" and not any(a.startswith("--threads=") for a in args):
        return "aomenc needs --threads=N"
    if tool == "MP4Box" and "-for-test" not in args:
        return "MP4Box needs -for-test (no dates, no version string)"
    if tool == "mkvmerge" and not {"--no-date", "--deterministic"} <= set(args):
        return "mkvmerge needs --no-date --deterministic SEED"
    if tool == "ffmpeg" and "-c:v" in args:
        codec = args[args.index("-c:v") + 1]
        after = args[args.index("-c:v") :]  # before -i, -threads sets only the decoder threads
        if codec in ("libx264", "libaom-av1") and "-threads" not in after:
            return f"ffmpeg {codec} needs -threads N after -c:v"
        if codec == "libx265":
            params = args[args.index("-x265-params") + 1] if "-x265-params" in args else ""
            if "frame-threads=" not in params or "pools=" not in params:
                return "ffmpeg libx265 needs -x265-params ...:frame-threads=N:pools=N"
    return None


def load_recipes(path: Path) -> dict[str, Any]:
    """Load recipes.json and check its structure; raise GenError(4) on any problem."""
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise GenError(EXIT_INVALID, f"cannot read {path}: {exc}") from exc
    if doc.get("schema_version") != 1:
        raise GenError(EXIT_INVALID, "recipes.json: schema_version must be 1")
    sources = {s["file"]: s for s in doc["sources"]}
    ids: dict[str, dict[str, Any]] = {}
    for rec in doc["recipes"]:
        rid = rec["id"]
        if not re.fullmatch(r"syn_[A-Za-z0-9_]{1,60}", rid) or rid in ids:
            raise GenError(EXIT_INVALID, f"recipes.json: invalid or duplicate id {rid!r}")
        if not rec.get("files"):
            raise GenError(EXIT_INVALID, f"recipes.json: {rid} has no files")
        ids[rid] = rec
    for item in [*doc["sources"], *doc["recipes"]]:
        for need in item.get("needs", []):
            if need not in sources and need not in ids:
                raise GenError(EXIT_INVALID, f"recipes.json: {item.get('id', item.get('file'))} needs unknown {need!r}")
        for step in item["steps"]:
            for argv in step_argvs(step):
                tool = argv[0]
                if tool not in UNPINNED and tool not in doc["pins"]:
                    raise GenError(EXIT_INVALID, f"recipes.json: tool {tool!r} has no pin")
                if problem := determinism_problem(argv):
                    name = item.get("id", item.get("file"))
                    raise GenError(EXIT_INVALID, f"recipes.json: {name}: not reproducible: {problem}")
                if problem := av1_scale_problem(argv):
                    name = item.get("id", item.get("file"))
                    raise GenError(EXIT_INVALID, f"recipes.json: {name}: {problem}")
        if problem := bento4_time_problem(item["steps"]):
            name = item.get("id", item.get("file"))
            raise GenError(EXIT_INVALID, f"recipes.json: {name}: not reproducible: {problem}")
    doc["_sources"], doc["_ids"] = sources, ids
    return doc


def closure(doc: dict[str, Any], wanted: list[str]) -> list[dict[str, Any]]:
    """Sources and recipes needed for ``wanted``, dependencies first, in file order."""
    sources, ids = doc["_sources"], doc["_ids"]
    keep: set[str] = set()

    def visit(name: str) -> None:
        if name in keep:
            return
        keep.add(name)
        item = sources.get(name) or ids[name]
        for need in item.get("needs", []):
            visit(need)

    for name in wanted:
        visit(name)
    ordered = [s for s in doc["sources"] if s["file"] in keep]
    done: set[str] = {s["file"] for s in ordered}
    pending = [r for r in doc["recipes"] if r["id"] in keep]
    while pending:  # stable topological order: a recipe runs after the recipes it needs
        progress = [r for r in pending if all(n in done for n in r.get("needs", []))]
        if not progress:
            raise GenError(EXIT_INVALID, "recipes.json: dependency cycle among " + ", ".join(r["id"] for r in pending))
        for r in progress:
            ordered.append(r)
            done.add(r["id"])
        pending = [r for r in pending if r["id"] not in done]
    return ordered


def select(doc: dict[str, Any], only: list[str], local_only: bool) -> list[str]:
    if only:
        unknown = [i for i in only if i not in doc["_ids"]]
        if unknown:
            raise GenError(EXIT_INVALID, "unknown id: " + ", ".join(unknown))
        return only
    return [r["id"] for r in doc["recipes"] if bool(r.get("local_only")) == local_only]


# ------------------------------------------------------------------------------------------------ pins


def probe(pin: dict[str, Any]) -> str:
    """The first line of the tool's version output that contains the pin's ``expect`` text, else the first line."""
    if "stamp" in pin:
        try:
            text = (Path(STAMPS) / f"{pin['stamp']}.version").read_text(encoding="utf-8")
        except OSError:
            return "<no version stamp>"
    else:
        try:
            res = subprocess.run(
                pin["probe"], capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=60, check=False
            )
        except (OSError, subprocess.TimeoutExpired):
            return "<not found>"
        text = res.stdout + res.stderr
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    for line in lines:
        if pin["expect"] in line:
            return line
    return lines[0] if lines else "<no output>"


def tools_of(items: list[dict[str, Any]]) -> list[str]:
    tools: set[str] = set()
    for item in items:
        for step in item["steps"]:
            tools.update(argv[0] for argv in step_argvs(step))
        if item.get("mp4"):
            tools.add("ffmpeg")
    return sorted(tools - UNPINNED)


def check_pins(doc: dict[str, Any], tools: list[str]) -> dict[str, str]:
    """Probe every tool; raise GenError(5) with a version diff when any differs from its pin."""
    found = {tool: probe(doc["pins"][tool]) for tool in tools}
    bad = [t for t in tools if doc["pins"][t]["expect"] not in found[t]]
    if bad:
        diff = "\n".join(f"  {t}: pinned {doc['pins'][t]['expect']!r}, found {found[t]!r}" for t in bad)
        raise GenError(EXIT_MISMATCH, "encoder version mismatch (no file written):\n" + diff)
    return found


# --------------------------------------------------------------------------------------------- running


class Runner:
    def __init__(self, out: Path, work: Path) -> None:
        self.out, self.work = out, work
        self.env = {**os.environ, "SOURCE_DATE_EPOCH": "0", "TZ": "UTC"}

    def expand(self, arg: str, extra: dict[str, str]) -> list[str]:
        if arg.startswith("{glob:") and arg.endswith("}"):
            pattern = arg[6:-1]
            names = sorted((p.name for p in self.work.glob(pattern)), key=natural_key)
            if not names:
                raise GenError(EXIT_STEP, f"no work file matches {pattern}")
            return names

        def corpus(m: re.Match[str]) -> str:
            return str(corpus_file(self.out, m.group(1)))

        arg = re.sub(r"\{corpus:([A-Za-z0-9_]+)\}", corpus, arg)
        subst = {
            "{python}": sys.executable,
            "{repo}": str(REPO),
            "{hm_cfg}": os.environ.get("OVT_HM_CFG_DIR", "/opt/ovt-oracles/share/hm/cfg"),
            "{jm_cfg}": os.environ.get("OVT_JM_CFG_DIR", "/opt/ovt-oracles/share/jm/cfg"),
            **extra,
        }
        for key, value in subst.items():
            arg = arg.replace(key, value)
        return [arg]

    def run_step(self, step: dict[str, Any], logf: Any, extra: dict[str, str] | None = None) -> None:
        argvs = [[a for arg in argv for a in self.expand(arg, extra or {})] for argv in step_argvs(step)]
        out_fh = (self.work / step["stdout"]).open("wb") if "stdout" in step else logf
        procs: list[subprocess.Popen[bytes]] = []
        try:
            prev: Any = subprocess.DEVNULL
            for i, argv in enumerate(argvs):
                last = i == len(argvs) - 1
                logf.write(("$ " + " ".join(argv) + "\n").encode())
                logf.flush()
                try:
                    proc = subprocess.Popen(
                        argv,
                        cwd=self.work,
                        env=self.env,
                        stdin=prev,
                        stdout=out_fh if last else subprocess.PIPE,
                        stderr=logf,
                    )
                except OSError as exc:
                    raise GenError(EXIT_INVALID, f"cannot run {argv[0]}: {exc}") from exc
                if prev is not subprocess.DEVNULL and hasattr(prev, "close"):
                    prev.close()  # the next process owns the pipe now
                prev = proc.stdout if not last else None
                procs.append(proc)
            codes = [p.wait() for p in procs]
        finally:
            if "stdout" in step:
                out_fh.close()
        if any(codes):
            raise GenError(EXIT_STEP, f"step failed (exit {codes}): {' '.join(argvs[-1])}")

    def run_item(self, item: dict[str, Any]) -> None:
        name = item.get("id", item.get("file"))
        steps = list(item["steps"])
        if item.get("mp4"):
            steps.append(wrap_step(item["mp4"]))
        (self.work / "logs").mkdir(exist_ok=True)
        with (self.work / "logs" / f"{name}.log").open("wb") as logf:
            for step in steps:
                self.run_step(step, logf)

    def collect(self, rec: dict[str, Any]) -> dict[str, Any]:
        """Copy a recipe's files to ``<out>/<id>/`` and write ``.ok``; returns the record."""
        names: list[str] = []
        for pattern in rec["files"]:
            if any(c in pattern for c in "*?["):
                names += sorted((p.name for p in self.work.glob(pattern)), key=natural_key)
            else:
                names.append(pattern)
        if rec.get("mp4"):
            mp4 = Path(rec["mp4"]).with_suffix(".mp4").name
            names = [mp4] + [n for n in names if n != mp4]
        dest = self.out / rec["id"]
        shutil.rmtree(dest, ignore_errors=True)
        dest.mkdir(parents=True)
        files = {}
        for name in names:
            src = self.work / name
            if not src.is_file():
                raise GenError(EXIT_STEP, f"{rec['id']}: expected output {name} was not produced")
            shutil.copyfile(src, dest / name)
            files[name] = sha256_file(dest / name)
        primary = rec.get("primary", names[0])
        record = {
            "sha256": files[primary],
            "primary": primary,
            "files": files,
            "size_bytes": (dest / primary).stat().st_size,
        }
        (dest / ".ok").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return record


def corpus_file(out: Path, cid: str) -> Path:
    """The primary file of a generated or fetched corpus id (its .ok record)."""
    for root in (out, Path(os.environ.get("OVT_CORPORA_DIR", out))):
        try:
            record = json.loads((root / cid / ".ok").read_text(encoding="utf-8"))
            return root / cid / record["primary"]
        except (OSError, ValueError, KeyError):
            continue
    raise GenError(EXIT_STEP, f"corpus {cid} is not available (fetch it with tools/fetch_corpora.py --ids {cid})")


def manual_corpus_gaps(doc: dict[str, Any], manifest: dict[str, Any], out: Path, wanted: list[str]) -> dict[str, str]:
    """Recipes among ``wanted`` whose ``{corpus:ID}`` input is a ``manual_download`` corpus not in the cache.

    Such a corpus has no URL for the fetcher (chapter 14 §14.3.1): a maintainer places it by hand. Until then the
    recipe cannot run, so the default selection skips it with this reason and its manifest entry stays unverified.
    Returns {recipe id: reason}.
    """
    manual = {e["id"] for e in manifest["entries"] if e.get("manual_download")}
    gaps = {}
    for rid in wanted:
        rec = doc["_ids"][rid]
        for step in rec["steps"]:
            for argv in step_argvs(step):
                for arg in argv:
                    for cid in re.findall(r"\{corpus:([A-Za-z0-9_]+)\}", arg):
                        if cid not in manual or rid in gaps:
                            continue
                        try:
                            corpus_file(out, cid)
                        except GenError:
                            gaps[rid] = (
                                f"needs {cid}, a manual_download corpus that is not in the cache "
                                f"(no URL or hash to fetch it; place it by hand, then run --only {rid})"
                            )
    return gaps


def missing_helpers(items: list[dict[str, Any]]) -> list[str]:
    missing = set()
    for item in items:
        for step in item["steps"]:
            for argv in step_argvs(step):
                for arg in argv:
                    if arg.startswith("{repo}/"):
                        path = REPO / arg[len("{repo}/") :]
                        if not path.exists():
                            missing.add(arg[len("{repo}/") :])
    return sorted(missing)


# -------------------------------------------------------------------------------------------- manifest


def manifest_entry(rec: dict[str, Any]) -> dict[str, Any]:
    """The manifest entry of a recipe id, without hashes (``unverified`` until generated in the oracle image)."""
    entry: dict[str, Any] = {"id": rec["id"], "source": "syn", "kind": "stream", "recipe": rec["recipe"]}
    entry.update(rec.get("licence_override", SYN_LICENCE))
    entry["unverified"] = True
    for key in ("features", "tags", "aliases"):
        if rec.get(key):
            entry[key] = list(rec[key])
    if rec.get("local_only"):
        entry["local_only"] = True
    return entry


def compare(manifest: dict[str, Any], results: dict[str, dict[str, Any]]) -> list[str]:
    entries = {e["id"]: e for e in manifest["entries"]}
    problems = []
    for cid, record in results.items():
        entry = entries.get(cid)
        if entry is None:
            problems.append(f"{cid}: no manifest entry")
        elif "sha256" not in entry:
            problems.append(f"{cid}: hash not recorded in the manifest (got {record['sha256']})")
        elif entry["sha256"] != record["sha256"] or entry.get("files", record["files"]) != record["files"]:
            problems.append(f"{cid}: manifest {entry['sha256']}, produced {record['sha256']}")
    return problems


def sync_manifest(path: Path, doc: dict[str, Any]) -> None:
    """Write one manifest entry per recipe id from recipes.json, keeping recorded hashes; drop stale syn streams.

    An entry without ``kind`` was written by another work item (the recipe-73 patterns of WI-1.413) and is kept.
    """
    manifest = json.loads(path.read_text(encoding="utf-8"))
    old = {e["id"]: e for e in manifest["entries"]}
    kept = [e for e in manifest["entries"] if not (e["source"] == "syn" and e.get("kind") == "stream")]
    for rec in doc["recipes"]:
        prev = old.get(rec["id"], {})
        if prev and "kind" not in prev:
            continue  # written by the WI that owns the generator (recipe 73: WI-1.413); kept as it is
        entry = manifest_entry(rec)
        if "sha256" in prev:
            entry.pop("unverified")
            entry.update({k: prev[k] for k in ("sha256", "size_bytes", "files") if k in prev})
        kept.append(entry)
    manifest["entries"] = kept
    path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def update_manifest(path: Path, manifest: dict[str, Any], results: dict[str, dict[str, Any]]) -> None:
    for entry in manifest["entries"]:
        record = results.get(entry["id"])
        if record:
            entry.pop("unverified", None)
            entry.update(sha256=record["sha256"], size_bytes=record["size_bytes"], files=record["files"])
    path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


# ------------------------------------------------------------------------------------------------- main


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--only", default="", help="comma-separated corpus ids (their dependencies run too)")
    ap.add_argument("--out", type=Path, help="output cache dir (default: the fetcher's cache dir)")
    ap.add_argument("--check", action="store_true", help="compare hashes with the manifest; exit 5 on a difference")
    ap.add_argument("--fixtures", action="store_true", help="run the non-stream fixture generators")
    ap.add_argument("--local-only", action="store_true", help="run the local_only (perf runner) recipes")
    ap.add_argument("--update-manifest", action="store_true", help="write the produced hashes into the manifest")
    ap.add_argument("--list", action="store_true", help="print the selected ids and exit")
    ap.add_argument("--sync-manifest", action="store_true", help="write the syn manifest entries from recipes.json")
    ap.add_argument("--recipes", type=Path, default=DEFAULT_RECIPES)
    ap.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    return ap.parse_args(argv)


def default_out() -> Path:
    sys.path.insert(0, str(REPO / "tools"))
    from fetch_corpora import cache_dir_for

    return cache_dir_for(None)


def run_fixtures(doc: dict[str, Any], out: Path) -> int:
    items = [{"steps": [{"cmd": f["cmd"]}]} for f in doc["fixtures"]]
    if missing := missing_helpers(items):
        raise GenError(EXIT_INVALID, "missing fixture generator(s): " + ", ".join(missing))
    check_pins(doc, tools_of(items))
    work = out / ".work"
    work.mkdir(parents=True, exist_ok=True)
    runner = Runner(out, work)
    with (work / "fixtures.log").open("wb") as logf:
        for fixture in doc["fixtures"]:
            target = REPO / fixture["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            runner.run_step({"cmd": fixture["cmd"]}, logf, {"{out}": str(target)})
            print(f"{sha256_file(target)}  {fixture['path']}")
    return EXIT_OK


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        doc = load_recipes(args.recipes)
        if args.sync_manifest:
            sync_manifest(args.manifest, doc)
            return EXIT_OK
        out = (args.out or default_out()).resolve()
        if args.fixtures:
            return run_fixtures(doc, out)
        only = [i for i in args.only.split(",") if i]
        wanted = select(doc, only, args.local_only)
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        if not only:  # an id named by --only runs, and fails if its corpus is missing
            gaps = manual_corpus_gaps(doc, manifest, out, wanted)
            for rid, reason in gaps.items():
                log(f"skip {rid}: {reason}")
            wanted = [rid for rid in wanted if rid not in gaps]
        items = closure(doc, wanted)
        if args.list:
            print("\n".join(r["id"] for r in items if "id" in r))
            return EXIT_OK
        if missing := missing_helpers(items):
            raise GenError(EXIT_INVALID, "missing helper script(s): " + ", ".join(missing))
        versions = check_pins(doc, tools_of(items))  # before anything is written
        work = out / ".work"
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
        runner = Runner(out, work)
        results: dict[str, dict[str, Any]] = {}
        for item in items:
            log(f"generate {item.get('id', item.get('file'))}")
            runner.run_item(item)
            if "id" in item:
                results[item["id"]] = runner.collect(item)
                print(f"{results[item['id']]['sha256']}  {item['id']}")
        if args.update_manifest:
            update_manifest(args.manifest, manifest, results)
        if args.check and (problems := compare(manifest, results)):
            log("hash mismatch against the manifest:\n  " + "\n  ".join(problems))
            log("encoder versions:\n  " + "\n  ".join(f"{t}: {v}" for t, v in versions.items()))
            return EXIT_MISMATCH
        return EXIT_OK
    except GenError as exc:
        log(f"error: {exc}")
        return exc.code


if __name__ == "__main__":
    sys.exit(main())
