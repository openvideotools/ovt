#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Check that a FlatBuffers schema file equals its fenced block in the spec (chapter 06 WI-0.080).

    check_schema_in_spec.py [--write] SPEC_MD SCHEMA_FBS [SCHEMA_FBS ...]

Chapter 06 §6.3/§6.4/§6.5 state that the schema text in the spec *is* the interface and that the file
under core/schema/ must stay identical to it. For every schema argument this script looks in SPEC_MD for
the ```fbs fenced block whose first line starts with `// core/schema/<basename>` and compares the block
with the file, byte for byte after normalising line endings and stripping trailing whitespace (the
trailing-whitespace pre-commit hook strips it from the file; the spec may keep it). Exit 0 when every file matches, 1 with a
unified diff otherwise, 2 on a usage or lookup error. `--write` copies the block into the file instead
(used once by the WI that lands a schema; CI runs the check). Run with `python -I`; standard library only.
"""

from __future__ import annotations

import argparse
import difflib
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


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("spec", type=Path, help="docs/spec/06-data-model.md")
    parser.add_argument("schemas", type=Path, nargs="+", help="core/schema/*.fbs")
    parser.add_argument("--write", action="store_true", help="write the spec block into the file")
    args = parser.parse_args(argv)

    try:
        spec_text = args.spec.read_text(encoding="utf-8").replace("\r\n", "\n")
    except OSError as exc:
        print(f"check_schema_in_spec: cannot read {args.spec}: {exc}", file=sys.stderr)
        return 2

    status = 0
    for schema in args.schemas:
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
        if actual == block:
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
