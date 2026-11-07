#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Export the google/uvq models to ONNX and validate them (chapter 9 §9.6.2; WI-3.314).

    export_uvq.py fetch    [--work DIR]
    export_uvq.py export   [--work DIR] [--out DIR] [--versions 1.5,1.0]
    export_uvq.py validate [--work DIR] [--out DIR] --clips DIR [--versions 1.5,1.0] [--report FILE]
    export_uvq.py manifest [--out DIR] [--base-url URL] [--manifest FILE] [--labels FILE]
    export_uvq.py all      ... (the four steps in order)

The pinned upstream is the PyTorch rewrite of UVQ (owner decision on #47), so the converter is the TorchScript
`torch.onnx.export` at opset 17, not tf2onnx (SPEC-ASSUMPTION(#47)). Every graph has fixed shapes and batch 1;
the host (WI-3.315) does the decoding, sampling, resizing and chunking described in
docs/dev/uvq_preprocessing.md and feeds one sampled frame (1.5) or one 1-second chunk (1.0) at a time.

`validate` scores each clip twice: once with the reference CLI (`uvq_inference.py --output_all_stats`, run as
a subprocess at UVQ_COMMIT) and once with ONNX Runtime CPU on the exported graphs, fed by the reference's own
preprocessing (`utils/video_reader.py` and `utils/probe.py`, imported from the pinned clone, not copied). The
pass criterion is |Δ MOS| ≤ 0.01 per clip for fp32 and ≤ 0.02 for fp16 (§9.6.2 step 4).

Run in the venv of tools/uvq_export/requirements.txt. TensorFlow and PyTorch are never runtime dependencies
of OVT; only the .onnx files and core/share/models/uvq/ ship.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
UVQ_REPO = "https://github.com/google/uvq.git"
UVQ_COMMIT = "c204ecc04f093e90c941cb126c7ac3d5beca0414"  # same pin as tests/oracles/Dockerfile (#84)
OPSET = 17
DEFAULT_WORK = REPO / "build" / "uvq_export"
DEFAULT_BASE_URL = f"https://github.com/openvideotools/ovt/releases/download/uvq-models-{UVQ_COMMIT[:8]}"
TOL_FP32 = 0.01
TOL_FP16 = 0.02
CLIP_IDS = [f"ytugc_clip{i:02d}" for i in range(1, 21)]

# Symbolics registered for ops the TorchScript exporter does not know. Each is recorded in report.json
# (§9.6.2 step 2: "document it in report.json").
CUSTOM_SYMBOLICS = [
    {
        "op": "aten::to_dense",
        "where": "PermuteLayerNHWC.forward in uvq1p5_pytorch/utils/custom_nn_layers.py and uvq_pytorch/utils/"
        "custom_nn_layers.py (DistortionNet heads)",
        "mapping": "identity: the tensor is already dense, so the op is a no-op and exports as nothing",
    }
]

# SPEC-ASSUMPTION(#246): UVQ 1.5 has no CompressionNet and a single per-frame output, so there is no
# uvq15_compression graph; the 1.0 graphs carry the seven combinations, the grids and the labels of §9.6.5.
# Graph name -> (version, role, input shapes). Shapes are NCHW (NCDHW for the 1.0 CompressionNet) float32;
# frame inputs are RGB in [-1, 1] (docs/dev/uvq_preprocessing.md).
GRAPHS: dict[str, tuple[str, str, dict[str, list[int]]]] = {
    "uvq15_content": ("1.5", "content", {"rgb": [1, 3, 1080, 1920]}),
    "uvq15_distortion": ("1.5", "distortion", {"rgb": [1, 3, 1080, 1920]}),
    "uvq15_aggregation": ("1.5", "aggregation", {"content": [1, 8, 8, 128], "distortion": [1, 24, 24, 128]}),
    "uvq10_content": ("1.0", "content", {"rgb": [1, 3, 496, 496]}),
    "uvq10_compression": ("1.0", "compression", {"rgb": [1, 3, 5, 720, 1280]}),
    "uvq10_distortion": ("1.0", "distortion", {"rgb": [1, 3, 720, 1280]}),
    "uvq10_aggregation": (
        "1.0",
        "aggregation",
        {"compression": [1, 16, 16, 100], "content": [1, 16, 16, 100], "distortion": [1, 16, 16, 100]},
    ),
}
OUTPUTS: dict[str, list[str]] = {
    "uvq15_content": ["features"],
    "uvq15_distortion": ["features"],
    "uvq15_aggregation": ["mos"],
    "uvq10_content": ["features", "label_probs"],
    "uvq10_compression": ["features", "compression_level"],
    "uvq10_distortion": ["features", "distortion_probs"],
    "uvq10_aggregation": ["scores"],
}
# The seven README combinations, in the order of the uvq10_aggregation `scores` output and of the
# FrameScore values of §9.6.5 after `mos` (`mos` is compression_content_distortion).
UVQ10_COMBOS = [
    "compression",
    "content",
    "distortion",
    "compression_content",
    "compression_distortion",
    "content_distortion",
    "compression_content_distortion",
]


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def uvq_dir(work: Path) -> Path:
    return work / "uvq"


# ---------------------------------------------------------------------------------------------------------------
# Step 1: clone at UVQ_COMMIT and verify the checkpoints.


def read_weight_hashes() -> dict[str, str]:
    out = {}
    for line in (HERE / "weights.sha256").read_text(encoding="utf-8").splitlines():
        if line.strip() and not line.startswith("#"):
            digest, name = line.split(maxsplit=1)
            out[name.strip()] = digest
    return out


def fetch(work: Path) -> Path:
    src = uvq_dir(work)
    if not (src / ".git").is_dir():
        work.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "--quiet", UVQ_REPO, str(src)], check=True)
    subprocess.run(["git", "-C", str(src), "checkout", "--quiet", UVQ_COMMIT], check=True)
    head = subprocess.run(["git", "-C", str(src), "rev-parse", "HEAD"], check=True, capture_output=True, text=True)
    if head.stdout.strip() != UVQ_COMMIT:
        sys.exit(f"E-COMMIT {src} is at {head.stdout.strip()}, expected {UVQ_COMMIT}")
    bad = [name for name, digest in read_weight_hashes().items() if sha256(src / name) != digest]
    if bad:
        sys.exit("E-WEIGHTS hash mismatch: " + ", ".join(bad))
    print(f"ok  google/uvq {UVQ_COMMIT[:8]}, {len(read_weight_hashes())} checkpoint files verified")
    return src


def import_upstream(src: Path) -> None:
    """Make the pinned clone importable (`uvq1p5_pytorch`, `uvq_pytorch`, `utils.probe`, `video_reader`)."""
    for p in (str(src / "utils"), str(src)):
        if p not in sys.path:
            sys.path.insert(0, p)


# ---------------------------------------------------------------------------------------------------------------
# Step 2-4: wrappers with fixed shapes, export, fp16, checker and a torch-vs-ORT check.


def build_modules(src: Path, versions: list[str]) -> dict[str, Any]:
    import torch
    from torch import nn

    import_upstream(src)
    mods: dict[str, Any] = {}

    if "1.5" in versions:
        from uvq1p5_pytorch.utils import uvq1p5

        u15 = uvq1p5.UVQ1p5()

        class Frame15(nn.Module):
            """ContentNet/DistortionNet of 1.5 on one frame (the upstream forward takes [N, F, C, H, W])."""

            def __init__(self, net: nn.Module) -> None:
                super().__init__()
                self.net = net

            def forward(self, rgb: torch.Tensor) -> torch.Tensor:
                return self.net(rgb.unsqueeze(1))

        class Agg15(nn.Module):
            def __init__(self, net: nn.Module) -> None:
                super().__init__()
                self.net = net

            def forward(self, content: torch.Tensor, distortion: torch.Tensor) -> torch.Tensor:
                return self.net(content, distortion)["uvq_1p5_features"]

        mods["uvq15_content"] = Frame15(u15.content_net)
        mods["uvq15_distortion"] = Frame15(u15.distortion_net)
        mods["uvq15_aggregation"] = Agg15(u15.aggregation_net)

    if "1.0" in versions:
        from uvq_pytorch.utils import aggregationnet, compressionnet, contentnet, distortionnet

        c10 = contentnet.ContentNetInference().model
        k10 = compressionnet.CompressionNetInference().model
        d10 = distortionnet.DistortionNetInference().model
        agg = aggregationnet.AggregationNetInference()

        class Content10(nn.Module):
            def __init__(self) -> None:
                super().__init__()
                self.net = c10

            def forward(self, rgb: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
                features, probs = self.net(rgb)
                return features.permute(0, 2, 3, 1), probs

        class Distortion10(nn.Module):
            """2x2 grid of 360x640 patches in one batch; same tiling as DistortionNetInference's loops."""

            def __init__(self) -> None:
                super().__init__()
                self.net = d10

            def forward(self, rgb: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
                patches = rgb.reshape(3, 2, 360, 2, 640).permute(1, 3, 0, 2, 4).reshape(4, 3, 360, 640)
                features, probs = self.net(patches)  # [4, 8, 8, 100] NHWC, [4, 26]
                grid = features.reshape(2, 2, 8, 8, 100).permute(0, 2, 1, 3, 4).reshape(1, 16, 16, 100)
                return grid, probs.reshape(1, 2, 2, 26)

        class Compression10(nn.Module):
            """4x4 grid of 5x180x320 patches; the per-patch label is CompressionNet.forward's on batch 1."""

            def __init__(self) -> None:
                super().__init__()
                self.net = k10

            def forward(self, rgb: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
                p = rgb.reshape(3, 5, 4, 180, 4, 320).permute(2, 4, 0, 1, 3, 5).reshape(16, 3, 5, 180, 320)
                n = self.net
                inc = n.final_conv3d(n.inception_block1(p))  # [16, 100, 1, 4, 4]
                features = inc.squeeze(2).permute(0, 2, 3, 1)  # [16, 4, 4, 100]
                level = torch.sigmoid(n.conv3d_0c(n.avgpool_3d(inc)).mean(dim=(1, 2)))  # [16, 1, 1]
                grid = features.reshape(4, 4, 4, 4, 100).permute(0, 2, 1, 3, 4).reshape(1, 16, 16, 100)
                return grid, level.reshape(1, 4, 4, 1)

        class Agg10(nn.Module):
            """All 7 x 5 aggregation heads; output [1, 7] = per-combination mean over the five folds."""

            def __init__(self) -> None:
                super().__init__()
                self.names = [name for name, _ in agg.get_model_names_iterator()]
                self.heads = nn.ModuleList([agg.models[name] for name in self.names])

            def forward(self, compression: torch.Tensor, content: torch.Tensor, distortion: torch.Tensor) -> Any:
                feats = {
                    "compression": compression.permute(0, 3, 1, 2),
                    "content": content.permute(0, 3, 1, 2),
                    "distortion": distortion.permute(0, 3, 1, 2),
                }
                outs = [head(feats) for head in self.heads]  # each [1] (the head means over its batch of 1)
                per_combo = torch.stack(outs).reshape(len(UVQ10_COMBOS), 5)
                return per_combo.mean(dim=1).reshape(1, len(UVQ10_COMBOS))

        mods["uvq10_content"] = Content10()
        mods["uvq10_compression"] = Compression10()
        mods["uvq10_distortion"] = Distortion10()
        mods["uvq10_aggregation"] = Agg10()

    for m in mods.values():
        m.eval()
    return mods


def graph_names(versions: list[str]) -> list[str]:
    return [g for g, (v, _, _) in GRAPHS.items() if v in versions]


def export(work: Path, out: Path, versions: list[str]) -> dict[str, Any]:
    import numpy as np
    import onnx
    import onnxruntime as ort
    import torch
    from onnxconverter_common import float16

    torch.manual_seed(0)
    torch.onnx.register_custom_op_symbolic("aten::to_dense", lambda g, x, *rest: x, OPSET)
    src = fetch(work)
    mods = build_modules(src, versions)
    out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(20261107)
    info: dict[str, Any] = {}
    for name in graph_names(versions):
        _, _, inputs = GRAPHS[name]
        args = tuple(torch.from_numpy(rng.uniform(-1, 1, shape).astype(np.float32)) for shape in inputs.values())
        fp32 = out / f"{name}_fp32.onnx"
        with torch.no_grad():
            torch.onnx.export(
                mods[name],
                args,
                str(fp32),
                dynamo=False,
                opset_version=OPSET,
                input_names=list(inputs),
                output_names=OUTPUTS[name],
                do_constant_folding=True,
            )
            ref = mods[name](*args)
        ref_list = [ref] if isinstance(ref, torch.Tensor) else list(ref)
        model = onnx.load(str(fp32))
        model.producer_name = "ovt tools/uvq_export"
        model.producer_version = UVQ_COMMIT[:8]
        model.doc_string = f"google/uvq {UVQ_COMMIT} ({name}), Apache-2.0; exported by OVT WI-3.314"
        onnx.save(model, str(fp32))
        onnx.checker.check_model(str(fp32), full_check=True)
        fp16 = out / f"{name}_fp16.onnx"
        onnx.save(float16.convert_float_to_float16(model, keep_io_types=True), str(fp16))
        onnx.checker.check_model(str(fp16), full_check=True)
        feeds = {k: a.numpy() for k, a in zip(inputs, args)}
        entry: dict[str, Any] = {"opset": OPSET, "inputs": inputs, "outputs": OUTPUTS[name]}
        for precision, path in (("fp32", fp32), ("fp16", fp16)):
            sess = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])
            got = sess.run(None, feeds)
            diff = max(float(np.max(np.abs(g - r.numpy()))) for g, r in zip(got, ref_list))
            scale = max(float(np.max(np.abs(r.numpy()))) for r in ref_list)
            entry[precision] = {
                "bytes": path.stat().st_size,
                "sha256": sha256(path),
                "max_abs_diff_vs_torch": diff,
                "max_abs_torch_output": scale,
            }
        info[name] = entry
        print(
            f"ok  {name}: |out| ≤ {entry['fp32']['max_abs_torch_output']:.3g}, "
            f"fp32 Δ {entry['fp32']['max_abs_diff_vs_torch']:.2e}, fp16 Δ {entry['fp16']['max_abs_diff_vs_torch']:.2e}"
        )
    (out / "export.json").write_text(json.dumps(info, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    # Determinism (§0.7.5): the pinned venv reproduces the committed manifest's hashes byte for byte.
    committed = REPO / "core" / "share" / "models" / "uvq" / "manifest.json"
    if committed.is_file():
        want = {f["name"]: f["sha256"] for f in json.loads(committed.read_text(encoding="utf-8"))["files"]}
        for name, entry in info.items():
            for precision in ("fp32", "fp16"):
                file = f"{name}_{precision}.onnx"
                if file in want and want[file] != entry[precision]["sha256"]:
                    print(f"W-HASH {file} differs from {committed.relative_to(REPO)}; the export is not reproduced")
    return info


# ---------------------------------------------------------------------------------------------------------------
# Step 5: validation on the YouTube-UGC clips.


def find_clip(clips: Path, clip_id: str) -> Path:
    found = sorted(p for p in (clips / clip_id).glob("*.mp4"))
    if not found:
        sys.exit(f"E-CLIP {clip_id} not under {clips} (tools/fetch_corpora.py --ids {clip_id})")
    return found[0]


def reference_cli(src: Path, clip: Path, version: str, threads: int) -> dict[str, Any]:
    cmd = [
        sys.executable,
        str(src / "uvq_inference.py"),
        str(clip),
        "--model_version",
        version,
        "--output_all_stats",
        "--threads",
        str(threads),
    ]
    env = dict(os.environ, OMP_NUM_THREADS=str(threads))
    res = subprocess.run(cmd, cwd=src, env=env, check=True, capture_output=True, text=True)
    line = [ln for ln in res.stdout.splitlines() if ln.startswith("{")][-1]
    return json.loads(line)


def ffmpeg_version() -> str:
    """First line of `ffmpeg -version`: the reference's frame bytes depend on the FFmpeg on PATH."""
    res = subprocess.run(["ffmpeg", "-version"], check=True, capture_output=True, text=True)
    return res.stdout.splitlines()[0]


class Sessions:
    def __init__(self, out: Path, precision: str, names: list[str], threads: int) -> None:
        import onnxruntime as ort

        opts = ort.SessionOptions()
        opts.intra_op_num_threads = threads
        self.s = {
            n: ort.InferenceSession(str(out / f"{n}_{precision}.onnx"), opts, providers=["CPUExecutionProvider"])
            for n in names
        }

    def run(self, name: str, **feeds: Any) -> list[Any]:
        return self.s[name].run(None, feeds)


def ort_uvq15(src: Path, clip: Path, sess: Sessions) -> dict[str, Any]:
    """Mirrors uvq_inference.run_single_inference + UVQ1p5.infer at the defaults (fps 1, no transpose)."""
    import numpy as np
    import probe  # type: ignore[import-not-found]  # upstream utils/probe.py
    import video_reader  # type: ignore[import-not-found]  # upstream utils/video_reader.py

    duration = probe.get_video_duration(str(clip))
    video_length = math.ceil(duration)
    scores = []
    for batch, _ in video_reader.iter_video_batches_1p5(str(clip), video_length, False, video_fps=1, batch_size=1):
        rgb = np.ascontiguousarray(batch.transpose(0, 3, 1, 2))
        (content,) = sess.run("uvq15_content", rgb=rgb)
        (distortion,) = sess.run("uvq15_distortion", rgb=rgb)
        (mos,) = sess.run("uvq15_aggregation", content=content, distortion=distortion)
        scores.append(float(mos.reshape(-1)[0]))
    return {"mos": float(np.mean(np.asarray(scores, dtype=np.float32))), "per_frame": scores}


def ort_uvq10(src: Path, clip: Path, sess: Sessions) -> dict[str, Any]:
    """Mirrors UVQ1p0.infer: 5 fps, 1-second chunks; content and distortion use frame 0 of each chunk."""
    import numpy as np
    import probe  # type: ignore[import-not-found]
    import video_reader  # type: ignore[import-not-found]

    video_length = math.ceil(probe.get_video_duration(str(clip)))
    video, small = video_reader.load_video_1p0(str(clip), video_length, False)
    video = video.transpose(0, 4, 1, 2, 3).astype(np.float32)  # [sec, C, F, H, W]
    small = small.transpose(0, 1, 4, 2, 3).astype(np.float32)  # [sec, F, C, H, W]
    chunks, top1 = [], []
    for k in range(video_length):
        comp, _ = sess.run("uvq10_compression", rgb=np.ascontiguousarray(video[k : k + 1]))
        cont, probs = sess.run("uvq10_content", rgb=np.ascontiguousarray(small[k : k + 1, 0]))
        dist, _ = sess.run("uvq10_distortion", rgb=np.ascontiguousarray(video[k : k + 1, :, 0]))
        (scores,) = sess.run("uvq10_aggregation", compression=comp, content=cont, distortion=dist)
        chunks.append(scores.reshape(-1))
        top1.append(int(np.argmax(probs)))
    mean = np.mean(np.stack(chunks), axis=0)
    out: dict[str, Any] = {c: float(v) for c, v in zip(UVQ10_COMBOS, mean)}
    out["mos"] = out["compression_content_distortion"]
    out["top1_labels"] = top1
    return out


def torch_uvq10_top1(src: Path, clip: Path) -> list[int]:
    """Per-chunk argmax of the reference ContentNet (the CLI does not print labels)."""
    import numpy as np
    import probe  # type: ignore[import-not-found]
    import video_reader  # type: ignore[import-not-found]
    from uvq_pytorch.utils import contentnet

    video_length = math.ceil(probe.get_video_duration(str(clip)))
    _, small = video_reader.load_video_1p0(str(clip), video_length, False)
    small = small.transpose(0, 1, 4, 2, 3)
    _, labels = contentnet.ContentNetInference().get_labels_and_features_for_all_frames(small)
    return [int(i) for i in np.argmax(labels, axis=1)]


def validate(
    work: Path, out: Path, clips: Path, versions: list[str], report_path: Path, threads: int, ids: list[str]
) -> int:
    import onnxruntime as ort
    import torch

    src = fetch(work)
    import_upstream(src)
    torch.set_num_threads(threads)
    exported = json.loads((out / "export.json").read_text(encoding="utf-8"))
    report: dict[str, Any] = {
        "uvq_commit": UVQ_COMMIT,
        "opset": OPSET,
        "converter": f"torch.onnx.export (TorchScript) torch {torch.__version__}",
        "onnxruntime": ort.__version__,
        "ffmpeg": ffmpeg_version(),
        "execution_provider": "CPUExecutionProvider",
        "python": platform.python_version(),
        "tolerance": {"fp32": TOL_FP32, "fp16": TOL_FP16},
        "models": exported,
        "custom_ops": CUSTOM_SYMBOLICS,
        "clips": {},
    }
    failures = 0
    for version in versions:
        names = graph_names([version])
        sessions = {p: Sessions(out, p, names, threads) for p in ("fp32", "fp16")}
        for clip_id in ids:
            clip = find_clip(clips, clip_id)
            ref = reference_cli(src, clip, version, threads)
            ref_mos = float(ref["uvq1p5_score"] if version == "1.5" else ref["compression_content_distortion"])
            row: dict[str, Any] = {"file": clip.name, "reference_mos": ref_mos}
            for precision, sess in sessions.items():
                got = ort_uvq15(src, clip, sess) if version == "1.5" else ort_uvq10(src, clip, sess)
                delta = got["mos"] - ref_mos
                tol = TOL_FP32 if precision == "fp32" else TOL_FP16
                cell: dict[str, Any] = {"mos": got["mos"], "delta": delta, "pass": abs(delta) <= tol}
                if version == "1.5":
                    cell["max_frame_delta"] = max(abs(a - b) for a, b in zip(got["per_frame"], ref["per_frame_scores"]))
                else:
                    cell["max_combo_delta"] = max(abs(got[c] - float(ref[c])) for c in UVQ10_COMBOS)
                    if precision == "fp32":
                        row["top1_labels_match"] = got["top1_labels"] == torch_uvq10_top1(src, clip)
                        cell["pass"] = cell["pass"] and row["top1_labels_match"]
                row[precision] = cell
                failures += 0 if cell["pass"] else 1
            report["clips"].setdefault(version, {})[clip_id] = row
            print(
                f"{'ok ' if row['fp32']['pass'] and row['fp16']['pass'] else 'BAD'} {version} {clip_id} "
                f"ref {ref_mos:.4f} Δfp32 {row['fp32']['delta']:+.5f} Δfp16 {row['fp16']['delta']:+.5f}"
            )
    for version in versions:
        rows = report["clips"].get(version, {}).values()
        report.setdefault("summary", {})[version] = {
            p: {"max_abs_delta": max(abs(r[p]["delta"]) for r in rows), "all_pass": all(r[p]["pass"] for r in rows)}
            for p in ("fp32", "fp16")
        }
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {report_path}; {failures} failing cells")
    return 1 if failures else 0


# ---------------------------------------------------------------------------------------------------------------
# Step 6: manifest (§9.6.3) and the YouTube-8M label names (§9.6.5).


def write_manifest(out: Path, base_url: str, manifest: Path, labels: Path, work: Path) -> None:
    exported = json.loads((out / "export.json").read_text(encoding="utf-8"))
    files = []
    for name in GRAPHS:
        if name not in exported:
            continue
        version, role, inputs = GRAPHS[name]
        for precision in ("fp32", "fp16"):
            e = exported[name][precision]
            files.append(
                {
                    "name": f"{name}_{precision}.onnx",
                    "url": f"{base_url}/{name}_{precision}.onnx",
                    "bytes": e["bytes"],
                    "sha256": e["sha256"],
                    "precision": precision,
                    "uvq_version": version,
                    "role": role,
                    "inputs": inputs,
                    "outputs": OUTPUTS[name],
                }
            )
    # SPEC-ASSUMPTION(#246): both generations in one manifest; "versions", "opset", "labels" and the per-file
    # "uvq_version", "role", "inputs" and "outputs" keys are additions to the §9.6.3 example.
    doc = {
        "schema": 1,
        "model": "uvq",
        "version": "1.5",
        "versions": sorted({GRAPHS[n][0] for n in exported}, reverse=True),
        "source": "https://github.com/google/uvq",
        "commit": UVQ_COMMIT,
        "license": "Apache-2.0",
        "opset": OPSET,
        "labels": "labels_yt8m.txt",
        "files": files,
    }
    manifest.parent.mkdir(parents=True, exist_ok=True)
    text = re.sub(
        r"\[\s+([0-9,\s]+?)\s+\]",
        lambda m: "[" + ", ".join(m.group(1).split()).replace(",,", ",") + "]",
        json.dumps(doc, indent=2),
    )
    manifest.write_text(text + "\n", encoding="utf-8")
    import csv

    with (uvq_dir(work) / "uvq_pytorch" / "checkpoint" / "contentnet_labels.csv").open(encoding="utf-8") as f:
        # 56 classes have no Name upstream; their Knowledge Graph id (/m/...) stands in, so line i+1 is class i.
        rows = sorted((int(r["Index"]), r["Name"].strip() or r["KnowledgeGraphId"]) for r in csv.DictReader(f))
    if [i for i, _ in rows] != list(range(len(rows))):
        sys.exit("E-LABELS contentnet_labels.csv indices are not 0..N-1")
    labels.write_text("".join(f"{name}\n" for _, name in rows), encoding="utf-8")
    print(f"wrote {manifest} ({len(files)} files) and {labels} ({len(rows)} labels)")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("step", choices=["fetch", "export", "validate", "manifest", "all"])
    ap.add_argument("--work", type=Path, default=DEFAULT_WORK, help="clone and output root")
    ap.add_argument("--out", type=Path, help="directory of the .onnx files (default <work>/onnx)")
    ap.add_argument("--versions", default="1.5,1.0")
    ap.add_argument("--clips", type=Path, help="fetch_corpora.py cache holding ytugc_clip01..20")
    ap.add_argument("--ids", default=",".join(CLIP_IDS), help="clip ids to validate (default the 20 of §9.6.2)")
    ap.add_argument("--report", type=Path, default=HERE / "report.json")
    ap.add_argument("--threads", type=int, default=min(4, os.cpu_count() or 1))
    ap.add_argument("--base-url", default=DEFAULT_BASE_URL)
    ap.add_argument("--manifest", type=Path, default=REPO / "core" / "share" / "models" / "uvq" / "manifest.json")
    ap.add_argument("--labels", type=Path, default=REPO / "core" / "share" / "models" / "uvq" / "labels_yt8m.txt")
    a = ap.parse_args(argv)
    versions = [v for v in a.versions.split(",") if v]
    if any(v not in ("1.5", "1.0") for v in versions):
        ap.error("--versions takes 1.5 and/or 1.0")
    out = a.out or a.work / "onnx"
    if a.step in ("validate", "all") and a.clips is None:
        ap.error("validate needs --clips")
    rc = 0
    if a.step == "fetch":
        fetch(a.work)
    if a.step in ("export", "all"):
        export(a.work, out, versions)
    if a.step in ("validate", "all"):
        rc = validate(a.work, out, a.clips, versions, a.report, a.threads, [i for i in a.ids.split(",") if i])
    if a.step in ("manifest", "all"):
        write_manifest(out, a.base_url, a.manifest, a.labels, a.work)
    return rc


if __name__ == "__main__":
    sys.exit(main())
