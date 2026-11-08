#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""CTest driver of WI-0.091 acceptance test 1, second half (chapter 06 §6.7.2).

    validate_meta_schema.py TEST_EXECUTABLE AT1_DIR SCHEMA_JSON

Runs the AT1 test case of ovt_tests_cache, which leaves AT1_DIR/<source_hash>/meta.json behind, then validates
that file with `python -m jsonschema -i meta.json SCHEMA_JSON`, the command the acceptance line names. Exit 0
on success, 1 on failure, 77 (CTest SKIP_RETURN_CODE) when the jsonschema package is not installed.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    exe, at1_dir, schema = argv[0], pathlib.Path(argv[1]), argv[2]
    try:
        import jsonschema  # noqa: F401
    except ImportError:
        print("skipped: python package 'jsonschema' is not installed (tools/requirements.txt)")
        return 77
    run = subprocess.run([exe, "test_cache: AT1*"], capture_output=True, text=True, check=False)
    if run.returncode != 0:
        print(run.stdout, run.stderr, file=sys.stderr)
        return 1
    metas = sorted(at1_dir.glob("*/meta.json"))
    if len(metas) != 1:
        print(f"expected one meta.json under {at1_dir}, found {metas}", file=sys.stderr)
        return 1
    check = subprocess.run([sys.executable, "-I", "-m", "jsonschema", "-i", str(metas[0]), schema], check=False)
    if check.returncode != 0:
        print(f"{metas[0]} does not validate against {schema}", file=sys.stderr)
        return 1
    print(f"{metas[0]} validates against {schema}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
