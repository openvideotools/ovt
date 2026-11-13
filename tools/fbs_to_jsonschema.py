#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate the JSON Schemas of the `-f json` output (chapter 12 §12.6, WI-1.501).

    fbs_to_jsonschema.py --flatc FLATC [--schema-dir core/schema] [--out docs/spec/schemas] [--check]

Runs `flatc --jsonschema` on every core/schema/*.fbs, merges the definitions and rewrites them into
JSON Schema draft 2020-12 following the JSON rules of chapter 12 §12.1.4:

  * enums are their value names (an unknown value prints as its integer, §6.8.1);
  * long / ulong (and every integer beyond ±(2^53 - 1), which the writer quotes) are
    oneOf[integer, decimal string];
  * float / double also accept the strings "nan", "inf" and "-inf";
  * [ubyte] is a base64 string, except in CellGrid (an array of integers);
  * the nested FlatBuffers (Block.ext, FrameAnalysis.filter_params, FrameInfo.ref_state, HevcCu.palette,
    Av1Block.palette) gain a `<field>_decoded` object, chosen by the sibling `<field>_type` string.

It writes one self-contained `<Table>.schema.json` per root table and per command result table, and
`ovt-cli-1.schema.json` for the envelope, whose `result` is checked against the table of its `command`
(chapter 12 §12.2.0). Only flatc's property names, order and scalar ranges are used, so the output does
not depend on the flatc version. Output is deterministic: the same .fbs always gives the same bytes.

  --check   generate into a temporary directory and compare with --out; print a unified diff of every
            difference and exit 1 when a file is stale or missing (CMake target ovt_json_schema_check).
"""

from __future__ import annotations

import argparse
import difflib
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

DRAFT = "https://json-schema.org/draft/2020-12/schema"
BASE_URL = "https://ovt.dev/schema/"
# The envelope's own `$schema` value (chapter 12 §12.1.4; cli/src/output/json_writer.h kEnvelopeSchemaUrl).
ENVELOPE_ID = BASE_URL + "ovt-cli-1.json"
ENVELOPE_FILE = "ovt-cli-1.schema.json"

FBS_FILES = ("ovt_model.fbs", "ovt_container.fbs", "ovt_metrics.fbs")

# Root tables (chapter 6 §6.1.1) and the result tables of chapter 12 table 12.2.0.
TABLE_FILES = (
    "BdRateResult",
    "CacheReport",
    "CliEnvelope",
    "ContainerRoot",
    "ContainerTree",
    "DoctorReport",
    "DumpManifest",
    "ExtractManifest",
    "FrameAnalysis",
    "FrameInfo",
    "HdrReport",
    "MetricsReport",
    "MetricsResult",
    "PluginList",
    "SceneSegment",
    "StreamIndex",
    "SyntaxNode",
    "VerifyReport",
)

# Tables whose [ubyte] vectors are per-cell values, written as arrays of integers (§12.1.4).
BYTE_ARRAY_TABLES = frozenset({"CellGrid"})

# Nested FlatBuffers: table -> [(field, type field or None, candidate tables)] (fbs comments, §12.1.4).
NESTED: dict[str, list[tuple[str, str | None, tuple[str, ...]]]] = {
    "Block": [("ext", "ext_type", ("AvcMb", "HevcCu", "HevcPu", "HevcTu", "Av1Block", "Av1Txb"))],
    "FrameAnalysis": [
        ("filter_params", "filter_params_type", ("HevcFilterParams", "Av1FilterParams", "AvcFilterParams"))
    ],
    "FrameInfo": [("ref_state", "ref_state_type", ("HevcRps", "Av1RefState", "AvcRefMarking"))],
    "HevcCu": [("palette", None, ("HevcPalette",))],
    "Av1Block": [("palette", None, ("Av1Palette",))],
}

# Command -> result shape (chapter 12 table 12.2.0). ("table", T), ("array", T), ("any", [T...]), or
# ("free", why): a JSON object or array whose shape no chapter fixes yet.
COMMANDS: dict[str, tuple[str, Any]] = {
    "info": ("free", "ContainerTree + StreamIndex summary; JSON shape not fixed by chapter 12 (§12.2.1)"),
    "probe": ("table", "ContainerTree"),
    "syntax": ("any", ["StreamIndex", "SyntaxNode"]),
    "index": ("table", "StreamIndex"),
    "frames": ("array", "FrameInfo"),
    "dump": ("table", "DumpManifest"),
    "render": ("table", "DumpManifest"),
    "extract": ("table", "ExtractManifest"),
    "cache": ("table", "CacheReport"),
    "analyze": ("array", "FrameAnalysis"),
    "plugins": ("table", "PluginList"),
    "stats": ("free", "free JSON object (§12.2.0)"),
    "metrics": ("table", "MetricsReport"),
    "bdrate": ("table", "BdRateResult"),
    "hdr": ("table", "HdrReport"),
    "compare": ("free", "free JSON object (§12.2.0)"),
    "scenes": ("array", "SceneSegment"),
    "models": ("free", "free JSON object (§12.2.0)"),
    "verify": ("table", "VerifyReport"),
    "doctor": ("table", "DoctorReport"),
    "hrd": ("free", "free JSON object (§12.2.0)"),
}

# Exit-code names written as the envelope `status` (chapter 12 §12.1.3; cli/src/exit_codes.h).
STATUS_NAMES = ("OK", "ERROR", "USAGE", "IO", "UNSUPPORTED", "CORRUPT", "PLUGIN", "CANCELLED", "MISMATCH", "PARTIAL")
SEVERITY_NAMES = ("Info", "Warning", "Error", "Fatal")

MAX_EXACT = 2**53 - 1  # the JSON writer quotes integers beyond ±MAX_EXACT

# Shared helper definitions. Their names start with "_" so they cannot clash with a FlatBuffers type.
U64 = "_UInt64"
I64 = "_Int64"
FLOAT = "_Float"
BYTES = "_Bytes"
BYTE_ARRAY = {"type": "array", "items": {"type": "integer", "minimum": 0, "maximum": 255}}
HELPERS: dict[str, dict[str, Any]] = {
    U64: {
        "$comment": "ulong: a JSON integer, or a decimal string when the value exceeds 2^53 - 1 (§12.1.4).",
        "oneOf": [
            {"type": "integer", "minimum": 0, "maximum": 2**64 - 1},
            {"type": "string", "pattern": "^[0-9]+$"},
        ],
    },
    I64: {
        "$comment": "long: a JSON integer, or a decimal string when |value| exceeds 2^53 - 1 (§12.1.4).",
        "oneOf": [
            {"type": "integer", "minimum": -(2**63), "maximum": 2**63 - 1},
            {"type": "string", "pattern": "^-?[0-9]+$"},
        ],
    },
    FLOAT: {
        "$comment": "float / double: a JSON number, or one of the strings nan, inf, -inf (§12.1.4).",
        "oneOf": [{"type": "number"}, {"enum": ["nan", "inf", "-inf"]}],
    },
    BYTES: {
        # SPEC-ASSUMPTION(#285): §12.1.4 writes [ubyte] as base64, §6.8.1 as an array of numbers
        # (lowercase hex for hash-like fields). Both forms validate until the issue is answered.
        "$comment": "[ubyte]: a base64 string (§12.1.4; lowercase hex is also a valid base64 alphabet string) "
        "or an array of integers (§6.8.1). SPEC-ASSUMPTION(#285).",
        "anyOf": [
            {"type": "string", "contentEncoding": "base64", "pattern": "^[A-Za-z0-9+/]*={0,2}$"},
            BYTE_ARRAY,
        ],
    },
}


class SchemaError(Exception):
    pass


def ref(name: str) -> dict[str, Any]:
    return {"$ref": "#/$defs/" + name}


def def_name(flatc_ref: str) -> str:
    """`#/definitions/ovt_fb_Block` (flatc 2.x and later) -> `Block`."""
    name = flatc_ref.rsplit("/", 1)[-1]
    for prefix in ("ovt_fb_", "ovt.fb."):
        if name.startswith(prefix):
            return name[len(prefix) :]
    raise SchemaError(f"unexpected flatc reference {flatc_ref!r} (namespace is not ovt.fb)")


def run_flatc(flatc: str, schema_dir: Path) -> dict[str, dict[str, Any]]:
    """Merged flatc definitions, keyed by unqualified type name, in first-seen order."""
    merged: dict[str, dict[str, Any]] = {}
    with tempfile.TemporaryDirectory(prefix="ovt-jsonschema-") as tmp:
        for fbs in FBS_FILES:
            cmd = [flatc, "--jsonschema", "-o", tmp, "-I", str(schema_dir), str(schema_dir / fbs)]
            proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
            if proc.returncode != 0:
                raise SchemaError(f"{' '.join(cmd)} failed ({proc.returncode}):\n{proc.stderr}")
            out = Path(tmp) / (Path(fbs).stem + ".schema.json")
            doc = json.loads(out.read_text(encoding="utf-8"))
            for key, body in doc.get("definitions", doc.get("$defs", {})).items():
                merged.setdefault(def_name(key), body)
    return merged


def is_enum(body: dict[str, Any]) -> bool:
    return "enum" in body


def convert_scalar(prop: dict[str, Any], table: str, field: str, known: set[str]) -> dict[str, Any]:
    if "$ref" in prop:
        name = def_name(prop["$ref"])
        if name not in known:
            raise SchemaError(f"{table}.{field}: reference to unknown type {name}")
        return ref(name)
    kind = prop.get("type")
    if kind == "integer":
        lo, hi = prop.get("minimum"), prop.get("maximum")
        if not isinstance(lo, int) or not isinstance(hi, int):
            raise SchemaError(f"{table}.{field}: integer without a range")
        if hi > MAX_EXACT:
            return ref(U64) if lo == 0 else ref(I64)
        return {"type": "integer", "minimum": lo, "maximum": hi}
    if kind == "number":
        return ref(FLOAT)
    if kind in ("boolean", "string"):
        return {"type": kind}
    if kind == "array":
        items = prop.get("items")
        if not isinstance(items, dict):
            raise SchemaError(f"{table}.{field}: array without items")
        if items.get("type") == "integer" and items.get("minimum") == 0 and items.get("maximum") == 255:
            return BYTE_ARRAY if table in BYTE_ARRAY_TABLES else ref(BYTES)
        return {"type": "array", "items": convert_scalar(items, table, field, known)}
    raise SchemaError(f"{table}.{field}: unsupported flatc property {json.dumps(prop, sort_keys=True)}")


def candidates_schema(cands: tuple[str, ...]) -> dict[str, Any]:
    return ref(cands[0]) if len(cands) == 1 else {"anyOf": [ref(c) for c in cands]}


def convert_table(name: str, body: dict[str, Any], known: set[str]) -> dict[str, Any]:
    if body.get("type") != "object" or not isinstance(body.get("properties"), dict):
        raise SchemaError(f"{name}: flatc definition is neither an enum nor an object")
    props: dict[str, Any] = {}
    nested = {field: (tfield, cands) for field, tfield, cands in NESTED.get(name, [])}
    for field, prop in body["properties"].items():
        if field in nested:
            tfield, cands = nested[field]
            for c in cands:
                if c not in known:
                    raise SchemaError(f"{name}.{field}: nested type {c} is not in the schema")
            # SPEC-ASSUMPTION(#285): §12.1.4 adds `<field>_decoded` next to the raw bytes; §6.8.1
            # replaces the raw field in place (its example writes `"ext": null`). Both validate.
            props[field] = {"anyOf": [ref(BYTES), {"type": "null"}, candidates_schema(cands)]}
            props[field + "_decoded"] = {"anyOf": [candidates_schema(cands), {"type": "null"}]}
            continue
        props[field] = convert_scalar(prop, name, field, known)
    out: dict[str, Any] = {"type": "object", "properties": props, "additionalProperties": False}
    for field, (tfield, cands) in nested.items():
        if field not in props:
            raise SchemaError(f"{name}: nested field {field} not found")
        if tfield is None or len(cands) == 1:
            continue
        if tfield not in props:
            raise SchemaError(f"{name}: type field {tfield} not found")
        rules = out.setdefault("allOf", [])
        for c in cands:
            rules.append(
                {
                    "if": {"properties": {tfield: {"const": c}}, "required": [tfield]},
                    "then": {
                        "properties": {
                            field: {"anyOf": [ref(BYTES), {"type": "null"}, ref(c)]},
                            field + "_decoded": {"anyOf": [ref(c), {"type": "null"}]},
                        }
                    },
                }
            )
    return out


def convert(flatc_defs: dict[str, dict[str, Any]]) -> dict[str, dict[str, Any]]:
    """All definitions in draft 2020-12 form, sorted by name (helpers first)."""
    known = set(flatc_defs)
    enums = {n for n, b in flatc_defs.items() if is_enum(b)}
    unknown_nested = set(NESTED) - known
    if unknown_nested:
        raise SchemaError(f"nested-field tables missing from the schema: {sorted(unknown_nested)}")
    defs: dict[str, dict[str, Any]] = {}
    for name in sorted(flatc_defs):
        body = flatc_defs[name]
        if name in enums:
            names = body["enum"]
            if not names or not all(isinstance(v, str) for v in names):
                raise SchemaError(f"{name}: enum without value names")
            defs[name] = {"anyOf": [{"type": "string", "enum": list(names)}, {"type": "integer"}]}
        else:
            defs[name] = convert_table(name, body, known)
    return {**HELPERS, **defs}


def closure(defs: dict[str, dict[str, Any]], roots: list[str]) -> dict[str, dict[str, Any]]:
    """The definitions reachable from `roots`, in the order of `defs`."""
    seen: set[str] = set()
    todo = list(roots)
    while todo:
        name = todo.pop()
        if name in seen:
            continue
        if name not in defs:
            raise SchemaError(f"unknown definition {name}")
        seen.add(name)
        stack: list[Any] = [defs[name]]
        while stack:
            node = stack.pop()
            if isinstance(node, dict):
                target = node.get("$ref")
                if isinstance(target, str) and target.startswith("#/$defs/"):
                    todo.append(target[len("#/$defs/") :])
                stack.extend(node.values())
            elif isinstance(node, list):
                stack.extend(node)
    return {n: b for n, b in defs.items() if n in seen}


def table_document(defs: dict[str, dict[str, Any]], table: str) -> dict[str, Any]:
    return {
        "$schema": DRAFT,
        "$id": BASE_URL + table + ".schema.json",
        "title": f"ovt.fb.{table} as written by `ovt -f json` (chapter 12 §12.1.4)",
        "$comment": "Generated by tools/fbs_to_jsonschema.py from core/schema/*.fbs (WI-1.501). Do not edit.",
        "$ref": "#/$defs/" + table,
        "$defs": closure(defs, [table]),
    }


EMPTY_OBJECT = {"type": "object", "maxProperties": 0}


def result_schema(shape: tuple[str, Any]) -> dict[str, Any]:
    kind, arg = shape
    if kind == "table":
        body: dict[str, Any] = ref(arg)
    elif kind == "array":
        body = {"type": "array", "items": ref(arg)}
    elif kind == "any":
        body = {"anyOf": [ref(t) for t in arg]}
    else:
        return {"$comment": arg, "type": ["object", "array"]}
    # A failed command writes `"result": {}` (cli/src/output/json_writer.cpp).
    return {"anyOf": [body, EMPTY_OBJECT]}


def envelope_document(defs: dict[str, dict[str, Any]]) -> dict[str, Any]:
    roots = [arg for kind, arg in COMMANDS.values() if kind in ("table", "array")]
    roots += [t for kind, arg in COMMANDS.values() if kind == "any" for t in arg]
    diagnostic = {
        "type": "object",
        "properties": {
            "severity": {"enum": list(SEVERITY_NAMES)},
            "code": {"type": "string", "pattern": "^[A-Z][A-Z0-9]*-[0-9]{4}$"},
            "message": {"type": "string"},
            "span": {
                "type": "object",
                "properties": {"bit_offset": ref(U64), "bit_length": {"type": "integer", "minimum": 0}},
                "required": ["bit_offset", "bit_length"],
                "additionalProperties": False,
            },
            "spec_ref": {"type": "string"},
        },
        "required": ["severity", "code", "message"],
        "additionalProperties": False,
    }
    rules = [
        {
            "if": {"properties": {"command": {"const": cmd}}, "required": ["command"]},
            "then": {"properties": {"result": result_schema(shape)}},
        }
        for cmd, shape in COMMANDS.items()
    ]
    local = {"EnvelopeDiagnostic": diagnostic}
    return {
        "$schema": DRAFT,
        "$id": ENVELOPE_ID,
        "title": "ovt CLI JSON envelope, schema_version 1 (chapter 12 §12.1.4)",
        "$comment": "Generated by tools/fbs_to_jsonschema.py from core/schema/*.fbs (WI-1.501). Do not edit.",
        "type": "object",
        "properties": {
            "$schema": {"const": ENVELOPE_ID},
            "ovt_version": {"type": "string", "minLength": 1},
            "schema_version": {"const": 1},
            "command": {"enum": list(COMMANDS)},
            "input": {
                "type": "object",
                "properties": {
                    "path": {"type": "string"},
                    "size_bytes": ref(U64),
                    "source_hash": {"type": "string", "pattern": "^([0-9a-f]{2})*$"},
                },
                "required": ["path", "size_bytes", "source_hash"],
                "additionalProperties": False,
            },
            "result": {"type": ["object", "array"]},
            "diagnostics": {"type": "array", "items": ref("EnvelopeDiagnostic")},
            "status": {"enum": list(STATUS_NAMES)},
            "timing": {
                "type": "object",
                "properties": {"cache_hit": {"type": "boolean"}},
                "additionalProperties": ref(FLOAT),
            },
        },
        "required": ["$schema", "ovt_version", "schema_version", "command", "result", "diagnostics", "status"],
        "additionalProperties": False,
        "allOf": rules,
        "$defs": {**closure(defs, roots), **local},
    }


def render(doc: dict[str, Any]) -> str:
    return json.dumps(doc, indent=2, ensure_ascii=False) + "\n"


def generate(flatc: str, schema_dir: Path) -> dict[str, str]:
    defs = convert(run_flatc(flatc, schema_dir))
    files = {t + ".schema.json": render(table_document(defs, t)) for t in TABLE_FILES}
    files[ENVELOPE_FILE] = render(envelope_document(defs))
    return dict(sorted(files.items()))


def check(files: dict[str, str], out: Path) -> int:
    stale = 0
    for name, text in files.items():
        path = out / name
        try:
            current = path.read_text(encoding="utf-8")
        except OSError:
            print(f"fbs_to_jsonschema: {path} is missing", file=sys.stderr)
            stale += 1
            continue
        if current != text:
            stale += 1
            sys.stdout.writelines(
                difflib.unified_diff(
                    current.splitlines(keepends=True),
                    text.splitlines(keepends=True),
                    fromfile=f"{path} (committed)",
                    tofile=f"{path} (from core/schema)",
                )
            )
    if stale:
        print(
            f"fbs_to_jsonschema: {stale} schema file(s) out of date; regenerate with "
            "`cmake --build --preset <preset> --target ovt_json_schema` and commit the result",
            file=sys.stderr,
        )
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--flatc", required=True, help="flatc executable")
    root = Path(__file__).resolve().parent.parent
    ap.add_argument("--schema-dir", type=Path, default=root / "core" / "schema")
    ap.add_argument("--out", type=Path, default=root / "docs" / "spec" / "schemas")
    ap.add_argument("--check", action="store_true", help="compare instead of writing; exit 1 with a diff")
    args = ap.parse_args(argv)
    try:
        files = generate(args.flatc, args.schema_dir)
    except (SchemaError, OSError, json.JSONDecodeError) as exc:
        print(f"fbs_to_jsonschema: {exc}", file=sys.stderr)
        return 2
    if args.check:
        return check(files, args.out)
    args.out.mkdir(parents=True, exist_ok=True)
    for name, text in files.items():
        path = args.out / name
        if not path.exists() or path.read_text(encoding="utf-8") != text:
            path.write_text(text, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
