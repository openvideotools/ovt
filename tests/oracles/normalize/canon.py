#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Shared part of the oracle normalisers (chapter 14 §14.4.0, WI-0.555).

Every normaliser in this directory turns one oracle's native output into one canonical JSON family
(§14.4.0), validates it against ``schema/<family>.schema.json`` and prints it. This module holds the
pieces they share: the deterministic JSON writer, the small JSON Schema validator and the CLI frame.

Run a normaliser as ``python -I tests/oracles/normalize/<x>.py <oracle-output> [options] > out.json``
(``-`` reads stdin). ``python -I canon.py --validate <family> <file>`` checks an existing file.

Exit codes: 0 ok; 2 usage (argparse); 3 the oracle output cannot be parsed or holds a name the maps do
not know; 4 the result does not match its family schema (a normaliser bug).
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from collections.abc import Callable, Iterable
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
SCHEMA_DIR = HERE / "schema"
FAMILIES = ("syntax-elements", "header-struct", "block-grid", "box-tree", "sample-table", "scores")

EXIT_INPUT = 3
EXIT_SCHEMA = 4


class NormalizeError(Exception):
    """The oracle output is malformed or names something the maps do not cover (exit 3)."""


class SchemaError(Exception):
    """A document does not match its family schema (exit 4)."""


# --------------------------------------------------------------------------------------------------------
# Deterministic JSON. Keys sorted; containers one element per line; arrays of scalars on one line so a
# 4x4-cell grid stays readable in a diff. Floats are written with at most 6 decimals (the %.6f rule that
# WI-0.556's C++ normaliser follows too); infinities as the strings "inf" / "-inf" (JSON has no inf).


def fmt_float(value: float) -> float | str:
    if math.isinf(value):
        return "inf" if value > 0 else "-inf"
    if math.isnan(value):
        raise NormalizeError("NaN in oracle output")
    rounded = round(value, 6)
    return 0.0 if rounded == 0 else rounded


def _scalar(value: Any) -> str:
    if isinstance(value, float):
        value = fmt_float(value)
    return json.dumps(value, ensure_ascii=False)


def _is_scalar(value: Any) -> bool:
    return not isinstance(value, (dict, list))


def _write(value: Any, indent: int, out: list[str]) -> None:
    pad = " " * indent
    if isinstance(value, dict):
        if not value:
            out.append("{}")
            return
        out.append("{\n")
        items = sorted(value.items())
        for n, (key, item) in enumerate(items):
            out.append(f"{pad} {json.dumps(str(key), ensure_ascii=False)}: ")
            _write(item, indent + 1, out)
            out.append(",\n" if n + 1 < len(items) else "\n")
        out.append(pad + "}")
    elif isinstance(value, list):
        if all(_is_scalar(v) for v in value):
            out.append("[" + ",".join(_scalar(v) for v in value) + "]")
            return
        out.append("[\n")
        for n, item in enumerate(value):
            out.append(pad + " ")
            _write(item, indent + 1, out)
            out.append(",\n" if n + 1 < len(value) else "\n")
        out.append(pad + "]")
    else:
        out.append(_scalar(value))


def dumps(value: Any) -> str:
    out: list[str] = []
    _write(value, 0, out)
    return "".join(out) + "\n"


# --------------------------------------------------------------------------------------------------------
# JSON Schema subset (draft 2020-12 keywords: type, enum, const, properties, required,
# additionalProperties, items, minItems, minimum, anyOf, $ref to #/$defs/...). The schemas in schema/ use
# only these. A local validator keeps tools/requirements-test.txt free of a new dependency (jsonschema).

_TYPES: dict[str, Callable[[Any], bool]] = {
    "object": lambda v: isinstance(v, dict),
    "array": lambda v: isinstance(v, list),
    "string": lambda v: isinstance(v, str),
    "integer": lambda v: isinstance(v, int) and not isinstance(v, bool),
    "number": lambda v: isinstance(v, (int, float)) and not isinstance(v, bool),
    "boolean": lambda v: isinstance(v, bool),
    "null": lambda v: v is None,
}
_KNOWN = set(
    "$schema $id $defs title description type enum const properties required additionalProperties items"
    " minItems minimum anyOf $ref".split()
)


def _check(doc: Any, schema: dict[str, Any], root: dict[str, Any], path: str, errors: list[str]) -> None:
    unknown = set(schema) - _KNOWN
    if unknown:
        raise SchemaError(f"schema keyword(s) not supported by canon.py: {sorted(unknown)}")
    if "$ref" in schema:
        ref = schema["$ref"]
        if not ref.startswith("#/$defs/"):
            raise SchemaError(f"unsupported $ref {ref}")
        _check(doc, root["$defs"][ref[len("#/$defs/") :]], root, path, errors)
    if "anyOf" in schema and not any(_valid(doc, sub, root) for sub in schema["anyOf"]):
        errors.append(f"{path}: matches no alternative of anyOf")
    if "type" in schema:
        types = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
        if not any(_TYPES[t](doc) for t in types):
            errors.append(f"{path}: expected {'|'.join(types)}, got {type(doc).__name__}")
            return
    if "enum" in schema and doc not in schema["enum"]:
        errors.append(f"{path}: {doc!r} not in {schema['enum']}")
    if "const" in schema and doc != schema["const"]:
        errors.append(f"{path}: {doc!r} != {schema['const']!r}")
    if "minimum" in schema and _TYPES["number"](doc) and doc < schema["minimum"]:
        errors.append(f"{path}: {doc} < minimum {schema['minimum']}")
    if isinstance(doc, dict):
        props = schema.get("properties", {})
        for key in schema.get("required", []):
            if key not in doc:
                errors.append(f"{path}: missing required key {key!r}")
        extra = schema.get("additionalProperties", True)
        for key, value in doc.items():
            if key in props:
                _check(value, props[key], root, f"{path}/{key}", errors)
            elif extra is False:
                errors.append(f"{path}: unexpected key {key!r}")
            elif isinstance(extra, dict):
                _check(value, extra, root, f"{path}/{key}", errors)
    if isinstance(doc, list):
        if len(doc) < schema.get("minItems", 0):
            errors.append(f"{path}: fewer than {schema['minItems']} items")
        if "items" in schema:
            for n, item in enumerate(doc):
                _check(item, schema["items"], root, f"{path}/{n}", errors)


def _valid(doc: Any, schema: dict[str, Any], root: dict[str, Any]) -> bool:
    errors: list[str] = []
    _check(doc, schema, root, "", errors)
    return not errors


def load_schema(family: str) -> dict[str, Any]:
    if family not in FAMILIES:
        raise SchemaError(f"unknown family {family!r} (one of {', '.join(FAMILIES)})")
    schema: dict[str, Any] = json.loads((SCHEMA_DIR / f"{family}.schema.json").read_text(encoding="utf-8"))
    return schema


def validate(doc: Any, family: str) -> list[str]:
    """Return the list of schema violations of ``doc`` against ``family`` (empty = valid)."""
    schema = load_schema(family)
    errors: list[str] = []
    _check(doc, schema, schema, "", errors)
    return errors


# --------------------------------------------------------------------------------------------------------
# Inputs and the CLI frame


def read_text(path: str) -> str:
    if path == "-":
        return sys.stdin.read()
    return Path(path).read_text(encoding="utf-8", errors="replace")


def read_json(path: str) -> Any:
    try:
        return json.loads(read_text(path))
    except json.JSONDecodeError as exc:
        raise NormalizeError(f"{path}: not JSON: {exc}") from exc


def load_map(name: str) -> Any:
    """Load a name/field map that lives next to the normalisers (``<name>.json``)."""
    return json.loads((HERE / f"{name}.json").read_text(encoding="utf-8"))


def load_list(name: str) -> set[str]:
    """Load a one-name-per-line list (``#`` comments and blank lines ignored)."""
    lines = (HERE / name).read_text(encoding="utf-8").splitlines()
    return {s for s in (line.split("#", 1)[0].strip() for line in lines) if s}


INT_RE = re.compile(r"^-?\d+$")


def to_int(text: str, what: str) -> int:
    if not INT_RE.match(text.strip()):
        raise NormalizeError(f"{what}: {text!r} is not an integer")
    return int(text)


# --------------------------------------------------------------------------------------------------------
# box-tree helpers shared by the ISOBMFF normalisers (§14.4.6-§14.4.9)

HEX_RE = re.compile(r"^(?:0x)?([0-9a-fA-F]+)$")
FIXED_RE = re.compile(r"^-?\d+(?:\.\d+)?$")


def box_paths(boxes: Iterable[tuple[int, str, dict[str, Any]]]) -> list[dict[str, Any]]:
    """Give every box its ``box-tree`` path. Input: ``(depth, fourcc, record)`` in file order (pre-order,
    depth 0 = top level). A 1-based ``[n]`` follows a fourcc only when its parent has several such boxes."""
    nodes: list[tuple[int, str, dict[str, Any]]] = []  # (parent index or -1, fourcc, record)
    stack: list[int] = []
    for depth, fourcc, record in boxes:
        if depth > len(stack):
            raise NormalizeError(f"box {fourcc!r} at depth {depth} without a parent")
        del stack[depth:]
        nodes.append((stack[-1] if stack else -1, fourcc, record))
        stack.append(len(nodes) - 1)
    count: dict[tuple[int, str], int] = {}
    for parent, fourcc, _ in nodes:
        count[parent, fourcc] = count.get((parent, fourcc), 0) + 1
    seen: dict[tuple[int, str], int] = {}
    paths: list[str] = []
    out = []
    for parent, fourcc, record in nodes:
        seen[parent, fourcc] = seen.get((parent, fourcc), 0) + 1
        part = fourcc + (f"[{seen[parent, fourcc]}]" if count[parent, fourcc] > 1 else "")
        paths.append(part if parent < 0 else f"{paths[parent]}/{part}")
        out.append({"path": paths[-1], **record})
    return out


def field_value(text: str, conv: str = "auto") -> Any:
    """One oracle field value in the §8.9.2 rule-3 form: integers as integers, fixed point as the raw integer
    (``fixed16`` = 16.16, ``fixed8`` = 8.8), hex as integers (``hex``), byte blobs as lowercase hex
    (``blob``; GPAC writes ``data:application/octet-string,<HEX>``), booleans 0/1, the rest as strings."""
    text = text.strip()
    if conv in ("fixed16", "fixed8"):
        if not FIXED_RE.match(text):
            raise NormalizeError(f"{text!r} is not a fixed-point number")
        return round(float(text) * (65536 if conv == "fixed16" else 256))
    if conv == "hex":
        m = HEX_RE.match(text.split(" ", 1)[0])
        if not m:
            raise NormalizeError(f"{text!r} is not a hex number")
        return int(m.group(1), 16)
    if conv == "blob":
        return text.rsplit(",", 1)[-1].lower()
    if conv == "minus1":
        return to_int(text, "value") - 1
    if conv == "str":
        return text
    if INT_RE.match(text):
        return int(text)
    if text.startswith("0x") and HEX_RE.match(text):
        return int(text, 16)
    if text.startswith("data:application/octet-string,"):
        return text.rsplit(",", 1)[-1].lower()
    if text in ("true", "false"):
        return int(text == "true")
    return text


def map_fields(raw: Iterable[tuple[str, str]], spec: dict[str, Any], keep_unmapped: bool = False) -> dict[str, Any]:
    """Map ``(oracle name, text)`` pairs through ``spec`` = ``{oracle name: spec name | [spec name, conv]}``.
    A spec name ending in ``[]`` collects repeated values into a list. Unmapped names are dropped unless
    ``keep_unmapped`` (used while extending a map: they come out under ``?<oracle name>``)."""
    out: dict[str, Any] = {}
    for name, text in raw:
        target = spec.get(name)
        if target is None:
            if keep_unmapped:
                out[f"?{name}"] = field_value(text)
            continue
        key, conv = (target, "auto") if isinstance(target, str) else target
        value = field_value(text, conv)
        if key.endswith("[]"):
            out.setdefault(key[:-2], []).append(value)
        else:
            out[key] = value
    return out


def box_map(maps: dict[str, Any], fourcc: str) -> dict[str, Any]:
    """The field map of one box: the ``"*"`` entries overlaid with the box's own."""
    return {**maps.get("*", {}), **maps.get(fourcc, {})}


def run(
    family: str,
    description: str,
    convert: Callable[[argparse.Namespace], Any],
    add_args: Callable[[argparse.ArgumentParser], None] | None = None,
    argv: Iterable[str] | None = None,
) -> int:
    """Parse the common CLI, convert, validate against ``family`` and print. Returns the exit code."""
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("input", help="oracle output file, or - for stdin")
    parser.add_argument("-o", "--output", help="write here instead of stdout")
    if add_args:
        add_args(parser)
    args = parser.parse_args(list(argv) if argv is not None else None)
    try:
        doc = convert(args)
    except NormalizeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return EXIT_INPUT
    errors = validate(doc, family)
    if errors:
        print(f"error: output does not match the {family} schema:", file=sys.stderr)
        for line in errors[:20]:
            print(f"  {line}", file=sys.stderr)
        return EXIT_SCHEMA
    text = dumps(doc)
    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Validate a canonical JSON file against its family schema.")
    parser.add_argument("--validate", metavar="FAMILY", required=True, choices=FAMILIES)
    parser.add_argument("file")
    args = parser.parse_args(argv)
    try:
        errors = validate(read_json(args.file), args.validate)
    except NormalizeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return EXIT_INPUT
    for line in errors:
        print(line, file=sys.stderr)
    return EXIT_SCHEMA if errors else 0


if __name__ == "__main__":
    sys.exit(main())
