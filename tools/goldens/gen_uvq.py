#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate the UVQ goldens (chapter 14 §14.4.14, WI-0.565b).

    python -I tools/goldens/gen_uvq.py [--out tests/golden/metrics] [--corpora-dir DIR] [--only CLIP[,CLIP...]]
        [--versions 1.5,1.0] [--fps 1] [--work-dir DIR] [--list]

Runs inside the oracle image: google/uvq (the revision pinned in ``tests/oracles/Dockerfile``, PyTorch) on CPU, one
thread, on each ``ytugc`` ``fast`` clip (no-reference, the clip itself is the input). For every clip and model version
it writes ``<clip>.uvq-<ver>.json.gz`` (canonical ``scores`` JSON through ``tests/oracles/normalize/uvq.py``,
reproducible gzip) and ``uvq-index.json`` (versions, input hashes, failed steps).

SPEC-ASSUMPTION(#306): the oracle is ``uvq_inference.py --model_version <ver> --device cpu --threads 1 --fps <fps>``
(#47), not the TensorFlow ``uvq_main.py`` of the §14.4.14 text; UVQ 1.5 samples ``--fps`` frames per second (default 1,
the ``uvq.fps=1`` of the OVT side), UVQ 1.0 scores the whole clip.

Exit status: 0 all done, 1 a step failed, 2 usage, 4 a tool or corpus is missing.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import tempfile
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import gen_metrics as gm  # noqa: E402

CLIPS = ("ytugc_clip01", "ytugc_clip02", "ytugc_clip03")
VERSIONS = ("1.5", "1.0")


def uvq_script() -> Path:
    prefix = os.environ.get("OVT_ORACLES_PREFIX", "/opt/ovt-oracles")
    path = Path(os.environ.get("OVT_ORACLE_UVQ_INFERENCE", Path(prefix) / "uvq" / "uvq_inference.py"))
    if not path.is_file():
        raise gm.MissingTool(f"uvq_inference.py ({path})")
    return path


def run_uvq(script: Path, clip: Path, version: str, fps: int, work: Path) -> Path:
    out = work / f"{clip.stem}.uvq-{version}.native.json"
    cmd = [sys.executable, str(script), str(clip), "--model_version", version, "--device", "cpu", "--threads", "1",
           "--fps", str(fps), "--output", str(out)]  # fmt: skip
    env_keys = {
        "OMP_NUM_THREADS": "1",
        "MKL_NUM_THREADS": "1",
        "CUBLAS_WORKSPACE_CONFIG": ":4096:8",
    }
    saved = {k: os.environ.get(k) for k in env_keys}
    os.environ.update(env_keys)
    try:
        gm.run(cmd, work, f"uvq_inference.py {version}")
    finally:
        for k, v in saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    if not out.is_file():
        raise gm.StepError(f"uvq_inference.py {version} wrote no {out.name}")
    return out


def process(cid: str, args: argparse.Namespace, script: Path) -> dict[str, Any]:
    entry: dict[str, Any] = {"goldens": {}, "failed": {}}
    clip = gm.corpus_path(args.corpora_dir, cid, None)
    entry["sha256"] = gm.sha256_file(clip)
    work = args.work_root / cid
    work.mkdir(parents=True, exist_ok=True)
    for ver in args.versions:
        try:
            native = run_uvq(script, clip, ver, args.fps, work)
            text = gm.normalise("uvq.py", native, [], work)
            name = f"{cid}.uvq-{ver}.json.gz"
            gm.write_gz(args.out / name, text)
            entry["goldens"][ver] = name
        except gm.StepError as exc:
            entry["failed"][ver] = str(exc)
    return entry


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--out", type=Path, default=gm.REPO / "tests" / "golden" / "metrics")
    p.add_argument(
        "--corpora-dir",
        type=Path,
        default=Path(os.environ.get("OVT_CORPORA_DIR", "corpora")),
    )
    p.add_argument("--only", help="comma-separated clip ids")
    p.add_argument("--versions", default=",".join(VERSIONS), help="subset of " + ",".join(VERSIONS))
    p.add_argument("--fps", type=int, default=1)
    p.add_argument("--work-dir", type=Path)
    p.add_argument("--list", action="store_true", help="print the plan and exit")
    args = p.parse_args(argv)
    args.versions = [v for v in args.versions.split(",") if v]
    if set(args.versions) - set(VERSIONS):
        p.error("unknown version: " + ",".join(sorted(set(args.versions) - set(VERSIONS))))
    args.only = [i for i in args.only.split(",") if i] if args.only else list(CLIPS)
    if set(args.only) - set(CLIPS):
        p.error("unknown clip: " + ",".join(sorted(set(args.only) - set(CLIPS))))
    args.out = args.out.resolve()
    args.corpora_dir = args.corpora_dir.resolve()
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.list:
        for cid in args.only:
            print(cid, " ".join(f"uvq-{v}" for v in args.versions))
        return gm.EXIT_OK
    args.out.mkdir(parents=True, exist_ok=True)
    args.work_root = args.work_dir or Path(tempfile.mkdtemp(prefix="gen_uvq_"))
    args.work_root.mkdir(parents=True, exist_ok=True)
    index: dict[str, Any] = {"clips": {}, "fps": args.fps}
    try:
        script = uvq_script()
        stamps = os.environ.get("OVT_ORACLES_STAMPS")
        stamp = Path(stamps) / "uvq.version" if stamps else None
        index["uvq"] = stamp.read_text(encoding="utf-8").strip() if stamp and stamp.is_file() else "unknown"
        for cid in args.only:
            index["clips"][cid] = process(cid, args, script)
            e = index["clips"][cid]
            print(f"{cid}: {'FAILED ' + ','.join(e['failed']) if e['failed'] else 'ok'} ({len(e['goldens'])} goldens)",
                  file=sys.stderr)  # fmt: skip
    except gm.MissingTool as exc:
        print(f"error: not found (run this in the oracle image): {exc}", file=sys.stderr)
        return gm.EXIT_MISSING
    finally:
        if args.work_dir is None:
            shutil.rmtree(args.work_root, ignore_errors=True)
    (args.out / "uvq-index.json").write_text(json.dumps(index, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    bad = {cid: e["failed"] for cid, e in index["clips"].items() if e["failed"]}
    for cid, failed in bad.items():
        for ver, why in failed.items():
            print(f"FAILED {cid} uvq-{ver}: {why}", file=sys.stderr)
    return gm.EXIT_OK if not bad else gm.EXIT_STEP


if __name__ == "__main__":
    sys.exit(main())
