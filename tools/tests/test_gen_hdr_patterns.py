# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/gen_hdr_patterns.py (chapter 10 section 10.9.2; WI-1.413)."""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parent.parent
REPO = TOOLS.parent
sys.path.insert(0, str(TOOLS))

pytest.importorskip("numpy")  # NumPy is a pytest-lane requirement (tools/requirements-test.txt)

import gen_hdr_patterns as gen  # noqa: E402

PATTERN_DIR = REPO / "tests/corpora/patterns"
MANIFEST = REPO / "tests/corpora/manifest.json"


def _has_libx265() -> bool:
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        return False
    out = subprocess.run(
        [ffmpeg, "-hide_banner", "-encoders"],
        capture_output=True,
        text=True,
        check=False,
    )
    return "libx265" in out.stdout


needs_x265 = pytest.mark.skipif(not _has_libx265(), reason="ffmpeg with libx265 not available")


def test_pq_codes_match_the_spec() -> None:
    """The formula reproduces the codes of section 10.4.4 (limited and full range)."""
    assert [int(gen.luma_code(gen.pq_oetf(v))) for v in gen.PQ_NITS] == [
        195,
        509,
        573,
        723,
        855,
        940,
    ]
    assert [int(gen.full_luma_code(gen.pq_oetf(v))) for v in gen.PQ_NITS] == [
        153,
        520,
        594,
        769,
        923,
        1023,
    ]


def test_pq_eotf_inverts_oetf() -> None:
    for v in gen.PQ_NITS:
        assert float(gen.pq_eotf(gen.pq_oetf(v))) == pytest.approx(v, rel=1e-9)


def test_hlg_75_percent_is_203_nits() -> None:
    assert float(gen.hlg_display_nits_grey(0.75)) == pytest.approx(203.0, rel=0.01)
    assert float(gen.hlg_display_nits_grey(1.0)) == pytest.approx(1000.0, rel=1e-6)


def test_pq_expected_json_content() -> None:
    """Acceptance 2: nits per patch, limited codes and FALL = area-weighted mean."""
    exp = json.loads((PATTERN_DIR / "pq_patches_1000.expected.json").read_text(encoding="utf-8"))
    assert exp["nits"] == [1, 100, 203, 1000, 4000, 10000]
    assert exp["codes_limited"] == [195, 509, 573, 723, 855, 940]
    assert exp["fall_nits"] == pytest.approx(sum(exp["nits"]) / 6)
    assert exp["max_cll_nits"] == 10000
    assert sum(exp["band_fractions"]) == pytest.approx(1.0)


def test_p3_saturation_has_saturated_area() -> None:
    exp = json.loads((PATTERN_DIR / "p3_saturation.expected.json").read_text(encoding="utf-8"))
    assert exp["saturated_area_samples"] > 0
    assert exp["out_of_gamut_709_samples"] == exp["saturated_area_samples"]


def test_manifest_has_every_pattern_entry() -> None:
    """Acceptance 3 (until tools/check_fast_subset.py of WI-0.553 exists): entry, tag and SHA-256 per pattern."""
    entries = {e["id"]: e for e in json.loads(MANIFEST.read_text(encoding="utf-8"))["entries"]}
    for name in gen.PATTERNS:
        entry = entries[f"syn_pat_{name}"]
        mp4 = PATTERN_DIR / f"{name}.mp4"
        assert "patterns" in entry["tags"]
        assert entry["source"] == "syn"
        assert entry["sha256"] == hashlib.sha256(mp4.read_bytes()).hexdigest()
        assert entry["size_bytes"] == mp4.stat().st_size
        assert (PATTERN_DIR / f"{name}.expected.json").is_file()


def test_manifest_validates_against_schema() -> None:
    jsonschema = pytest.importorskip("jsonschema")
    schema = json.loads((REPO / "tests/corpora/manifest.schema.json").read_text(encoding="utf-8"))
    jsonschema.validate(json.loads(MANIFEST.read_text(encoding="utf-8")), schema)


@needs_x265
def test_run_twice_is_byte_identical(tmp_path: Path) -> None:
    """Acceptance 1, on the two smallest patterns to keep the fast lane quick."""
    for name in ("pq_patches_1000", "hlg_patches"):
        a, b = tmp_path / "a", tmp_path / "b"
        gen.generate(a, [name], None)
        gen.generate(b, [name], None)
        for suffix in (".y4m", ".mp4", ".expected.json"):
            assert (a / f"{name}{suffix}").read_bytes() == (b / f"{name}{suffix}").read_bytes(), suffix


@needs_x265
def test_committed_files_match_generator() -> None:
    pin = json.loads((PATTERN_DIR / "encoder.json").read_text(encoding="utf-8"))
    res = subprocess.run(
        [sys.executable, "-I", str(TOOLS / "gen_hdr_patterns.py"), "--check"],
        capture_output=True,
        text=True,
        check=False,
    )
    if res.returncode == gen.EXIT_ENCODER_MISMATCH:
        pytest.skip(f"local x265 differs from the pinned {pin['x265']}")
    assert res.returncode == 0, res.stderr
