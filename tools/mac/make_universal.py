#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Merge an arm64 and an x86_64 "OVT Studio.app" into one universal2 bundle (chapter 13 §13.5.1, WI-0.063).

    make_universal.py --arm64 <arm64 .app> --x86_64 <x86_64 .app> --out <universal .app>
                      [--identity ID] [--entitlements PLIST] [--no-sign] [--allow-differing GLOB ...]
                      [--lipo P] [--codesign P]

Both inputs are complete bundles from tools/mac/bundle.sh, built from the same commit with the same Qt. The output
tree is a copy of the arm64 bundle in which every Mach-O file is replaced by `lipo -create` of its arm64 slice and
the x86_64 slice of the file at the same path in the other bundle. A file that is already fat (Qt from aqtinstall is
universal) contributes only the slice of its own architecture (`lipo -thin`), so `lipo -create` never sees one
architecture twice.

The two trees must otherwise match: the same set of paths, the same symlink targets, and byte-identical non-Mach-O
files. A difference means the inputs do not come from the same commit or Qt version (the WI's pitfall) and is an
error, except for `_CodeSignature/` contents (they hash architecture-specific code and are rebuilt by the signing
step) and paths matched by --allow-differing (fnmatch on the path relative to the bundle; the arm64 copy is kept).

Every merged Mach-O is then checked with `lipo -archs`: it must hold exactly arm64 and x86_64. Finally the bundle is
re-signed, inside-out as in §13.5.4 steps 1 and 2 (loose Mach-O files deepest first, then each framework, then the
executables in Contents/MacOS other than the main one, then the bundle itself). The default identity is "-" (ad
hoc, as tools/mac/bundle.sh signs), because an arm64 Mach-O must carry a signature to run and lipo invalidates the
bundle seal. A real identity adds the hardened runtime and a secure timestamp. --entitlements (default
tools/mac/entitlements.plist when that file exists, WI-0.061) goes on the Contents/MacOS executables and the bundle.
--no-sign leaves signing to tools/mac/sign_and_notarize.sh (WI-0.061).

Exit status: 0 success, 1 the inputs do not match or a merged file lacks an architecture, 2 usage or tool error.
On failure the output bundle is removed.
The Apple tools are run through --lipo / --codesign so the tests can substitute fakes on Linux.
"""

from __future__ import annotations

import argparse
import filecmp
import fnmatch
import os
import shutil
import subprocess
import sys
from pathlib import Path

ARCHS = ("arm64", "x86_64")
THIN_MAGICS = {b"\xfe\xed\xfa\xce", b"\xfe\xed\xfa\xcf", b"\xce\xfa\xed\xfe", b"\xcf\xfa\xed\xfe"}
FAT_MAGICS = {b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"}
REPO = Path(__file__).resolve().parents[2]


class ToolError(RuntimeError):
    pass


class Mismatch(RuntimeError):
    pass


def magic(path: Path) -> bytes:
    try:
        with path.open("rb") as f:
            return f.read(4)
    except OSError:
        return b""


def is_macho(path: Path) -> bool:
    return not path.is_symlink() and path.is_file() and magic(path) in THIN_MAGICS | FAT_MAGICS


def run(cmd: list[str]) -> str:
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, check=False)
    except OSError as e:
        raise ToolError(f"cannot run {cmd[0]}: {e}") from e
    if r.returncode != 0:
        raise ToolError(f"{' '.join(cmd)} failed ({r.returncode}): {r.stderr.strip()}")
    return r.stdout


def archs_of(lipo: str, path: Path) -> list[str]:
    return run([lipo, "-archs", str(path)]).split()


def walk(root: Path) -> dict[str, Path]:
    """Every file, symlink and directory under root, keyed by its POSIX path relative to root. Symlinks are not
    followed, so a framework's Versions/Current link is compared as a link."""
    entries: dict[str, Path] = {}
    for dirpath, dirs, files in os.walk(root):
        dirs.sort()
        base = Path(dirpath)
        for name in sorted(dirs + files):
            p = base / name
            entries[p.relative_to(root).as_posix()] = p
    return entries


def kind(p: Path) -> str:
    if p.is_symlink():
        return "symlink"
    if p.is_dir():
        return "directory"
    return "Mach-O" if is_macho(p) else "file"


def is_signature(rel: str) -> bool:
    return "_CodeSignature/" in rel or rel.endswith(("_CodeSignature", "/CodeResources"))


def compare_trees(arm: Path, x86: Path, allow: list[str]) -> list[str]:
    """The Mach-O paths to merge (relative). Raises Mismatch listing every difference that is not allowed."""
    a, b = walk(arm), walk(x86)
    problems: list[str] = []
    machos: list[str] = []
    for rel in sorted(set(a) | set(b)):
        if is_signature(rel):
            continue
        allowed = any(fnmatch.fnmatchcase(rel, g) for g in allow)
        if rel not in a or rel not in b:
            if not allowed:
                problems.append(f"{rel}: only in the {'arm64' if rel in a else 'x86_64'} bundle")
            continue
        pa, pb = a[rel], b[rel]
        ka, kb = kind(pa), kind(pb)
        if ka != kb:
            problems.append(f"{rel}: {ka} in the arm64 bundle but {kb} in the x86_64 bundle")
        elif ka == "Mach-O":
            machos.append(rel)
        elif ka == "symlink":
            if os.readlink(pa) != os.readlink(pb):
                problems.append(f"{rel}: symlink targets differ ({os.readlink(pa)} vs {os.readlink(pb)})")
        elif ka == "file" and not allowed and not filecmp.cmp(pa, pb, shallow=False):
            problems.append(f"{rel}: contents differ")
    if problems:
        raise Mismatch("\n".join(problems))
    return machos


def slice_of(lipo: str, src: Path, arch: str, scratch: Path) -> Path:
    archs = archs_of(lipo, src)
    if arch not in archs:
        raise Mismatch(f"{src}: has {' '.join(archs) or 'no architecture'}, not {arch}")
    if archs == [arch]:
        return src
    out = scratch / f"{arch}.slice"
    run([lipo, str(src), "-thin", arch, "-output", str(out)])
    return out


def merge(lipo: str, arm: Path, x86: Path, out: Path, rels: list[str]) -> None:
    scratch = out.parent / (out.name + ".lipo-tmp")
    scratch.mkdir(exist_ok=True)
    try:
        for rel in rels:
            dst = out / rel
            mode = dst.stat().st_mode
            sa = slice_of(lipo, arm / rel, "arm64", scratch)
            sx = slice_of(lipo, x86 / rel, "x86_64", scratch)
            merged = scratch / "merged"
            run([lipo, "-create", str(sa), str(sx), "-output", str(merged)])
            shutil.move(str(merged), dst)
            os.chmod(dst, mode)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


def verify(lipo: str, out: Path) -> list[str]:
    problems = []
    for rel, p in walk(out).items():
        if is_macho(p):
            got = archs_of(lipo, p)
            if sorted(got) != sorted(ARCHS):
                problems.append(f"{rel}: lipo -archs reports '{' '.join(got)}', want 'x86_64 arm64'")
    return problems


def main_executable(app: Path) -> str:
    import plistlib

    try:
        with (app / "Contents" / "Info.plist").open("rb") as f:
            return str(plistlib.load(f).get("CFBundleExecutable", ""))
    except (OSError, plistlib.InvalidFileException, ValueError):
        return ""


def sign(codesign: str, app: Path, identity: str, entitlements: Path | None) -> None:
    """Inside-out signing of the whole bundle (§13.5.4 steps 1 and 2)."""
    base = [codesign, "--force", "--sign", identity]
    base += ["--timestamp=none"] if identity == "-" else ["--options", "runtime", "--timestamp"]
    ent = ["--entitlements", str(entitlements)] if entitlements else []
    contents = app / "Contents"
    macos = contents / "MacOS"
    loose = [
        p for p in walk(contents).values() if is_macho(p) and ".framework/" not in p.as_posix() and p.parent != macos
    ]
    loose.sort(key=lambda p: (-len(p.parts), p.as_posix()))  # deepest first
    for p in loose:
        run(base + [str(p)])
    fw = contents / "Frameworks"
    if fw.is_dir():
        for p in sorted(fw.glob("*.framework")):
            run(base + [str(p)])
    main = main_executable(app)
    if macos.is_dir():
        for p in sorted(macos.iterdir()):
            if p.name != main and is_macho(p):
                run(base + ent + [str(p)])
    run(base + ent + [str(app)])  # signs the main executable with the bundle
    run([codesign, "--verify", "--deep", "--strict", str(app)])  # --deep is fine for verification


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="make_universal.py", description=__doc__.split("\n\n")[0])
    ap.add_argument("--arm64", type=Path, required=True, help="the arm64 bundle")
    ap.add_argument("--x86_64", type=Path, required=True, help="the x86_64 bundle")
    ap.add_argument("--out", type=Path, required=True, help="the universal bundle to create (must not exist)")
    ap.add_argument("--identity", default="-", help='codesign identity (default "-": ad hoc)')
    ap.add_argument("--entitlements", type=Path, help="entitlements for the executables and the bundle")
    ap.add_argument("--no-sign", action="store_true", help="leave signing to tools/mac/sign_and_notarize.sh")
    ap.add_argument("--allow-differing", action="append", default=[], metavar="GLOB")
    ap.add_argument("--lipo", default="lipo")
    ap.add_argument("--codesign", default="codesign")
    args = ap.parse_args(argv)

    for label, app in (("--arm64", args.arm64), ("--x86_64", args.x86_64)):
        if not (app / "Contents").is_dir():
            print(f"make_universal.py: {label} {app} is not an app bundle (no Contents/)", file=sys.stderr)
            return 2
    if args.out.exists():
        print(f"make_universal.py: {args.out} already exists", file=sys.stderr)
        return 2
    entitlements = args.entitlements
    if entitlements is None and (REPO / "tools/mac/entitlements.plist").is_file():
        entitlements = REPO / "tools/mac/entitlements.plist"
    if entitlements is not None and not entitlements.is_file():
        print(f"make_universal.py: entitlements file {entitlements} not found", file=sys.stderr)
        return 2

    try:
        rels = compare_trees(args.arm64, args.x86_64, args.allow_differing)
        if not rels:
            print(f"make_universal.py: no Mach-O file under {args.arm64}", file=sys.stderr)
            return 2
        args.out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(args.arm64, args.out, symlinks=True)
        merge(args.lipo, args.arm64, args.x86_64, args.out, rels)
        problems = verify(args.lipo, args.out)
        if problems:
            raise Mismatch("\n".join(problems))
        if not args.no_sign:
            sign(args.codesign, args.out, args.identity, entitlements)
    except Mismatch as e:
        print(f"make_universal.py: the bundles cannot be merged:\n{e}", file=sys.stderr)
        rc = 1
    except ToolError as e:
        print(f"make_universal.py: {e}", file=sys.stderr)
        rc = 2
    else:
        rc = 0
    if rc:
        shutil.rmtree(args.out, ignore_errors=True)  # never leave a half-merged bundle behind
        return rc
    signed = "not signed" if args.no_sign else f"signed {'ad hoc' if args.identity == '-' else 'as ' + args.identity}"
    print(f"make_universal.py: {args.out}: {len(rels)} Mach-O files merged (x86_64 arm64), {signed}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
