# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""JSON Schemas of the `-f json` output (chapter 12 §12.6, WI-1.501).

Run by CTest as `cli_schema_validate` (labels fast;cli) with
  OVT_CLI    the built `ovt` executable (AT1 runs it over the fast MP4 corpus subset)
  OVT_FLATC  the flatc the build uses (AT2 regenerates the schemas from a modified .fbs)
Either test is skipped when its tool or input is missing; OVT_REQUIRE_CORPORA=1 turns a missing corpus
into a failure, as for the golden tests.
"""

from __future__ import annotations

import base64
import copy
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

import pytest

jsonschema = pytest.importorskip("jsonschema")

ROOT = Path(__file__).resolve().parents[2]
SCHEMAS = ROOT / "docs" / "spec" / "schemas"
ENVELOPE = SCHEMAS / "ovt-cli-1.schema.json"
GENERATOR = ROOT / "tools" / "fbs_to_jsonschema.py"
sys.path.insert(0, str(ROOT / "tools"))

import fbs_to_jsonschema  # noqa: E402

# The commands whose output AT1 validates (WI-1.501 step 4).
AT1_COMMANDS = ("info", "probe", "syntax", "frames", "index")


def generated_files() -> list[Path]:
    names = [t + ".schema.json" for t in fbs_to_jsonschema.TABLE_FILES] + [fbs_to_jsonschema.ENVELOPE_FILE]
    return [SCHEMAS / n for n in sorted(names)]


def load(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def validator(path: Path = ENVELOPE) -> Any:
    schema = load(path)
    return jsonschema.Draft202012Validator(schema, format_checker=jsonschema.Draft202012Validator.FORMAT_CHECKER)


def errors(doc: Any, path: Path = ENVELOPE) -> list[str]:
    return [f"{'/'.join(map(str, e.absolute_path))}: {e.message}" for e in validator(path).iter_errors(doc)]


def envelope(command: str, result: Any, status: str = "OK") -> dict[str, Any]:
    return {
        "$schema": "https://ovt.dev/schema/ovt-cli-1.json",
        "ovt_version": "0.1.0-dev",
        "schema_version": 1,
        "command": command,
        "input": {"path": "clip.mp4", "size_bytes": 1234, "source_hash": "00ff" * 16},
        "result": result,
        "diagnostics": [{"severity": "Warning", "code": "SYN-0007", "message": "trailing bits"}],
        "status": status,
    }


# ------------------------------------------------------------------------------------- the schemas


@pytest.mark.parametrize("path", generated_files(), ids=lambda p: p.name)
def test_generated_schema_is_valid_draft_2020_12(path: Path) -> None:
    schema = load(path)
    assert schema["$schema"] == "https://json-schema.org/draft/2020-12/schema"
    jsonschema.Draft202012Validator.check_schema(schema)


def test_every_command_has_a_result_rule() -> None:
    schema = load(ENVELOPE)
    ruled = [r["if"]["properties"]["command"]["const"] for r in schema["allOf"]]
    assert ruled == schema["properties"]["command"]["enum"]


# --------------------------------------------------------------------------- §12.1.4 JSON rules


def test_probe_envelope_with_large_ulong_as_string() -> None:
    tree = {
        "format": "isobmff",
        "root": [
            {
                "fourcc": "mdat",
                "file_span": {"offset": "9007199254740993", "length": 16},  # 2^53 + 1: quoted by the writer
                "header_length": 8,
                "fields": [],
                "children": [],
            }
        ],
        "tracks": [{"track_id": 1, "codec": "H264", "width": 1920.0, "height": "nan", "duration": 12}],
    }
    assert errors(envelope("probe", tree)) == []


def test_analyze_envelope_with_decoded_nested_tables() -> None:
    frame = {
        "frame": {"id": {"decode_index": 0, "display_index": 0}, "type": "I", "size_bytes": 4096},
        "grid": {"cells_w": 2, "cells_h": 1, "pred": [1, 2], "qp": [26, 27]},
        "blocks": [
            {
                "id": 0,
                "parent": -1,
                "kind": "MB",
                "pred": "Intra",
                "ext_type": "AvcMb",
                "ext": base64.b64encode(b"\x01\x02\x03").decode(),
                "ext_decoded": {"mb_type": "I_16x16_0_0_0"},
            }
        ],
        "filter_params_type": "AvcFilterParams",
        "filter_params": None,
        "filter_params_decoded": {},
        "qp_hist": [0] * 256,
    }
    assert errors(envelope("analyze", [frame])) == []


def test_unknown_enum_value_prints_as_integer() -> None:
    assert errors(envelope("probe", {"tracks": [{"codec": 200}]})) == []


def test_failed_command_has_empty_result() -> None:
    doc = envelope("frames", {}, status="IO")
    doc["diagnostics"] = [{"severity": "Error", "code": "CLI-0004", "message": "cannot read 'x.mp4'"}]
    del doc["input"]
    assert errors(doc) == []


@pytest.mark.parametrize(
    ("mutate", "where"),
    [
        (lambda d: d.update(schema_version=2), "schema_version"),
        (lambda d: d.update(status="FINE"), "status"),
        (lambda d: d.update(command="nope"), "command"),
        (lambda d: d.update(extra=1), ""),
        (lambda d: d["diagnostics"][0].update(severity="warning"), "diagnostics/0/severity"),
        (lambda d: d["input"].update(source_hash="XYZ"), "input/source_hash"),
        (lambda d: d["result"]["tracks"][0].update(codec="H999"), "result"),
        (lambda d: d["result"]["tracks"][0].update(duration="12.5"), "result"),
        (lambda d: d["result"]["tracks"][0].update(not_a_field=1), "result"),
        (lambda d: d.update(result=[]), "result"),
    ],
)
def test_rejects_invalid_envelopes(mutate: Any, where: str) -> None:
    doc = envelope("probe", {"tracks": [{"track_id": 1, "codec": "H264", "duration": 12}]})
    assert errors(doc) == []
    bad = copy.deepcopy(doc)
    mutate(bad)
    found = errors(bad)
    assert found, "mutated envelope still validates"
    assert any(e.startswith(where) for e in found), found


def test_decoded_table_must_match_its_type_field() -> None:
    block = {"ext_type": "HevcCu", "ext_decoded": {"mb_type": "I_16x16_0_0_0"}}  # an AvcMb field
    assert errors(envelope("analyze", [{"blocks": [block]}]))
    block["ext_type"] = "AvcMb"
    assert errors(envelope("analyze", [{"blocks": [block]}])) == []


def test_cellgrid_bytes_are_integer_arrays() -> None:
    grid = {"pred": base64.b64encode(b"\x01").decode()}
    assert errors(envelope("analyze", [{"grid": grid}]))


def test_check_jsonschema_accepts_an_envelope(tmp_path: Path) -> None:
    tool = shutil.which("check-jsonschema")
    if tool is None:
        pytest.skip("check-jsonschema is not installed (pip install -r tools/requirements.txt)")
    doc = tmp_path / "probe.json"
    doc.write_text(json.dumps(envelope("probe", {"format": "isobmff"})), encoding="utf-8")
    proc = subprocess.run([tool, "--schemafile", str(ENVELOPE), str(doc)], capture_output=True, text=True)
    assert proc.returncode == 0, proc.stdout + proc.stderr


# ------------------------------------------------------- AT1: real `ovt` output over the corpus


def fast_mp4_files() -> list[Path]:
    import fetch_corpora

    manifest = load(ROOT / "tests" / "corpora" / "manifest.json")
    cache = fetch_corpora.cache_dir_for(None)
    files: list[Path] = []
    missing: list[str] = []
    for entry in manifest["entries"]:
        if "fast" not in entry.get("tags", []) or "mp4" not in entry.get("containers", []):
            continue
        record = fetch_corpora.read_ok(cache / entry["id"])
        if record is None:
            missing.append(entry["id"])
            continue
        files += [cache / entry["id"] / name for name in sorted(record.get("files", {}))]
    if missing:
        msg = f"corpus subset 'fast' missing: run tools/fetch_corpora.py --tags fast ({len(missing)} MP4 entries)"
        if os.environ.get("OVT_REQUIRE_CORPORA") == "1":
            pytest.fail(msg)
        pytest.skip(msg)
    return files


def test_ovt_json_output_validates_over_fast_mp4_corpus(tmp_path: Path) -> None:
    cli = os.environ.get("OVT_CLI")
    tool = shutil.which("check-jsonschema")
    if not cli or tool is None:
        pytest.skip("needs OVT_CLI and check-jsonschema")
    files = fast_mp4_files()
    assert files, "no MP4 file in the fast corpus subset"
    validated = 0
    pending: list[str] = []
    for cmd in AT1_COMMANDS:
        for media in files:
            proc = subprocess.run([cli, cmd, str(media), "-f", "json"], capture_output=True, text=True)
            # SPEC-ASSUMPTION(#286): a subcommand that is not implemented yet (WI-1.502 to WI-1.506)
            # exits 2 with CLI-0001; it is reported as pending, not failed.
            if proc.returncode == 2 and "CLI-0001" in proc.stderr:
                pending.append(cmd)
                break
            out = tmp_path / f"{cmd}-{media.name}.json"
            out.write_text(proc.stdout, encoding="utf-8")
            check = subprocess.run([tool, "--schemafile", str(ENVELOPE), str(out)], capture_output=True, text=True)
            assert check.returncode == 0, f"ovt {cmd} {media}:\n{check.stdout}{check.stderr}"
            validated += 1
    if not validated:
        pytest.skip(f"no AT1 subcommand implemented yet: {', '.join(pending)}")


# ------------------------------------------------------------- AT2: stale schemas are detected


def flatc() -> str:
    exe = os.environ.get("OVT_FLATC") or shutil.which("flatc")
    if not exe:
        pytest.skip("flatc not found (set OVT_FLATC)")
    return exe


def test_committed_schemas_are_up_to_date() -> None:
    proc = subprocess.run(
        [sys.executable, "-I", str(GENERATOR), "--flatc", flatc(), "--check"], capture_output=True, text=True
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_modified_fbs_without_regeneration_fails_with_a_diff(tmp_path: Path) -> None:
    schema_dir = tmp_path / "schema"
    shutil.copytree(ROOT / "core" / "schema", schema_dir)
    model = schema_dir / "ovt_model.fbs"
    text = model.read_text(encoding="utf-8")
    anchor = "table SyntaxNode {"
    assert anchor in text
    model.write_text(text.replace(anchor, anchor + "\n  wi_1501_probe:ulong;", 1), encoding="utf-8")
    proc = subprocess.run(
        [sys.executable, "-I", str(GENERATOR), "--flatc", flatc(), "--schema-dir", str(schema_dir), "--check"],
        capture_output=True,
        text=True,
    )
    assert proc.returncode == 1, proc.stdout + proc.stderr
    assert '+        "wi_1501_probe"' in proc.stdout or '"wi_1501_probe"' in proc.stdout
    assert "SyntaxNode.schema.json" in proc.stdout
    assert "out of date" in proc.stderr


def test_generation_is_deterministic(tmp_path: Path) -> None:
    exe = flatc()
    for out in ("a", "b"):
        subprocess.run([sys.executable, "-I", str(GENERATOR), "--flatc", exe, "--out", str(tmp_path / out)], check=True)
    for path in sorted((tmp_path / "a").iterdir()):
        assert path.read_bytes() == (tmp_path / "b" / path.name).read_bytes(), path.name
