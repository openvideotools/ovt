#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check that a FlatBuffers schema file equals its fenced block in the spec (chapter 06 WI-0.080).

    check_schema_in_spec.py [--write] SPEC_MD SCHEMA [SCHEMA ...]

Chapter 06 §6.3/§6.4/§6.5 state that the schema text in the spec *is* the interface and that the file
under core/schema/ must stay identical to it. For every schema argument this script looks in SPEC_MD for
the ```fbs fenced block whose first line starts with `// core/schema/<basename>` and compares the block
with the file, byte for byte after normalising line endings and stripping trailing whitespace (the
trailing-whitespace pre-commit hook strips it from the file; the spec may keep it). Exit 0 when every file matches, 1 with a
unified diff otherwise, 2 on a usage or lookup error. `--write` copies the block into the file instead
(used once by the WI that lands a schema; CI runs the check). A SCHEMA that is a directory stands for every
`*.fbs` file in it, in name order (CI passes `core/schema`, so a new schema file is covered without a CI
edit; WI-0.081). When the block is followed by a `**Table list ...:**` paragraph (§6.4), the backticked
names in it must equal the tables, structs, enums and unions the block declares, or the check fails too
(WI-0.081 step 3). Run with `python -I`; standard library only.
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
from pathlib import Path

FENCE_OPEN = "```fbs"
FENCE_CLOSE = "```"


def normalise(text: str) -> str:
    """LF line endings, no trailing whitespace on any line, exactly one newline at the end."""
    return "\n".join(line.rstrip() for line in text.replace("\r\n", "\n").split("\n")).rstrip("\n") + "\n"


def extract_block(spec_text: str, basename: str) -> str:
    """The ```fbs block of SPEC_MD whose first line names core/schema/<basename>; raises LookupError."""
    marker = f"// core/schema/{basename}"
    lines = spec_text.splitlines()
    i = 0
    while i < len(lines):
        if lines[i].strip() == FENCE_OPEN and i + 1 < len(lines) and lines[i + 1].startswith(marker):
            j = i + 1
            while j < len(lines) and lines[j].strip() != FENCE_CLOSE:
                j += 1
            if j >= len(lines):
                raise LookupError(f"unterminated {FENCE_OPEN} block for {basename} at line {i + 1}")
            return normalise("\n".join(lines[i + 1 : j]))
        i += 1
    raise LookupError(f"no {FENCE_OPEN} block starting with '{marker}' in the spec")


TABLE_LIST_PREFIX = "**Table list"
_DECLARATION = re.compile(r"\b(?:table|struct|enum|union)\s+([A-Za-z_]\w*)\s*(?::\s*\w+\s*)?\{")
_COMMENTS = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)
_BACKTICKED = re.compile(r"`([^`]+)`")


def extract_table_list(spec_text: str, basename: str) -> list[str] | None:
    """Names of the `**Table list` paragraph that follows the block for <basename>, or None if there is none.

    Only the text between the block's closing fence and the next heading or fence is searched. Backticked
    tokens that are not plain identifiers (file paths such as `tools/check_schema_in_spec.py`) are skipped.
    """
    marker = f"// core/schema/{basename}"
    lines = spec_text.splitlines()
    start = next(
        (i for i in range(len(lines) - 1) if lines[i].strip() == FENCE_OPEN and lines[i + 1].startswith(marker)),
        None,
    )
    if start is None:
        return None
    end = next(
        (j for j in range(start + 1, len(lines)) if lines[j].strip() == FENCE_CLOSE),
        len(lines),
    )
    for line in lines[end + 1 :]:
        if line.startswith("#") or line.strip().startswith(FENCE_CLOSE):
            return None
        if line.startswith(TABLE_LIST_PREFIX):
            return [name for name in _BACKTICKED.findall(line) if re.fullmatch(r"[A-Za-z_]\w*", name)]
    return None


def declared_names(schema_text: str) -> list[str]:
    """Tables, structs, enums and unions a schema text declares, in order (comments ignored)."""
    return _DECLARATION.findall(_COMMENTS.sub("", schema_text))


def table_list_problems(spec_text: str, basename: str, block: str) -> list[str]:
    """Differences between the spec's table list for <basename> and the declarations of its block."""
    listed = extract_table_list(spec_text, basename)
    if listed is None:
        return []
    declared = declared_names(block)
    problems = [
        f"declared in the schema but missing from the table list: {name}" for name in declared if name not in listed
    ]
    problems += [f"in the table list but not declared in the schema: {name}" for name in listed if name not in declared]
    problems += [f"listed more than once: {name}" for name in sorted({n for n in listed if listed.count(n) > 1})]
    return problems


def expand_schemas(paths: list[Path]) -> list[Path]:
    """Directories become their *.fbs files (sorted); files are kept as given. Raises LookupError."""
    out: list[Path] = []
    for path in paths:
        if path.is_dir():
            found = sorted(path.glob("*.fbs"))
            if not found:
                raise LookupError(f"no *.fbs files in directory {path}")
            out.extend(found)
        else:
            out.append(path)
    return out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("spec", type=Path, help="docs/spec/06-data-model.md")
    parser.add_argument(
        "schemas",
        type=Path,
        nargs="+",
        help="core/schema/*.fbs files or the core/schema directory",
    )
    parser.add_argument("--write", action="store_true", help="write the spec block into the file")
    args = parser.parse_args(argv)

    try:
        spec_text = args.spec.read_text(encoding="utf-8").replace("\r\n", "\n")
    except OSError as exc:
        print(f"check_schema_in_spec: cannot read {args.spec}: {exc}", file=sys.stderr)
        return 2

    try:
        schemas = expand_schemas(args.schemas)
    except LookupError as exc:
        print(f"check_schema_in_spec: {exc}", file=sys.stderr)
        return 2

    status = 0
    for schema in schemas:
        try:
            block = extract_block(spec_text, schema.name)
        except LookupError as exc:
            print(f"check_schema_in_spec: {schema}: {exc}", file=sys.stderr)
            return 2
        if args.write:
            schema.write_text(block, encoding="utf-8", newline="\n")
            print(f"check_schema_in_spec: wrote {schema} ({len(block.splitlines())} lines)")
            continue
        try:
            actual = normalise(schema.read_text(encoding="utf-8"))
        except OSError as exc:
            print(f"check_schema_in_spec: cannot read {schema}: {exc}", file=sys.stderr)
            return 2
        problems = table_list_problems(spec_text, schema.name, block)
        for problem in problems:
            print(
                f"check_schema_in_spec: {schema.name} table list (spec): {problem}",
                file=sys.stderr,
            )
        if problems:
            status = 1
        if actual == block:
            if not problems:
                print(f"check_schema_in_spec: {schema} matches {args.spec}")
            continue
        status = 1
        diff = difflib.unified_diff(
            block.splitlines(keepends=True),
            actual.splitlines(keepends=True),
            fromfile=f"{args.spec} (block for {schema.name})",
            tofile=str(schema),
        )
        sys.stdout.writelines(diff)
        print(
            f"check_schema_in_spec: {schema} differs from the spec block",
            file=sys.stderr,
        )
    return status


if __name__ == "__main__":
    sys.exit(main())
