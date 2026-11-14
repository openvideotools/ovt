# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/gen_vmaf_ids.py (chapter 9 §9.5.3; WI-3.309) on a stand-in libvmaf tree."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import gen_vmaf_ids as gen  # noqa: E402

FEATURES = [
    "VMAF_integer_feature_adm2_score",
    "VMAF_integer_feature_motion2_score",
    "VMAF_integer_feature_vif_scale0_score",
    "VMAF_integer_feature_vif_scale1_score",
    "VMAF_integer_feature_vif_scale2_score",
    "VMAF_integer_feature_vif_scale3_score",
]
NEG_OPTS = [{"adm_enhn_gain_limit": 1.0}, {}] + [{"vif_enhn_gain_limit": 1.0}] * 4

MESON = """
if built_in_models_enabled
        model_files = [
            'vmaf_v0.6.1.json',
            'vmaf_b_v0.6.3.json',
            'vmaf_v0.6.1neg.json',
        ]
        if float_enabled
            model_files += [
              'vmaf_float_v0.6.1.json',
            ]
        endif
endif
"""


def make_tree(tmp_path: Path, meson: str = MESON, extra: dict | None = None) -> Path:
    (tmp_path / "libvmaf" / "src").mkdir(parents=True)
    (tmp_path / "libvmaf" / "src" / "meson.build").write_text(meson, encoding="utf-8")
    model = tmp_path / "model"
    model.mkdir()
    docs = {
        "vmaf_v0.6.1.json": {"model_dict": {"feature_names": FEATURES}},
        "vmaf_v0.6.1neg.json": {"model_dict": {"feature_names": FEATURES, "feature_opts_dicts": NEG_OPTS}},
        "vmaf_b_v0.6.3.json": {"0001": {"model_dict": {"feature_names": FEATURES}}},
        "vmaf_float_v0.6.1.json": {"model_dict": {"feature_names": [f.replace("integer_", "") for f in FEATURES]}},
    }
    docs.update(extra or {})
    for name, doc in docs.items():
        (model / name).write_text(json.dumps(doc), encoding="utf-8")
    return tmp_path


def test_ids_from_versions():
    assert gen.ovt_id("vmaf_v0.6.1") == "vmaf"
    assert gen.ovt_id("vmaf_v0.6.1neg") == "vmaf_neg"
    assert gen.ovt_id("vmaf_4k_v0.6.1neg") == "vmaf_4k_neg"
    assert gen.ovt_id("vmaf_float_b_v0.6.3") == "vmaf_float_b"
    with pytest.raises(gen.GenError):
        gen.ovt_id("vmaf_v0.6.1xyz")
    with pytest.raises(gen.GenError):
        gen.ovt_id("nomodel")


def test_describe_outputs_and_flags(tmp_path):
    entries = {e["id"]: e for e in gen.describe(make_tree(tmp_path))}
    assert list(entries) == ["vmaf", "vmaf_b", "vmaf_float", "vmaf_neg"]
    assert entries["vmaf"]["outputs"] == [
        "vmaf", "integer_adm2", "integer_adm_scale0", "integer_adm_scale1", "integer_adm_scale2",
        "integer_adm_scale3", "integer_motion2", "integer_motion", "integer_vif_scale0", "integer_vif_scale1",
        "integer_vif_scale2", "integer_vif_scale3",
    ]  # fmt: skip
    assert entries["vmaf_neg"]["outputs"][1] == "integer_adm2_egl_1"
    assert entries["vmaf_neg"]["outputs"][6:8] == ["integer_motion2", "integer_motion"]
    assert entries["vmaf_neg"]["outputs"][-1] == "integer_vif_scale3_egl_1"
    assert entries["vmaf_b"]["collection"] and entries["vmaf_b"]["outputs"] == gen._COLLECTION_OUTPUTS
    assert entries["vmaf_float"]["needs_float"] and not entries["vmaf"]["needs_float"]
    assert entries["vmaf_float"]["outputs"][1] == "adm2"


def test_unknown_option_or_feature_fails(tmp_path):
    bad = {"vmaf_v0.6.1.json": {"model_dict": {"feature_names": FEATURES, "feature_opts_dicts": [{"x": 1}] + [{}] * 5}}}
    with pytest.raises(gen.GenError, match="unknown feature option"):
        gen.describe(make_tree(tmp_path, extra=bad))


def test_unknown_feature_fails(tmp_path):
    bad = {"vmaf_v0.6.1.json": {"model_dict": {"feature_names": ["VMAF_feature_ansnr_score"]}}}
    with pytest.raises(gen.GenError, match="unknown feature"):
        gen.describe(make_tree(tmp_path, extra=bad))


def test_check_mode(tmp_path):
    root = make_tree(tmp_path / "tree")
    out = tmp_path / "vmaf_models.inc"
    assert gen.main(["--libvmaf-src", str(root), "-o", str(out)]) == 0
    text = out.read_text(encoding="utf-8")
    assert 'OVT_VMAF_MODEL("vmaf_b", "vmaf_b_v0.6.3", true, false, "bagging"' in text
    assert gen.main(["--libvmaf-src", str(root), "--check", str(out)]) == 0
    out.write_text(text.replace("vmaf_neg", "vmaf_xxx"), encoding="utf-8")
    assert gen.main(["--libvmaf-src", str(root), "--check", str(out)]) == 1
    assert gen.main(["--libvmaf-src", str(tmp_path / "missing"), "--check", str(out)]) == 2
