#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate the EBML/Matroska element table core/src/container/ebml/ebml_schema.gen.cpp (chapter 08 §8.6.7, WI-4.200).

    python3 -I tools/gen_ebml_schema.py            # regenerate from the committed inputs
    python3 -I tools/gen_ebml_schema.py --check    # exit 1 when a committed output differs (CI `ebml-schema-check`)
    python3 -I tools/gen_ebml_schema.py --fetch    # move the pins to the upstream HEADs, refresh the inputs, regenerate

Inputs (tools/registry_inputs/ebml/, byte-exact copies of the IETF CELLAR schemas, CC-BY-4.0):
  ebml_header.xml     ebml.xml of https://github.com/ietf-wg-cellar/ebml-specification (RFC 8794 EBML header elements)
  ebml_matroska.xml   ebml_matroska.xml of https://github.com/ietf-wg-cellar/matroska-specification (RFC 9559)
  INPUTS.sha256       provenance (`# <name> <url> <commit> <date>` lines) and the SHA-256 of every input file

Conventions follow tools/gen_box_registry.py (WI-1.203): committed output, so the build needs no Python; sorted
tables; no timestamps except the snapshot date recorded by --fetch; standard library only; --check compares byte for
byte. Unlike the box registry the inputs are committed, so --check needs no network.

Rules (the pitfalls of chapter 08 WI-4.200):
  * `id` is hex with the length marker included (0x1A45DFA3); the marker must agree with the digit count and the id
    must be neither all-ones in its value bits nor longer than four bytes (RFC 8794 §5, EBMLMaxIDLength 4).
  * `minOccurs` defaults to 0 and an absent `maxOccurs` means unbounded (-1), RFC 8794 §11.1.4.1.
  * Paths keep their `\\` separators and the `(1*)` / `(-)` / `+` markers verbatim.
  * An id defined by both files (EBMLMaxIDLength, EBMLMaxSizeLength) takes the EBML header file's definition.
"""

from __future__ import annotations

import argparse
import datetime
import difflib
import hashlib
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INPUTS = ROOT / "tools" / "registry_inputs" / "ebml"
OUT_CPP = ROOT / "core" / "src" / "container" / "ebml" / "ebml_schema.gen.cpp"

# (provenance name, repository URL, path inside the repository, committed file name). Header first: it wins on a
# duplicate id.
SOURCES = [
    ("ebml", "https://github.com/ietf-wg-cellar/ebml-specification", "ebml.xml", "ebml_header.xml"),
    ("matroska", "https://github.com/ietf-wg-cellar/matroska-specification", "ebml_matroska.xml", "ebml_matroska.xml"),
]

TYPES = {  # schema `type` -> C++ enumerator of ovt::container::ebml::EbmlType
    "master": "Master",
    "uinteger": "UInteger",
    "integer": "Integer",
    "float": "Float",
    "string": "String",
    "utf-8": "Utf8",
    "date": "Date",
    "binary": "Binary",
}
TRUE_VALUES = {"1", "true"}


class InputError(Exception):
    pass


@dataclass(frozen=True)
class Element:
    id: int
    name: str
    type: str
    path: str
    min_occurs: int
    max_occurs: int  # -1 = unbounded
    default: str | None
    unknown_size_allowed: bool
    origin: str


def validate_id(raw: str, name: str) -> int:
    """RFC 8794 §5: the id keeps its length marker; reject a marker/length mismatch and the reserved all-ones value."""
    if not raw.lower().startswith("0x") or len(raw) % 2:
        raise InputError(f"{name}: id {raw!r} is not an even-length 0x hex string")
    value = int(raw, 16)
    n = (len(raw) - 2) // 2
    if not 1 <= n <= 4:
        raise InputError(f"{name}: id {raw} has {n} bytes; EBMLMaxIDLength is 4")
    first = value >> (8 * (n - 1))
    marker_len = next((i + 1 for i in range(8) if first & (0x80 >> i)), 0)
    if marker_len != n:
        raise InputError(f"{name}: id {raw} has a {marker_len}-byte length marker but {n} bytes")
    if (value & ((1 << (7 * n)) - 1)) == (1 << (7 * n)) - 1:
        raise InputError(f"{name}: id {raw} is the reserved all-ones value")
    return value


def parse_schema(path: Path) -> list[Element]:
    out = []
    for el in ET.parse(path).getroot().iter():
        if not el.tag.endswith("element"):
            continue
        a = el.attrib
        name = a.get("name", "")
        for key in ("name", "path", "id", "type"):
            if key not in a:
                raise InputError(f"{path.name}: an element lacks `{key}` ({name or a})")
        if a["type"] not in TYPES:
            raise InputError(f"{path.name}: {name}: unknown type {a['type']!r}")
        out.append(
            Element(
                id=validate_id(a["id"], name),
                name=name,
                type=a["type"],
                path=a["path"],
                min_occurs=int(a.get("minOccurs", "0")),
                max_occurs=int(a["maxOccurs"]) if "maxOccurs" in a else -1,
                default=a.get("default"),
                unknown_size_allowed=a.get("unknownsizeallowed", "0").lower() in TRUE_VALUES,
                origin=path.name,
            )
        )
    return out


def build_table(inputs: Path) -> list[Element]:
    by_id: dict[int, Element] = {}
    for _, _, _, local in SOURCES:
        for e in parse_schema(inputs / local):
            if e.id in by_id:
                if by_id[e.id].name != e.name or by_id[e.id].path != e.path:
                    raise InputError(f"id 0x{e.id:X} is {by_id[e.id].name} in one file and {e.name} in another")
                continue  # the same element restated as a constraint: the first (header) definition stays
            by_id[e.id] = e
    names: dict[str, int] = {}
    for e in by_id.values():
        if e.path in names:
            raise InputError(f"path {e.path} is defined by 0x{names[e.path]:X} and 0x{e.id:X}")
        names[e.path] = e.id
    return [by_id[i] for i in sorted(by_id)]


# ---------------------------------------------------------------------------------------------------------------
# Provenance


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


# REUSE-IgnoreStart
SPDX_LINES = [
    "# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors",
    "# SPDX-License-Identifier: LGPL-2.1-or-later",
]
# The generated table carries facts of the CELLAR schemas (CC-BY-4.0), so its header names both licences; the first
# identifier stays OVT's own for tools/check_spdx_dirs.py.
CPP_SPDX_LINES = [
    "// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors",
    "// SPDX-FileCopyrightText: IETF CELLAR working group contributors (ebml-specification, matroska-specification)",
    "// SPDX-License-Identifier: LGPL-2.1-or-later",
    "// SPDX-License-Identifier: CC-BY-4.0",
]
# REUSE-IgnoreEnd


def provenance_lines(inputs: Path) -> list[str]:
    path = inputs / "INPUTS.sha256"
    return [ln for ln in path.read_text("utf-8").splitlines() if ln.startswith("#")] if path.exists() else []


def read_provenance(inputs: Path) -> dict[str, str]:
    prov: dict[str, str] = {}
    for line in provenance_lines(inputs):
        parts = line.split()
        if len(parts) >= 5 and parts[1] in {s[0] for s in SOURCES}:
            prov[parts[1]] = parts[3][:12]
        elif len(parts) >= 3 and parts[:2] == ["#", "snapshot"]:
            prov["date"] = parts[2]
    return prov


def render_inputs_sha(inputs: Path) -> str:
    head = [ln for ln in provenance_lines(inputs) if ln not in SPDX_LINES]
    body = [f"{sha256(inputs / local)}  {local}" for _, _, _, local in SOURCES]
    return "\n".join([*SPDX_LINES, *head, *body]) + "\n"


def verify_hashes(inputs: Path) -> None:
    path = inputs / "INPUTS.sha256"
    hashes = {}
    for line in path.read_text("utf-8").splitlines() if path.exists() else []:
        parts = line.split()
        if len(parts) == 2 and not line.startswith("#"):
            hashes[parts[1]] = parts[0]
    for _, _, _, local in SOURCES:
        if sha256(inputs / local) != hashes.get(local):
            raise InputError(f"{local} does not match its SHA-256 in INPUTS.sha256; run --fetch or fix the input")


# ---------------------------------------------------------------------------------------------------------------
# Output


def c_str(s: str | None) -> str:
    if s is None:
        return "nullptr"
    out = []
    for ch in s:
        if ch in '"\\':
            out.append("\\" + ch)
        elif 0x20 <= ord(ch) < 0x7F and not (ch == "?" and out and out[-1] == "?"):  # no trigraphs
            out.append(ch)
        else:
            raise InputError(f"non-ASCII or control character in {s!r}")
    return '"' + "".join(out) + '"'


def render_cpp(table: list[Element], inputs: Path) -> str:
    prov = read_provenance(inputs)
    hashes = "ebml@{} matroska@{}".format(prov.get("ebml", "unknown"), prov.get("matroska", "unknown"))
    lines = [
        *CPP_SPDX_LINES,
        "//",
        f"// GENERATED by tools/gen_ebml_schema.py from {hashes}",
        f"// (snapshot {prov.get('date', 'unknown')}; hashes: tools/registry_inputs/ebml/INPUTS.sha256). DO NOT EDIT.",
        "// Chapter 08 §8.6.7. Regenerate with `python3 -I tools/gen_ebml_schema.py`; CI runs it with --check.",
        "// Element facts are taken from the EBML (RFC 8794) and Matroska (RFC 9559) schemas of the IETF CELLAR",
        "// working group, Copyright (c) the authors, used under CC-BY-4.0 (LICENSES/CC-BY-4.0.txt); changes: reformatted as a",
        "// sorted C++ table.",
        "",
        '#include "container/ebml/ebml_schema.h"',
        "",
        "namespace ovt::container::ebml {",
        "",
        "// clang-format off",
        "// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,modernize-raw-string-literal)",
        "// id, name, type, path, min_occurs, max_occurs (-1 = unbounded), default_value, unknown_size_allowed",
        "const EbmlElementDef kEbmlElements[] = {",
    ]
    for e in table:
        lines.append(
            f"    {{0x{e.id:X}U,{c_str(e.name)},EbmlType::{TYPES[e.type]},{c_str(e.path)},"
            f"{e.min_occurs},{e.max_occurs},{c_str(e.default)},{'true' if e.unknown_size_allowed else 'false'}}},"
        )
    lines += [
        "};",
        "// NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,modernize-raw-string-literal)",
        "// clang-format on",
        "",
        f"const std::size_t kEbmlElementCount = {len(table)};",
        "",
        "}  // namespace ovt::container::ebml",
        "",
    ]
    return "\n".join(lines)


def generate(inputs: Path, verify: bool) -> dict[Path, str]:
    if verify:  # --check: the inputs are exactly what INPUTS.sha256 pins
        verify_hashes(inputs)
    table = build_table(inputs)
    return {OUT_CPP: render_cpp(table, inputs)}


# ---------------------------------------------------------------------------------------------------------------
# Fetch


def git(*args: str, cwd: Path | None = None) -> str:
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def fetch(inputs: Path) -> None:
    prov = [
        *SPDX_LINES,
        "# Inputs of tools/gen_ebml_schema.py (chapter 08 §8.6.7). The commit lines are written by --fetch;",
    ]
    prov.append("# the hash lines are rewritten by every generator run and compared by --check.")
    inputs.mkdir(parents=True, exist_ok=True)
    for name, url, upstream, local in SOURCES:
        with tempfile.TemporaryDirectory(prefix="ovt-ebml-") as tmp:
            dest = Path(tmp) / "src"
            git("clone", "--quiet", "--depth", "1", url, str(dest))
            sha = git("rev-parse", "HEAD", cwd=dest)
            date = git("log", "-1", "--format=%cs", cwd=dest)
            shutil.copyfile(dest / upstream, inputs / local)
        prov.append(f"# {name} {url} {sha} {date}")
    prov.append(f"# snapshot {datetime.date.today().isoformat()}")
    (inputs / "INPUTS.sha256").write_text("\n".join(prov) + "\n", encoding="utf-8")
    (inputs / "INPUTS.sha256").write_text(render_inputs_sha(inputs), encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="fail when a committed output differs")
    mode.add_argument("--fetch", action="store_true", help="refresh the inputs from upstream, then regenerate")
    ap.add_argument("--inputs", type=Path, default=INPUTS, help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    try:
        if args.fetch:
            fetch(args.inputs)
        if not args.check:  # a plain run re-pins whatever the committed inputs are
            (args.inputs / "INPUTS.sha256").write_text(render_inputs_sha(args.inputs), encoding="utf-8")
        outputs = generate(args.inputs, verify=args.check)
    except (InputError, OSError, KeyError, ValueError, ET.ParseError, subprocess.CalledProcessError) as e:
        print(f"gen_ebml_schema: {e}", file=sys.stderr)
        return 2
    if args.check:
        stale = False
        for path, text in outputs.items():
            current = path.read_text("utf-8") if path.exists() else ""
            if current != text:
                stale = True
                rel = path.relative_to(ROOT).as_posix() if path.is_relative_to(ROOT) else str(path)
                sys.stdout.writelines(
                    difflib.unified_diff(current.splitlines(True), text.splitlines(True), rel, rel + " (generated)")
                )
        if stale:
            print(
                "gen_ebml_schema: committed outputs are stale; run python3 -I tools/gen_ebml_schema.py", file=sys.stderr
            )
        return 1 if stale else 0
    for path, text in outputs.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
