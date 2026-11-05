# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/embed_bfbs.py (WI-0.083)."""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import embed_bfbs  # noqa: E402


def _arrays(text: str) -> dict[str, bytes]:
    """Parse the generated fragment back into {array name: bytes}."""
    out: dict[str, bytes] = {}
    for name, body in re.findall(r"constexpr std::uint8_t (\w+)\[\] = \{(.*?)\};", text, re.DOTALL):
        out[name] = bytes(int(tok, 16) for tok in re.findall(r"0x([0-9a-f]{2})", body))
    return out


def test_embeds_every_input_byte_exact(tmp_path: Path) -> None:
    a = tmp_path / "ovt_model.bfbs"
    b = tmp_path / "ovt_container.bfbs"
    a.write_bytes(bytes(range(256)) * 3 + b"\x01")
    b.write_bytes(b"BFBS")
    out = tmp_path / "gen" / "res.inc"
    assert embed_bfbs.main(["--out", str(out), str(a), str(b)]) == 0
    text = out.read_text(encoding="utf-8")
    assert _arrays(text) == {"k_ovt_model_bfbs": a.read_bytes(), "k_ovt_container_bfbs": b"BFBS"}
    assert "#define OVT_BFBS_HAVE_OVT_MODEL 1" in text
    assert "#define OVT_BFBS_HAVE_OVT_CONTAINER 1" in text
    assert "OVT_BFBS_HAVE_OVT_METRICS" not in text
    assert text.count("alignas(8)") == 2
    assert str(tmp_path) not in text  # no paths: the bytes do not depend on the checkout


def test_output_is_deterministic_and_not_rewritten(tmp_path: Path) -> None:
    src = tmp_path / "s.bfbs"
    src.write_bytes(b"\x00\xff" * 40)
    out = tmp_path / "res.inc"
    assert embed_bfbs.main(["--out", str(out), str(src)]) == 0
    first = out.read_bytes()
    os.utime(out, ns=(1, 1))
    assert embed_bfbs.main(["--out", str(out), str(src)]) == 0
    assert out.read_bytes() == first
    assert out.stat().st_mtime_ns == 1  # unchanged content: file left alone, nothing recompiles
    src.write_bytes(b"\x01")
    assert embed_bfbs.main(["--out", str(out), str(src)]) == 0
    assert _arrays(out.read_text(encoding="utf-8")) == {"k_s_bfbs": b"\x01"}


def test_rejects_bad_inputs(tmp_path: Path) -> None:
    out = tmp_path / "res.inc"
    good = tmp_path / "ok.bfbs"
    good.write_bytes(b"x")
    empty = tmp_path / "empty.bfbs"
    empty.write_bytes(b"")
    wrong_suffix = tmp_path / "ok.bin"
    wrong_suffix.write_bytes(b"x")
    bad_stem = tmp_path / "1-bad.bfbs"
    bad_stem.write_bytes(b"x")
    assert embed_bfbs.main(["--out", str(out), str(tmp_path / "missing.bfbs")]) == 2
    assert embed_bfbs.main(["--out", str(out), str(empty)]) == 2
    assert embed_bfbs.main(["--out", str(out), str(wrong_suffix)]) == 2
    assert embed_bfbs.main(["--out", str(out), str(bad_stem)]) == 2
    assert embed_bfbs.main(["--out", str(out), str(good), str(good)]) == 2
    assert not out.exists()
