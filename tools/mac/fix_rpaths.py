#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Verify (and optionally repair) the dynamic-library references of a macOS .app bundle.

Chapter 13 §13.5.2, WI-0.060. Every Mach-O file inside the bundle may reference only `@rpath/`, `@executable_path/`
or `@loader_path/` paths, `/usr/lib/` and `/System/`; every `@rpath/` reference must resolve through the file's own
LC_RPATH entries to a file that exists inside the bundle.

    fix_rpaths.py <OVT Studio.app> [--fix] [--otool P] [--install-name-tool P]

Without --fix the script only reports. Exit status: 0 clean, 1 violations remain, 2 usage or tool error.

--fix first (a) gives every library in Contents/Frameworks the install name @rpath/<path relative to Frameworks>,
(b) rewrites every reference to a library that lives in Contents/Frameworks (matched by file name, or by the
`X.framework/...` tail) to @rpath/..., and (c) replaces the LC_RPATH entries by the single one that reaches
Contents/Frameworks from the file's location, removing duplicate LC_RPATH entries one copy at a time. It then
verifies; a duplicate LC_RPATH is a problem too. `macdeployqt` does (a) to (c) for what it copies;
this covers what it misses, namely libraries that are only loaded through `dlopen` (the plugins in
Contents/PlugIns/ovt/) and our own build-tree libraries.

The tools are run through --otool / --install-name-tool so the tests can substitute fakes on Linux.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ALLOWED_PREFIXES = ("@rpath/", "@executable_path/", "@loader_path/", "/usr/lib/", "/System/")
MACHO_MAGICS = {
    b"\xfe\xed\xfa\xce",
    b"\xfe\xed\xfa\xcf",
    b"\xce\xfa\xed\xfe",
    b"\xcf\xfa\xed\xfe",
    b"\xca\xfe\xba\xbe",  # fat; also Java classes, which do not occur in a bundle
    b"\xbe\xba\xfe\xca",
}


class ToolError(RuntimeError):
    pass


def is_macho(path: Path) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    try:
        with path.open("rb") as f:
            return f.read(4) in MACHO_MAGICS
    except OSError:
        return False


def find_machos(app: Path) -> list[Path]:
    found = []
    for root, dirs, files in os.walk(app):
        dirs.sort()
        for name in sorted(files):
            p = Path(root) / name
            if is_macho(p):
                found.append(p)
    return found


def run(cmd: list[str]) -> str:
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, check=False)
    except OSError as e:
        raise ToolError(f"cannot run {cmd[0]}: {e}") from e
    if r.returncode != 0:
        raise ToolError(f"{' '.join(cmd)} failed ({r.returncode}): {r.stderr.strip()}")
    return r.stdout


def list_deps(otool: str, path: Path) -> list[str]:
    """`otool -L`: the install name of a library first (when it has one), then the dependencies."""
    out = run([otool, "-L", str(path)]).splitlines()
    deps = []
    for line in out[1:]:  # line 0 is "<path>:"
        line = line.strip()
        if not line:
            continue
        # "<name> (compatibility version 1.0.0, current version 1.2.3)"
        deps.append(line.split(" (compatibility", 1)[0].strip())
    return deps


def list_rpaths(otool: str, path: Path) -> list[str]:
    out = run([otool, "-l", str(path)]).splitlines()
    rpaths = []
    for i, line in enumerate(out):
        if line.strip() == "cmd LC_RPATH":
            for follow in out[i + 1 : i + 4]:
                follow = follow.strip()
                if follow.startswith("path "):
                    rpaths.append(follow[len("path ") :].rsplit(" (offset", 1)[0].strip())
                    break
    return rpaths


def expand(ref: str, file: Path, app: Path) -> Path:
    contents = app / "Contents"
    if ref == "@executable_path" or ref.startswith("@executable_path/"):
        return contents / "MacOS" / ref[len("@executable_path/") :]
    if ref == "@loader_path" or ref.startswith("@loader_path/"):
        return file.parent / ref[len("@loader_path/") :]
    return Path(ref)


def resolve_rpath_dep(dep: str, file: Path, app: Path, rpaths: list[str]) -> bool:
    tail = dep[len("@rpath/") :]
    for rp in rpaths:
        base = expand(rp, file, app) if rp.startswith("@") else Path(rp)
        if Path(os.path.normpath(base / tail)).exists():
            return True
    return False


def check_file(app: Path, file: Path, otool: str) -> list[str]:
    problems = []
    rpaths = list_rpaths(otool, file)
    rel = file.relative_to(app)
    deps = list_deps(otool, file)
    is_lib = file.suffix == ".dylib" or ".framework/" in file.as_posix()
    for n, dep in enumerate(deps):
        if not dep.startswith(ALLOWED_PREFIXES):
            problems.append(f"{rel}: forbidden reference {dep}")
        elif is_lib and n == 0:
            continue  # the library's own install name: nothing has to resolve to it by name
        elif dep.startswith("@rpath/") and not resolve_rpath_dep(dep, file, app, rpaths):
            problems.append(f"{rel}: {dep} does not resolve through its LC_RPATH {rpaths}")
    for rp in rpaths:
        if rp not in ("@loader_path", "@executable_path") and not rp.startswith(("@executable_path/", "@loader_path/")):
            problems.append(f"{rel}: LC_RPATH outside the bundle: {rp}")
    for rp in dict.fromkeys(rp for rp in rpaths if rpaths.count(rp) > 1):
        problems.append(f"{rel}: duplicate LC_RPATH {rp}")  # recent dyld versions reject it
    return problems


def frameworks_index(app: Path) -> dict[str, str]:
    """Map a base name, and a `X.framework/...` tail, to the @rpath/ name of what Contents/Frameworks holds."""
    fw = app / "Contents" / "Frameworks"
    index: dict[str, str] = {}
    if not fw.is_dir():
        return index
    for root, dirs, files in os.walk(fw):
        dirs.sort()
        for name in sorted(files):
            p = Path(root) / name
            if not is_macho(p):
                continue
            rel = p.relative_to(fw).as_posix()
            index.setdefault(name, "@rpath/" + rel)
            tail = framework_tail(rel)
            if tail:
                index.setdefault(tail, "@rpath/" + rel)
    return index


def framework_tail(path: str) -> str | None:
    parts = path.split("/")
    for i, part in enumerate(parts):
        if part.endswith(".framework"):
            return "/".join(parts[i:])
    return None


def rpath_for(app: Path, file: Path) -> str:
    fw = app / "Contents" / "Frameworks"
    rel = os.path.relpath(fw, file.parent).replace(os.sep, "/")
    return "@loader_path" if rel == "." else "@loader_path/" + rel


def lookup(dep: str, index: dict[str, str]) -> str | None:
    tail = framework_tail(dep)
    if tail and tail in index:
        return index[tail]
    return index.get(dep.rsplit("/", 1)[-1])


def fix_file(app: Path, file: Path, otool: str, int_tool: str, index: dict[str, str]) -> None:
    fw = app / "Contents" / "Frameworks"
    deps = list_deps(otool, file)
    is_lib = file.suffix == ".dylib" or ".framework/" in file.as_posix()
    cmd: list[str] = [int_tool]
    if is_lib and deps:  # `otool -L` prints the install name of a library first
        if fw in file.parents:
            want_id = "@rpath/" + file.relative_to(fw).as_posix()
        else:
            want_id = "@rpath/" + file.name  # a plugin: nothing links it by name
        if deps[0] != want_id:
            cmd += ["-id", want_id]
        deps = deps[1:]
    for dep in deps:
        if dep.startswith(ALLOWED_PREFIXES):
            continue
        new = lookup(dep, index)
        if new:
            cmd += ["-change", dep, new]
    existing = list_rpaths(otool, file)
    want = rpath_for(app, file)
    # install_name_tool rejects the same -delete_rpath twice in one call, and one -delete_rpath removes one LC_RPATH.
    # Qt's frameworks carry some LC_RPATH twice, so name each path once here and remove the extra copies below.
    for rp in dict.fromkeys(existing):
        if rp != want:
            cmd += ["-delete_rpath", rp]
    if want not in existing:
        cmd += ["-add_rpath", want]
    if len(cmd) > 1:
        run(cmd + [str(file)])
    if len(set(existing)) != len(existing):
        drop_duplicate_rpaths(file, want, otool, int_tool, len(existing))


def drop_duplicate_rpaths(file: Path, want: str, otool: str, int_tool: str, bound: int) -> None:
    """Delete LC_RPATH copies one call at a time until only `want` is left, once.

    Re-reading after each call keeps this right whether install_name_tool removes the first copy or every copy.
    """
    for _ in range(bound):
        current = list_rpaths(otool, file)
        extra = next((rp for rp in current if rp != want), None)
        if extra is None and current.count(want) > 1:
            extra = want
        if extra is None:
            break
        run([int_tool, "-delete_rpath", extra, str(file)])
    if want not in list_rpaths(otool, file):
        run([int_tool, "-add_rpath", want, str(file)])


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="fix_rpaths.py", description=__doc__.split("\n\n")[0])
    ap.add_argument("app", type=Path, help="path of 'OVT Studio.app'")
    ap.add_argument("--fix", action="store_true", help="repair install names, references and rpaths first")
    ap.add_argument("--otool", default="otool")
    ap.add_argument("--install-name-tool", default="install_name_tool")
    args = ap.parse_args(argv)
    app: Path = args.app
    if not (app / "Contents").is_dir():
        print(f"fix_rpaths.py: {app} is not an app bundle (no Contents/)", file=sys.stderr)
        return 2
    try:
        machos = find_machos(app)
        if not machos:
            print(f"fix_rpaths.py: no Mach-O file under {app}", file=sys.stderr)
            return 2
        if args.fix:
            index = frameworks_index(app)
            for f in machos:
                fix_file(app, f, args.otool, args.install_name_tool, index)
        problems = []
        for f in machos:
            problems += check_file(app, f, args.otool)
    except ToolError as e:
        print(f"fix_rpaths.py: {e}", file=sys.stderr)
        return 2
    for p in problems:
        print(p, file=sys.stderr)
    print(f"fix_rpaths.py: {len(machos)} Mach-O files checked, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
