#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate the metric oracle goldens (chapter 14 §14.4.10-14.4.13, WI-0.565).

    python -I tools/goldens/gen_metrics.py [--out tests/golden/metrics] [--corpora-dir DIR] [--generate-syn]
        [--only PAIR[,PAIR...]] [--rd-points DIR] [--oracles ffpsnr,ffvmaf,vmaf,x264ssim,daala,bd] [--work-dir DIR]
        [--frames N] [--keep-going] [--list]

Runs inside the oracle image (``ffmpeg``, ``vmaf``, ``x264``, ``dump_psnrhvs``, ``dump_ciede2000`` and the
``bjontegaard`` package; each tool is overridable with ``$OVT_ORACLE_<NAME>``). ``x264_ssim`` (tests/oracles/x264_ssim)
is built with CMake from the repository when it is not on ``PATH``. For every metric pair below it writes, below
``--out``, gzip-compressed canonical ``scores`` JSON (``<id>.<oracle>.json.gz``, reproducible bytes):

    <pair>.ffpsnr.json.gz                  ffmpeg psnr filter, stats file                    §14.4.10
    <pair>.ffvmaf.json.gz                  ffmpeg libvmaf filter (vmaf_v0.6.1 + psnr, float_ssim)  §14.4.10
    <pair>.<model>.vmaf.json.gz            vmaf CLI, every bundled model, all features       §14.4.11
    <pair>.vmaf_v0.6.1-sub5.vmaf.json.gz   the same with --subsample 5
    <pair>.x264ssim.json.gz                x264_ssim                                         §14.4.12
    <pair>.psnrhvs.json.gz, <pair>.ciede.json.gz   Daala dump_psnrhvs / dump_ciede2000       §14.4.13
    bd/<set>.bd.json                       bjontegaard for every rd_points/<set>.json         §14.4.13

and ``metrics-index.json`` (the oracle versions, the pairs with their input hashes and frame counts, the
acceptance-test checks of WI-0.565, every skipped or failed step). A pair is the reference and the distorted Y4M
file cut to ``--frames`` pictures, so the goldens of a 1080p pair stay small and the Daala tools finish.

Checks (WI-0.565 acceptance tests 1 and 3), recorded in the index and failing the run when they do not hold:

* ``vmaf_v0.6.1`` pooled mean of the Netflix pair equals the published ``76.6689...`` (Netflix ``python/test``);
* the mean ``Y`` of ``x264_ssim`` on the x264 reconstruction of the Netflix reference equals the ``SSIM Mean Y`` of
  ``x264 --ssim`` to 1e-4.

Exit status: 0 all done, 1 a step failed (unless ``--keep-going``), 2 usage, 4 a tool or corpus is missing.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
NORM = REPO / "tests" / "oracles" / "normalize"
SYNTH = REPO / "tests" / "corpora" / "synthetic" / "generate.py"
X264_SSIM_SRC = REPO / "tests" / "oracles" / "x264_ssim"

EXIT_OK, EXIT_STEP, EXIT_USAGE, EXIT_MISSING = 0, 1, 2, 4
ALL_ORACLES = ("ffpsnr", "ffvmaf", "vmaf", "x264ssim", "daala", "bd")
# Every model bundled by libvmaf 3.0.0 (-Dbuilt_in_models=true, chapter 9 §9.5.3) and the features of §14.4.11.
VMAF_MODELS = (
    "vmaf_v0.6.1",
    "vmaf_v0.6.1neg",
    "vmaf_4k_v0.6.1",
    "vmaf_b_v0.6.3",
    "vmaf_float_v0.6.1",
    "vmaf_float_v0.6.1neg",
    "vmaf_float_4k_v0.6.1",
    "vmaf_float_b_v0.6.3",
)
VMAF_FEATURES = ("psnr", "float_ssim", "float_ms_ssim", "psnr_hvs", "ciede", "cambi")
FF_VMAF_FEATURES = ("psnr", "float_ssim")
SUBSAMPLE = 5
NETFLIX_PAIR = "netflix_src01_576x324"
# SPEC-ASSUMPTION(#306): the WI text says 76.68..., Netflix's published value is 76.6689...
# The pooled VMAF of vmaf_v0.6.1 on src01_hrc00 vs src01_hrc01 (576x324, 48 frames) in Netflix python/test/ and README.
NETFLIX_VMAF = 76.66890519623931
NETFLIX_VMAF_TOLERANCE = 0.005
X264_LOG_TOLERANCE = 1e-4
MAX_GOLDEN_BYTES = 2_000_000  # gzip; the project cap

# SPEC-ASSUMPTION(#306): frame caps of 48/60/60 pictures, pair ids in the file names, no UVQ / ytugc goldens here.
# A pair: id -> (corpus id, reference file name or None for the primary file, distorted file name or None, frame cap,
# Daala tools supported). A distorted ``.mp4`` is decoded to Y4M first.
PAIRS: dict[str, dict[str, Any]] = {
    NETFLIX_PAIR: {
        "ref": ("netflix_src01_hrc00_576x324", None),
        "dist": ("netflix_src01_hrc01_576x324", None),
        "frames": 48,
        "daala": True,
    },
    "syn_metrics_1080p": {
        "ref": ("syn_metrics_1080p_pair", "syn_metrics_1080p_ref.y4m"),
        "dist": ("syn_metrics_1080p_pair", "syn_metrics_1080p_dist.mp4"),
        "frames": 60,
        "daala": True,
    },
    "syn_metrics_pq_1080p": {
        "ref": ("syn_metrics_pq_1080p_pair", "syn_metrics_pq_1080p_ref.y4m"),
        "dist": ("syn_metrics_pq_1080p_pair", "syn_metrics_pq_1080p_dist.y4m"),
        "frames": 60,
        "daala": False,  # 10-bit: the Daala tools read 8-bit Y4M; a failure here is recorded as skipped
    },
}


class StepError(Exception):
    """One oracle step of one pair failed."""


class MissingTool(Exception):
    pass


# ------------------------------------------------------------------------------------------------ tools


def tool(name: str) -> str:
    env = os.environ.get("OVT_ORACLE_" + name.upper().replace("-", "_"))
    found = env or shutil.which(name)
    if not found:
        raise MissingTool(name)
    return found


def run(cmd: list[str], cwd: Path, what: str, stdout: Any = subprocess.DEVNULL) -> str:
    res = subprocess.run(  # noqa: S603
        cmd, cwd=cwd, stdin=subprocess.DEVNULL, stdout=stdout, stderr=subprocess.PIPE, text=True, errors="replace",
        check=False, env={**os.environ, "SOURCE_DATE_EPOCH": "0", "TZ": "UTC"},
    )  # fmt: skip
    if res.returncode != 0:
        tail = " | ".join((res.stderr or "").strip().splitlines()[-4:])
        raise StepError(f"{what} exited {res.returncode}: {re.sub(r'0x[0-9a-f]{6,}', '0x…', tail)}")
    return res.stderr or ""


def normalise(script: str, input_file: Path, args: list[str], cwd: Path) -> str:
    cmd = [sys.executable, "-I", str(NORM / script), str(input_file), *args]
    res = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, check=False)  # noqa: S603
    if res.returncode != 0:
        raise StepError(f"{script} exited {res.returncode}: {res.stderr.strip()[-400:]}")
    return res.stdout


def write_gz(path: Path, text: str) -> None:
    """Goldens are stored gzip-compressed (the repository carries them for ever); no name or time in the header."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with (
        path.open("wb") as raw,
        gzip.GzipFile(filename="", mode="wb", fileobj=raw, compresslevel=9, mtime=0) as f,
    ):
        f.write(text.encode("utf-8"))
    if path.stat().st_size > MAX_GOLDEN_BYTES:
        path.unlink()
        raise StepError(f"{path.name} is larger than {MAX_GOLDEN_BYTES} bytes gzip-compressed")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def build_x264_ssim(work: Path) -> str:
    """``x264_ssim`` from the image's PATH or $OVT_ORACLE_X264_SSIM, else built from tests/oracles/x264_ssim."""
    try:
        return tool("x264_ssim")
    except MissingTool:
        pass
    build = work / "x264_ssim_build"
    run(["cmake", "-S", str(X264_SSIM_SRC), "-B", str(build)], work, "cmake (x264_ssim)")
    run(["cmake", "--build", str(build)], work, "cmake --build (x264_ssim)")
    return str(build / "x264_ssim")


# ------------------------------------------------------------------------------------------------ inputs


def y4m_info(path: Path) -> dict[str, Any]:
    """Width, height, bit depth and frame count of a 4:2:0 Y4M file (frames counted by size)."""
    with path.open("rb") as f:
        head = f.readline().decode("ascii", "replace")
        if not head.startswith("YUV4MPEG2"):
            raise StepError(f"{path.name} is not a Y4M file")
        w = int(re.search(r" W(\d+)", head).group(1))  # type: ignore[union-attr]
        h = int(re.search(r" H(\d+)", head).group(1))  # type: ignore[union-attr]
        depth = 10 if " C420p10" in head else 8
        frame_bytes = (w * h + 2 * ((w // 2) * (h // 2))) * (2 if depth > 8 else 1) + len(b"FRAME\n")
        frames = (path.stat().st_size - len(head)) // frame_bytes
    return {"width": w, "height": h, "bit_depth": depth, "frames": frames}


def corpus_path(corpora: Path, cid: str, name: str | None) -> Path:
    try:
        record = json.loads((corpora / cid / ".ok").read_text(encoding="utf-8"))
        path = corpora / cid / (name or record["primary"])
    except (OSError, ValueError, KeyError) as exc:
        raise MissingTool(f"corpus {cid} is not in {corpora} (tools/fetch_corpora.py --ids {cid})") from exc
    if not path.is_file():
        raise MissingTool(f"corpus {cid} has no file {path.name}")
    return path


def to_y4m(src: Path, dst: Path, frames: int, work: Path) -> None:
    """The first ``frames`` pictures of ``src`` as Y4M (decoded when ``src`` is a video file), pixel format unchanged."""
    cmd = [
        tool("ffmpeg"),
        "-hide_banner",
        "-v",
        "error",
        "-y",
        "-i",
        str(src),
        "-frames:v",
        str(frames),
    ]
    if src.suffix != ".y4m":
        cmd += ["-pix_fmt", "yuv420p"]
    run(
        [*cmd, "-strict", "-1", "-f", "yuv4mpegpipe", str(dst)],
        work,
        f"ffmpeg ({src.name} to Y4M)",
    )


# ------------------------------------------------------------------------------------------------ oracles


class Pair:
    """The state of one reference/distorted pair while its goldens are produced."""

    def __init__(
        self,
        pid: str,
        spec: dict[str, Any],
        ref: Path,
        dist: Path,
        work: Path,
        out: Path,
    ) -> None:
        self.id, self.spec, self.ref, self.dist, self.work, self.out = (
            pid,
            spec,
            ref,
            dist,
            work,
            out,
        )
        self.info = y4m_info(ref)

    def golden(self, label: str, text: str) -> str:
        name = f"{self.id}.{label}.json.gz"
        write_gz(self.out / name, text)
        return name

    def ffpsnr(self) -> str:
        stats = self.work / "ffpsnr.txt"
        run([tool("ffmpeg"), "-hide_banner", "-i", str(self.dist), "-i", str(self.ref), "-lavfi",
             f"[0:v][1:v]psnr=stats_file={stats}", "-f", "null", "-"], self.work, "ffmpeg psnr")  # fmt: skip
        return self.golden("ffpsnr", normalise("ffmetrics.py", stats, [], self.work))

    def ffvmaf(self) -> str:
        log = self.work / "ffvmaf.json"
        features = "|".join(f"name={f}" for f in FF_VMAF_FEATURES)
        graph = (f"[0:v][1:v]libvmaf=model='version=vmaf_v0.6.1':feature='{features}':log_path={log}:log_fmt=json"
                 f":n_threads=4")  # fmt: skip
        run([tool("ffmpeg"), "-hide_banner", "-i", str(self.dist), "-i", str(self.ref), "-lavfi", graph, "-f", "null",
             "-"], self.work, "ffmpeg libvmaf")  # fmt: skip
        return self.golden("ffvmaf", normalise("ffmetrics.py", log, [], self.work))

    def vmaf(self, model: str, subsample: int | None = None) -> tuple[str, dict[str, Any]]:
        label = model + (f"-sub{subsample}" if subsample else "")
        log = self.work / f"vmaf_{label}.json"
        cmd = [
            tool("vmaf"),
            "--reference",
            str(self.ref),
            "--distorted",
            str(self.dist),
            "--model",
            f"version={model}",
        ]
        for feature in VMAF_FEATURES:
            cmd += ["--feature", feature]
        cmd += ["--threads", "4", "--json", "--output", str(log)]
        if subsample:
            cmd += ["--subsample", str(subsample)]
        run(cmd, self.work, f"vmaf {label}")
        text = normalise("vmaf_json.py", log, [], self.work)
        return self.golden(f"{label}.vmaf", text), json.loads(text)

    def x264ssim(self, exe: str) -> tuple[str, str]:
        txt = self.work / "x264ssim.txt"
        with txt.open("w", encoding="utf-8") as f:
            run([exe, str(self.ref), str(self.dist)], self.work, "x264_ssim", stdout=f)
        text = normalise("x264_ssim.py", txt, [], self.work)
        return self.golden("x264ssim", text), text

    def daala(self) -> dict[str, str]:
        files: dict[str, str] = {}
        hvs = self.work / "psnrhvs.txt"
        with hvs.open("w", encoding="utf-8") as f:
            run(
                [tool("dump_psnrhvs"), "-y", str(self.ref), str(self.dist)],
                self.work,
                "dump_psnrhvs",
                stdout=f,
            )
        files["psnrhvs"] = self.golden("psnrhvs", normalise("daala.py", hvs, ["--metric", "psnr_hvs"], self.work))
        ciede = self.work / "ciede.txt"
        with ciede.open("w", encoding="utf-8") as f:
            run(
                [tool("dump_ciede2000"), str(self.ref), str(self.dist)],
                self.work,
                "dump_ciede2000",
                stdout=f,
            )
        files["ciede"] = self.golden("ciede", normalise("daala.py", ciede, ["--metric", "ciede2000"], self.work))
        return files


def mean_of(doc: dict[str, Any], section: str, key: str) -> float:
    if section == "pooled":
        return float(doc["pooled"][key]["mean"])
    values = [f[key] for f in doc["frames"]]
    return sum(values) / len(values)


def x264_log_check(pair: Pair, exe: str) -> dict[str, Any]:
    """SPEC-ASSUMPTION(#306): acceptance test 3: ``x264_ssim`` on the x264 reconstruction equals the ``SSIM Mean Y`` of ``x264 --ssim``.

    ``--bframes 0``: x264 scores a non-reference B frame before its deblocking, so for those frames its figure differs
    from the decoded picture's by about 2e-3; I and P frames agree exactly.
    """
    es = pair.work / "x264_check.264"
    log = run([tool("x264"), "--ssim", "--qp", "28", "--threads", "1", "--bframes", "0", "--output", str(es), str(pair.ref)],
              pair.work, "x264 --ssim")  # fmt: skip
    m = re.search(r"SSIM Mean Y:(\d\.\d+)", log)
    if not m:
        raise StepError("x264 --ssim printed no 'SSIM Mean Y' line")
    recon = pair.work / "x264_check_recon.y4m"
    run([tool("ffmpeg"), "-hide_banner", "-v", "error", "-y", "-i", str(es), "-f", "yuv4mpegpipe", str(recon)],
        pair.work, "ffmpeg (x264 stream to Y4M)")  # fmt: skip
    txt = pair.work / "x264_check.txt"
    with txt.open("w", encoding="utf-8") as f:
        run(
            [exe, str(pair.ref), str(recon)],
            pair.work,
            "x264_ssim (reconstruction)",
            stdout=f,
        )
    ys = [float(line.split()[1][2:]) for line in txt.read_text().splitlines() if line.startswith("n:")]
    ours, theirs = sum(ys) / len(ys), float(m.group(1))
    result = {
        "x264_log_mean_y": theirs,
        "x264_ssim_mean_y": round(ours, 7),
        "frames": len(ys),
    }
    if abs(ours - theirs) > X264_LOG_TOLERANCE:
        raise StepError(f"x264_ssim mean Y {ours:.7f} differs from the x264 log {theirs:.7f} by more than "
                        f"{X264_LOG_TOLERANCE}")  # fmt: skip
    return result


def bd_goldens(out: Path, rd_points: Path, work: Path) -> dict[str, str]:
    files: dict[str, str] = {}
    for src in sorted(rd_points.glob("*.json")):
        text = normalise("bd_reference.py", src, [], work)
        dst = out / "bd" / f"{src.stem}.bd.json"
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(text, encoding="utf-8")
        files[src.stem] = f"bd/{dst.name}"
    return files


# ------------------------------------------------------------------------------------------------ versions


def first_line(cmd: list[str]) -> str:
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, check=False, stdin=subprocess.DEVNULL)  # noqa: S603
    except OSError:
        return "unavailable"
    lines = (res.stdout + res.stderr).strip().splitlines()
    return lines[0].strip() if lines else "unavailable"


def versions() -> dict[str, str]:
    out = {
        "ffmpeg": first_line([tool("ffmpeg"), "-version"]),
        "vmaf": first_line([tool("vmaf"), "--version"]),
        "x264": first_line([tool("x264"), "--version"]),
    }
    stamps = os.environ.get("OVT_ORACLES_STAMPS")
    if stamps and (Path(stamps) / "dump_psnrhvs.version").is_file():
        out["daala"] = (Path(stamps) / "dump_psnrhvs.version").read_text(encoding="utf-8").strip()
    try:
        from importlib.metadata import version

        out["bjontegaard"] = version("bjontegaard")
    except Exception:  # noqa: BLE001 - the package is only in the image
        out["bjontegaard"] = "unavailable"
    return out


# ------------------------------------------------------------------------------------------------ main


def process(pid: str, args: argparse.Namespace, x264_exe: str | None) -> dict[str, Any]:
    spec = PAIRS[pid]
    work = args.work_root / pid
    work.mkdir(parents=True, exist_ok=True)
    frames = min(spec["frames"], args.frames) if args.frames else spec["frames"]
    ref = work / "ref.y4m"
    dist = work / "dist.y4m"
    to_y4m(corpus_path(args.corpora_dir, *spec["ref"]), ref, frames, work)
    to_y4m(corpus_path(args.corpora_dir, *spec["dist"]), dist, frames, work)
    pair = Pair(pid, spec, ref, dist, work, args.out)
    other = y4m_info(dist)
    if (other["width"], other["height"], other["bit_depth"], other["frames"]) != tuple(
        pair.info[k] for k in ("width", "height", "bit_depth", "frames")
    ):
        raise StepError(f"reference {pair.info} and distorted {other} differ in size, bit depth or frame count")
    record: dict[str, Any] = {**pair.info, "ref_sha256": sha256_file(ref), "dist_sha256": sha256_file(dist),
                              "goldens": {}, "skipped": {}, "failed": {}, "checks": {}}  # fmt: skip

    def step(label: str, fn: Any) -> Any:
        try:
            return fn()
        except StepError as exc:
            if label.startswith("daala") and not spec["daala"]:
                record["skipped"][label] = str(exc)
            else:
                record["failed"][label] = str(exc)
        except MissingTool as exc:
            record["failed"][label] = f"{exc} is not on PATH"
        return None

    want = set(args.oracles)
    if "ffpsnr" in want:
        name = step("ffpsnr", pair.ffpsnr)
        if name:
            record["goldens"]["ffpsnr"] = name
    if "ffvmaf" in want:
        name = step("ffvmaf", pair.ffvmaf)
        if name:
            record["goldens"]["ffvmaf"] = name
    if "vmaf" in want:
        for model in VMAF_MODELS:
            res = step(f"vmaf:{model}", lambda m=model: pair.vmaf(m))
            if res:
                record["goldens"][f"{model}.vmaf"] = res[0]
                if pid == NETFLIX_PAIR and model == "vmaf_v0.6.1":
                    got = mean_of(res[1], "pooled", "vmaf")
                    record["checks"]["netflix_vmaf_mean"] = {
                        "got": got,
                        "want": NETFLIX_VMAF,
                    }
                    if abs(got - NETFLIX_VMAF) > NETFLIX_VMAF_TOLERANCE:
                        record["failed"]["check:netflix_vmaf_mean"] = (
                            f"pooled mean VMAF {got} differs from the Netflix reference {NETFLIX_VMAF}"
                        )
        res = step("vmaf:subsample", lambda: pair.vmaf("vmaf_v0.6.1", SUBSAMPLE))
        if res:
            record["goldens"][f"vmaf_v0.6.1-sub{SUBSAMPLE}.vmaf"] = res[0]
    if "x264ssim" in want and x264_exe:
        res = step("x264ssim", lambda: pair.x264ssim(x264_exe))
        if res:
            record["goldens"]["x264ssim"] = res[0]
        if pid == NETFLIX_PAIR:
            check = step("check:x264_log", lambda: x264_log_check(pair, x264_exe))
            if check:
                record["checks"]["x264_log"] = check
    if "daala" in want:
        files = step("daala", pair.daala)
        for key, name in (files or {}).items():
            record["goldens"][key] = name
    shutil.rmtree(work, ignore_errors=True)
    return record


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--out", type=Path, default=REPO / "tests" / "golden" / "metrics")
    p.add_argument("--only", help="comma-separated pair ids")
    p.add_argument(
        "--rd-points",
        type=Path,
        default=REPO / "tests" / "golden" / "metrics" / "rd_points",
    )
    p.add_argument(
        "--corpora-dir",
        type=Path,
        default=Path(os.environ.get("OVT_CORPORA_DIR", "corpora")),
    )
    p.add_argument(
        "--oracles",
        default=",".join(ALL_ORACLES),
        help="subset of " + ",".join(ALL_ORACLES),
    )
    p.add_argument(
        "--generate-syn",
        action="store_true",
        help="run tests/corpora/synthetic/generate.py for missing syn_ ids",
    )
    p.add_argument(
        "--frames",
        type=int,
        help="cut every pair to at most N pictures (default: per pair)",
    )
    p.add_argument("--work-dir", type=Path)
    p.add_argument(
        "--keep-going",
        action="store_true",
        help="exit 0 even when a step failed (CI report mode)",
    )
    p.add_argument("--list", action="store_true", help="print the plan and exit")
    args = p.parse_args(argv)
    args.oracles = [o for o in args.oracles.split(",") if o]
    if set(args.oracles) - set(ALL_ORACLES):
        p.error("unknown oracle: " + ",".join(sorted(set(args.oracles) - set(ALL_ORACLES))))
    args.only = [i for i in args.only.split(",") if i] if args.only else list(PAIRS)
    if set(args.only) - set(PAIRS):
        p.error("unknown pair: " + ",".join(sorted(set(args.only) - set(PAIRS))))
    args.out = args.out.resolve()
    args.corpora_dir = args.corpora_dir.resolve()
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.list:
        for pid in args.only:
            print(pid, PAIRS[pid]["ref"][0], PAIRS[pid]["frames"], " ".join(args.oracles))
        return EXIT_OK
    if args.generate_syn:
        wanted = sorted({PAIRS[p][s][0] for p in args.only for s in ("ref", "dist")
                         if PAIRS[p][s][0].startswith("syn_") and not (args.corpora_dir / PAIRS[p][s][0] / ".ok").is_file()})  # fmt: skip
        if wanted:
            run([sys.executable, "-I", str(SYNTH), "--only", ",".join(wanted), "--out", str(args.corpora_dir)], REPO,
                "generate.py", stdout=sys.stderr)  # fmt: skip
    args.out.mkdir(parents=True, exist_ok=True)
    args.work_root = args.work_dir or Path(tempfile.mkdtemp(prefix="gen_metrics_"))
    args.work_root.mkdir(parents=True, exist_ok=True)
    index: dict[str, Any] = {"pairs": {}}
    try:
        index["versions"] = versions()
        x264_exe = build_x264_ssim(args.work_root) if "x264ssim" in args.oracles else None
        for pid in args.only:
            try:
                index["pairs"][pid] = process(pid, args, x264_exe)
            except (StepError, MissingTool) as exc:
                index["pairs"][pid] = {
                    "failed": {"pair": str(exc)},
                    "goldens": {},
                    "skipped": {},
                    "checks": {},
                }
            print(f"{pid}: {'FAILED ' + ','.join(index['pairs'][pid]['failed']) if index['pairs'][pid]['failed'] else 'ok'}"
                  f" ({len(index['pairs'][pid]['goldens'])} goldens)", file=sys.stderr)  # fmt: skip
        if "bd" in args.oracles:
            try:
                index["bd"] = bd_goldens(args.out, args.rd_points, args.work_root)
            except StepError as exc:
                index["bd_failed"] = str(exc)
    except MissingTool as exc:
        print(f"error: not found (run this in the oracle image): {exc}", file=sys.stderr)
        return EXIT_MISSING
    finally:
        if args.work_dir is None:
            shutil.rmtree(args.work_root, ignore_errors=True)
    (args.out / "metrics-index.json").write_text(json.dumps(index, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    bad = {pid: r["failed"] for pid, r in index["pairs"].items() if r["failed"]}
    for pid, failed in bad.items():
        for label, why in failed.items():
            print(f"FAILED {pid} {label}: {why}", file=sys.stderr)
    if "bd_failed" in index:
        print(f"FAILED bd: {index['bd_failed']}", file=sys.stderr)
    return EXIT_OK if args.keep_going or (not bad and "bd_failed" not in index) else EXIT_STEP


if __name__ == "__main__":
    sys.exit(main())
