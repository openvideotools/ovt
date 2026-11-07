#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""google/uvq ``uvq_inference.py --output <file>`` JSON -> ``scores`` (chapter 14 §14.4.14, WI-0.555).

SPEC-ASSUMPTION(#47): the oracle is the PyTorch revision pinned in ``tests/oracles/Dockerfile``, run as
``python uvq_inference.py <clip> --model_version 1.5|1.0 --device cpu --output <id>.uvq.json``; it writes
the result dictionary of ``uvq1p5_pytorch/utils/uvq1p5.py`` or ``uvq_pytorch/utils/uvq1p0.py`` plus
``video_name``. (§14.4.14 still describes the TensorFlow CLI and its per-second CSV.)

* UVQ 1.5 (``uvq1p5_score``, ``per_frame_scores``, ``frame_indices``): one frame entry per sampled frame,
  ``{"i": frame_indices[k], "uvq": per_frame_scores[k]}`` (``i`` is the frame index in the source, as
  UVQ computes it from ``--fps``), and ``pooled.uvq.mean`` = ``uvq1p5_score`` (the mean UVQ takes).
* UVQ 1.0 (``compression_content_distortion`` and the other sub-network scores): no per-frame values, so
  ``frames`` is empty and every numeric key goes to ``pooled.uvq``; the MOS is
  ``pooled.uvq.compression_content_distortion``.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canon  # noqa: E402


def score(value: Any, what: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise canon.NormalizeError(f"{what}: {value!r} is not a number")
    return canon.to_score(repr(float(value)), what)


def convert(args: argparse.Namespace) -> dict[str, Any]:
    doc = canon.read_json(args.input)
    if not isinstance(doc, dict):
        raise canon.NormalizeError("expected a JSON object")
    if "uvq1p5_score" in doc:
        scores, indices = doc.get("per_frame_scores"), doc.get("frame_indices")
        if not isinstance(scores, list) or not isinstance(indices, list) or len(scores) != len(indices):
            raise canon.NormalizeError("per_frame_scores and frame_indices must be lists of the same length")
        if len(set(indices)) != len(indices) or not all(isinstance(i, int) and i >= 0 for i in indices):
            raise canon.NormalizeError("frame_indices must be distinct non-negative integers")
        frames = [{"i": i, "uvq": score(s, f"frame {i}")} for i, s in zip(indices, scores)]
        return {"frames": frames, "pooled": {"uvq": {"mean": score(doc["uvq1p5_score"], "uvq1p5_score")}}}
    if "compression_content_distortion" in doc:
        pooled = {k: score(v, k) for k, v in doc.items() if k != "video_name"}
        return {"frames": [], "pooled": {"uvq": pooled}}
    raise canon.NormalizeError("neither a UVQ 1.5 (uvq1p5_score) nor a UVQ 1.0 result")


if __name__ == "__main__":
    sys.exit(canon.run("scores", __doc__.splitlines()[0], convert))
