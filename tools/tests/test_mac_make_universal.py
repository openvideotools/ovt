# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/mac/make_universal.py (WI-0.063).

`lipo` and `codesign` exist only on macOS, so the tests run the script against fakes. A fake Mach-O is a Mach-O magic
followed by a line `archs=<a,b>` and a payload line; the fake `lipo` reads and writes that format, and the fake
`codesign` appends each call to a log. The real tools are exercised by the manual record
docs/acceptance/wi/WI-0.063.md on a Mac.
"""

from __future__ import annotations

import os
import plistlib
import subprocess
import sys
from pathlib import Path

MAC = Path(__file__).resolve().parents[1] / "mac"
SCRIPT = MAC / "make_universal.py"

THIN = b"\xcf\xfa\xed\xfe"
FAT = b"\xca\xfe\xba\xbe"

FAKE_LIPO = r"""#!/usr/bin/env python3
import sys

def read(p):
    data = open(p, "rb").read()
    head, archs, payload = data.split(b"\n", 2)
    return archs.decode()[len("archs="):].split(","), payload

def write(p, archs, payload):
    magic = b"\xca\xfe\xba\xbe" if len(archs) > 1 else b"\xcf\xfa\xed\xfe"
    open(p, "wb").write(magic + b"\n" + ("archs=" + ",".join(archs)).encode() + b"\n" + payload)

a = sys.argv[1:]
if a[0] == "-archs":
    print(" ".join(read(a[1])[0]))
elif a[0] == "-create":
    out = a[a.index("-output") + 1]
    archs, payloads = [], []
    for p in a[1:a.index("-output")]:
        ar, pl = read(p)
        for x in ar:
            if x in archs:
                sys.exit("fatal error: same architecture " + x)
        archs += ar
        payloads.append(pl)
    write(out, archs, b"|".join(payloads))
elif a[1] == "-thin":
    archs, payload = read(a[0])
    if a[2] not in archs:
        sys.exit("does not contain " + a[2])
    write(a[a.index("-output") + 1], [a[2]], payload)
else:
    sys.exit("bad args")
"""

FAKE_CODESIGN = r"""#!/usr/bin/env python3
import os, sys
with open(os.environ["FAKE_LOG"], "a") as f:
    f.write(" ".join(sys.argv[1:]) + "\n")
"""


def macho(archs: list[str], payload: str) -> bytes:
    return (FAT if len(archs) > 1 else THIN) + b"\n" + ("archs=" + ",".join(archs)).encode() + b"\n" + payload.encode()


def make_app(root: Path, arch: str, extra: dict[str, bytes] | None = None) -> Path:
    app = root / arch / "OVT Studio.app"
    c = app / "Contents"
    files = {
        "Info.plist": plistlib.dumps(
            {"CFBundleExecutable": "OVT Studio", "CFBundleIdentifier": "org.openvideotools.studio"}
        ),
        "MacOS/OVT Studio": macho([arch], f"studio-{arch}"),
        "MacOS/ovt": macho([arch], f"cli-{arch}"),
        "Frameworks/libovt_core.1.dylib": macho([arch], f"core-{arch}"),
        # Qt from aqtinstall is universal already: both inputs carry both slices.
        "Frameworks/QtCore.framework/Versions/A/QtCore": macho(["x86_64", "arm64"], "qt"),
        "Frameworks/QtCore.framework/Versions/A/Resources/Info.plist": b"<plist/>",
        "PlugIns/ovt/libovt_plugin_aom.dylib": macho([arch], f"aom-{arch}"),
        "PlugIns/platforms/libqcocoa.dylib": macho([arch], f"cocoa-{arch}"),
        "Resources/THIRD_PARTY_NOTICES.md": b"notices\n",
        "Resources/PATENTS": b"AOM patents\n",
        "_CodeSignature/CodeResources": f"seal-{arch}".encode(),
    }
    files.update(extra or {})
    for rel, data in files.items():
        p = c / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        if rel.startswith("MacOS/"):
            p.chmod(0o755)
    fw = c / "Frameworks/QtCore.framework"
    os.symlink("A", fw / "Versions/Current")
    os.symlink("Versions/Current/QtCore", fw / "QtCore")
    return app


def tools(tmp: Path) -> None:
    for name, body in (("lipo", FAKE_LIPO), ("codesign", FAKE_CODESIGN)):
        t = tmp / name
        t.write_text(body.replace("#!/usr/bin/env python3", f"#!{sys.executable}"))
        t.chmod(0o755)


def run(tmp: Path, arm: Path, x86: Path, *extra: str) -> subprocess.CompletedProcess:
    tools(tmp)
    return subprocess.run(
        [
            sys.executable,
            "-I",
            str(SCRIPT),
            "--arm64",
            str(arm),
            "--x86_64",
            str(x86),
            "--out",
            str(tmp / "out" / "OVT Studio.app"),
            "--lipo",
            str(tmp / "lipo"),
            "--codesign",
            str(tmp / "codesign"),
            *extra,
        ],
        capture_output=True,
        text=True,
        env={"FAKE_LOG": str(tmp / "codesign.log")},
        check=False,
    )


def archs(tmp: Path, p: Path) -> list[str]:
    return subprocess.run(
        [str(tmp / "lipo"), "-archs", str(p)], capture_output=True, text=True, check=True
    ).stdout.split()


def machos(app: Path) -> list[Path]:
    return sorted(p for p in app.rglob("*") if not p.is_symlink() and p.is_file() and p.read_bytes()[:4] in (THIN, FAT))


def test_every_macho_is_universal(tmp_path):
    """AT1: given two per-architecture bundles from the same commit, every Mach-O reports x86_64 arm64."""
    arm, x86 = make_app(tmp_path, "arm64"), make_app(tmp_path, "x86_64")
    r = run(tmp_path, arm, x86)
    assert r.returncode == 0, r.stderr
    out = tmp_path / "out" / "OVT Studio.app"
    found = machos(out)
    assert len(found) == 6
    for p in found:
        assert sorted(archs(tmp_path, p)) == ["arm64", "x86_64"], p
    assert "6 Mach-O files merged (x86_64 arm64), signed ad hoc" in r.stdout


def test_slices_come_from_the_matching_bundle(tmp_path):
    arm, x86 = make_app(tmp_path, "arm64"), make_app(tmp_path, "x86_64")
    assert run(tmp_path, arm, x86).returncode == 0
    out = tmp_path / "out" / "OVT Studio.app/Contents"
    assert (out / "MacOS/ovt").read_bytes().endswith(b"cli-arm64|cli-x86_64")
    assert (out / "Frameworks/QtCore.framework/Versions/A/QtCore").read_bytes().endswith(b"qt|qt")
    assert os.access(out / "MacOS/ovt", os.X_OK)


def test_symlinks_and_resources_are_kept(tmp_path):
    arm, x86 = make_app(tmp_path, "arm64"), make_app(tmp_path, "x86_64")
    assert run(tmp_path, arm, x86).returncode == 0
    out = tmp_path / "out" / "OVT Studio.app/Contents"
    assert os.readlink(out / "Frameworks/QtCore.framework/Versions/Current") == "A"
    assert (out / "Resources/PATENTS").read_bytes() == b"AOM patents\n"


def test_signing_is_inside_out(tmp_path):
    arm, x86 = make_app(tmp_path, "arm64"), make_app(tmp_path, "x86_64")
    assert run(tmp_path, arm, x86).returncode == 0
    log = (tmp_path / "codesign.log").read_text().splitlines()
    targets = [line.split("OVT Studio.app")[-1] for line in log]
    assert targets == [
        "/Contents/PlugIns/ovt/libovt_plugin_aom.dylib",
        "/Contents/PlugIns/platforms/libqcocoa.dylib",
        "/Contents/Frameworks/libovt_core.1.dylib",
        "/Contents/Frameworks/QtCore.framework",
        "/Contents/MacOS/ovt",
        "",  # the bundle
        "",  # verification
    ]
    assert all("--sign - --timestamp=none" in line for line in log[:-1])
    assert log[-1].startswith("--verify --deep --strict")


def test_real_identity_uses_hardened_runtime_and_entitlements(tmp_path):
    arm, x86 = make_app(tmp_path, "arm64"), make_app(tmp_path, "x86_64")
    ent = tmp_path / "entitlements.plist"
    ent.write_bytes(plistlib.dumps({"com.apple.security.cs.disable-library-validation": True}))
    r = run(tmp_path, arm, x86, "--identity", "Developer ID Application: X (T)", "--entitlements", str(ent))
    assert r.returncode == 0, r.stderr
    log = (tmp_path / "codesign.log").read_text().splitlines()
    assert all("--options runtime --timestamp" in line for line in log[:-1])
    with_ent = [line for line in log if "--entitlements" in line]
    assert len(with_ent) == 2  # MacOS/ovt and the bundle
    assert not any("--entitlements" in line for line in log[:4])


def test_no_sign(tmp_path):
    arm, x86 = make_app(tmp_path, "arm64"), make_app(tmp_path, "x86_64")
    r = run(tmp_path, arm, x86, "--no-sign")
    assert r.returncode == 0, r.stderr
    assert not (tmp_path / "codesign.log").exists()


def test_differing_resource_fails_and_leaves_no_output(tmp_path):
    arm = make_app(tmp_path, "arm64")
    x86 = make_app(tmp_path, "x86_64", {"Resources/THIRD_PARTY_NOTICES.md": b"other Qt version\n"})
    r = run(tmp_path, arm, x86)
    assert r.returncode == 1
    assert "Resources/THIRD_PARTY_NOTICES.md: contents differ" in r.stderr
    assert not (tmp_path / "out" / "OVT Studio.app").exists()


def test_allow_differing_keeps_the_arm64_copy(tmp_path):
    arm = make_app(tmp_path, "arm64")
    x86 = make_app(tmp_path, "x86_64", {"Resources/THIRD_PARTY_NOTICES.md": b"other\n"})
    r = run(tmp_path, arm, x86, "--allow-differing", "Contents/Resources/*.md")
    assert r.returncode == 0, r.stderr
    assert (tmp_path / "out/OVT Studio.app/Contents/Resources/THIRD_PARTY_NOTICES.md").read_bytes() == b"notices\n"


def test_file_in_one_bundle_only_fails(tmp_path):
    arm = make_app(tmp_path, "arm64")
    x86 = make_app(tmp_path, "x86_64", {"PlugIns/ovt/libovt_plugin_de265.dylib": macho(["x86_64"], "de265")})
    r = run(tmp_path, arm, x86)
    assert r.returncode == 1
    assert "PlugIns/ovt/libovt_plugin_de265.dylib: only in the x86_64 bundle" in r.stderr


def test_wrong_architecture_input_fails(tmp_path):
    arm = make_app(tmp_path, "arm64")
    x86 = make_app(tmp_path, "x86_64", {"MacOS/ovt": macho(["arm64"], "cli")})  # an arm64 file in the x86_64 bundle
    r = run(tmp_path, arm, x86)
    assert r.returncode == 1
    assert "not x86_64" in r.stderr


def test_existing_output_and_non_bundle_are_usage_errors(tmp_path):
    arm, x86 = make_app(tmp_path, "arm64"), make_app(tmp_path, "x86_64")
    (tmp_path / "out" / "OVT Studio.app").mkdir(parents=True)
    assert run(tmp_path, arm, x86).returncode == 2
    assert run(tmp_path, arm, tmp_path).returncode == 2


def test_tool_failure_is_exit_2(tmp_path):
    arm, x86 = make_app(tmp_path, "arm64"), make_app(tmp_path, "x86_64")
    r = subprocess.run(
        [
            sys.executable,
            "-I",
            str(SCRIPT),
            "--arm64",
            str(arm),
            "--x86_64",
            str(x86),
            "--out",
            str(tmp_path / "u.app"),
            "--lipo",
            str(tmp_path / "missing"),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert r.returncode == 2
    assert not (tmp_path / "u.app").exists()
