#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Check that libde265 patch 0001 changes nothing when DE265_INTERNALS is OFF (WI-2.162 acceptance test 1).

    check_internals_abi.py --nm NM --upstream DIR --off DIR --on DIR

Each DIR is the CMake binary directory of one build of the pinned libde265 tarball, all with the same compiler and
flags (plugins/ovt-plugin-de265/tests/CMakeLists.txt): pristine upstream, patched with DE265_INTERNALS=OFF, patched
with DE265_INTERNALS=ON. The check passes when

  1. the OFF library defines exactly the symbols of the upstream library (`nm -g`),
  2. every object file of the OFF build, deblock.cc.o included, is byte-identical to the upstream one,
  3. the ON library defines every upstream symbol plus only `de265_internals_*` functions, and at least one.

Exit 0 on success, 1 on a mismatch (each printed), 2 on usage or a missing build output.
Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

LIBRARY = Path("libde265") / "libde265.a"
OBJECTS = Path("libde265")  # de265.dir plus the x86 and encoder object libraries
INTERNALS_RE = re.compile(r"^_?de265_internals_[A-Za-z0-9_]+$")  # Mach-O prefixes C symbols with "_"


def defined_symbols(nm: str, library: Path) -> set[str]:
    """Names of the external symbols @p library defines (any `nm` type except undefined ones)."""
    out = subprocess.run([nm, "-g", str(library)], check=True, capture_output=True, text=True).stdout
    names = set()
    for line in out.splitlines():
        fields = line.split()
        # "<address> <type> <name>"; undefined symbols have no address. Archive member headers end in ':'.
        if len(fields) == 3 and fields[1] not in ("U", "w", "v"):
            names.add(fields[2])
    return names


def objects(build: Path) -> dict[str, Path]:
    return {p.relative_to(build / OBJECTS).as_posix(): p for p in sorted((build / OBJECTS).rglob("*.o"))}


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--nm", required=True)
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--off", type=Path, required=True)
    parser.add_argument("--on", type=Path, required=True)
    args = parser.parse_args(argv)

    for build in (args.upstream, args.off, args.on):
        if not (build / LIBRARY).is_file():
            print(f"check_internals_abi: {build / LIBRARY} does not exist; build the target first", file=sys.stderr)
            return 2

    errors: list[str] = []
    upstream = defined_symbols(args.nm, args.upstream / LIBRARY)
    off = defined_symbols(args.nm, args.off / LIBRARY)
    on = defined_symbols(args.nm, args.on / LIBRARY)

    for name in sorted(off - upstream):
        errors.append(f"DE265_INTERNALS=OFF adds symbol {name}")
    for name in sorted(upstream - off):
        errors.append(f"DE265_INTERNALS=OFF removes symbol {name}")

    upstream_objects = objects(args.upstream)
    off_objects = objects(args.off)
    if not any(name.endswith("/deblock.cc.o") for name in upstream_objects):
        errors.append(f"no deblock.cc.o under {args.upstream / OBJECTS}")
    if sorted(upstream_objects) != sorted(off_objects):
        errors.append(f"object lists differ: upstream {sorted(upstream_objects)}, OFF {sorted(off_objects)}")
    for name, path in upstream_objects.items():
        other = off_objects.get(name)
        if other is not None and path.read_bytes() != other.read_bytes():
            errors.append(f"DE265_INTERNALS=OFF changes the object code of {name}")

    added = on - upstream
    for name in sorted(upstream - on):
        errors.append(f"DE265_INTERNALS=ON removes symbol {name}")
    for name in sorted(n for n in added if not INTERNALS_RE.match(n)):
        errors.append(f"DE265_INTERNALS=ON adds symbol {name}, which is not a de265_internals_ function")
    if not added:
        errors.append("DE265_INTERNALS=ON adds no de265_internals_ symbol")

    for error in errors:
        print(f"check_internals_abi: {error}", file=sys.stderr)
    if errors:
        return 1
    print(
        f"check_internals_abi: OFF = upstream ({len(upstream)} symbols, {len(upstream_objects)} identical objects); "
        f"ON adds {len(added)} de265_internals_ functions"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
