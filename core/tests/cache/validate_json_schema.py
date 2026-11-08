#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""CTest driver of the cache's JSON Schema acceptance tests (chapter 06 §6.7.1–6.7.2).

    validate_json_schema.py TEST_EXECUTABLE TEST_SPEC OUT_DIR GLOB SCHEMA_JSON

Runs the Catch2 test case(s) TEST_SPEC of TEST_EXECUTABLE with OVT_TEST_FIXED_DIR=OUT_DIR (a directory of its
own, so the run never races the plain CTest run of the same test case), which leaves exactly one file
matching GLOB below OUT_DIR behind, then validates that file with `python -m jsonschema -i <file> SCHEMA_JSON`, the command the
acceptance lines name: WI-0.091 acceptance test 1 (meta.json, cache-meta-1.schema.json) and WI-0.092
acceptance test 4 (cache.json, cache-json-1.schema.json). Exit 0 on success, 1 on failure, 77 (CTest
SKIP_RETURN_CODE) when the jsonschema package is not installed.
"""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys


def main(argv: list[str]) -> int:
    if len(argv) != 5:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    exe, spec, out_dir, pattern, schema = argv[0], argv[1], pathlib.Path(argv[2]), argv[3], argv[4]
    try:
        import jsonschema  # noqa: F401
    except ImportError:
        print("skipped: python package 'jsonschema' is not installed (tools/requirements.txt)")
        return 77
    env = dict(os.environ, OVT_TEST_FIXED_DIR=str(out_dir))
    run = subprocess.run([exe, spec], capture_output=True, text=True, check=False, env=env)
    if run.returncode != 0:
        print(run.stdout, run.stderr, file=sys.stderr)
        return 1
    found = sorted(out_dir.glob(pattern))
    if len(found) != 1:
        print(f"expected one {pattern} under {out_dir}, found {found}", file=sys.stderr)
        return 1
    check = subprocess.run([sys.executable, "-I", "-m", "jsonschema", "-i", str(found[0]), schema], check=False)
    if check.returncode != 0:
        print(f"{found[0]} does not validate against {schema}", file=sys.stderr)
        return 1
    print(f"{found[0]} validates against {schema}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
