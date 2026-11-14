# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/mac/fix_rpaths.py and the shell scripts in tools/mac/ (WI-0.060).

`otool` and `install_name_tool` exist only on macOS, so the tests run the script against fakes that read and update
a JSON database of {file: {"deps": [...], "rpaths": [...]}}. The fakes print what `otool -L` and `otool -l` print.
The real tools are exercised by the manual record docs/acceptance/wi/WI-0.060.md on the Mac Mini.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

MAC = Path(__file__).resolve().parents[1] / "mac"
SCRIPT = MAC / "fix_rpaths.py"

FAKE_OTOOL = """#!/usr/bin/env python3
import json, os, sys
db = json.load(open(os.environ["FAKE_DB"]))
flag, path = sys.argv[1], sys.argv[2]
e = db[path]
if flag == "-L":
    print(path + ":")
    for d in e["deps"]:
        print("\\t" + d + " (compatibility version 1.0.0, current version 1.0.0)")
else:
    for r in e["rpaths"]:
        print("Load command 9\\n          cmd LC_RPATH\\n      cmdsize 32\\n         path " + r + " (offset 12)")
"""

FAKE_INT = """#!/usr/bin/env python3
import json, os, sys
dbp = os.environ["FAKE_DB"]
db = json.load(open(dbp))
a = sys.argv[1:]
e = db[a[-1]]
# Like the real tool: the same -delete_rpath twice in one call is an error, and one -delete_rpath removes one
# LC_RPATH (the first), or every copy with FAKE_DELETE_ALL=1.
deleted = [a[i + 1] for i in range(len(a) - 1) if a[i] == "-delete_rpath"]
if len(set(deleted)) != len(deleted):
    sys.exit("install_name_tool: more than one -delete_rpath option specified")
i = 0
while i < len(a) - 1:
    if a[i] == "-id":
        e["deps"][0] = a[i + 1]; i += 2
    elif a[i] == "-change":
        e["deps"] = [a[i + 2] if d == a[i + 1] else d for d in e["deps"]]; i += 3
    elif a[i] == "-add_rpath":
        e["rpaths"].append(a[i + 1]); i += 2
    elif a[i] == "-delete_rpath":
        if a[i + 1] not in e["rpaths"]:
            sys.exit("install_name_tool: no LC_RPATH load command with path: " + a[i + 1])
        if os.environ.get("FAKE_DELETE_ALL") == "1":
            e["rpaths"] = [r for r in e["rpaths"] if r != a[i + 1]]
        else:
            e["rpaths"].remove(a[i + 1])
        i += 2
    else:
        sys.exit("bad arg " + a[i])
json.dump(db, open(dbp, "w"))
"""

MACHO = b"\xcf\xfa\xed\xfe" + b"\0" * 12


def make_bundle(tmp: Path, entries: dict[str, dict]) -> tuple[Path, Path]:
    app = tmp / "OVT Studio.app"
    db = {}
    for rel, e in entries.items():
        p = app / "Contents" / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(MACHO)
        db[str(p)] = e
    dbp = tmp / "db.json"
    dbp.write_text(json.dumps(db))
    for name, body in (("otool", FAKE_OTOOL), ("install_name_tool", FAKE_INT)):
        t = tmp / name
        t.write_text(body.replace("#!/usr/bin/env python3", f"#!{sys.executable}"))
        t.chmod(0o755)
    return app, dbp


def run(tmp: Path, dbp: Path, app: Path, *extra: str, env: dict | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        [
            sys.executable,
            "-I",
            str(SCRIPT),
            str(app),
            "--otool",
            str(tmp / "otool"),
            "--install-name-tool",
            str(tmp / "install_name_tool"),
            *extra,
        ],
        capture_output=True,
        text=True,
        env={"FAKE_DB": str(dbp), **(env or {})},
        check=False,
    )


def clean_entries() -> dict[str, dict]:
    return {
        "MacOS/OVT Studio": {
            "deps": ["@rpath/libovt_core.1.dylib", "/usr/lib/libc++.1.dylib"],
            "rpaths": ["@executable_path/../Frameworks"],
        },
        "Frameworks/libovt_core.1.dylib": {
            "deps": ["@rpath/libovt_core.1.dylib", "/usr/lib/libSystem.B.dylib"],
            "rpaths": ["@loader_path"],
        },
        "PlugIns/ovt/libovt_plugin_aom.dylib": {
            "deps": ["@rpath/libovt_plugin_aom.dylib", "@rpath/libovt_core.1.dylib"],
            "rpaths": ["@loader_path/../../Frameworks"],
        },
    }


def test_clean_bundle_passes(tmp_path):
    app, dbp = make_bundle(tmp_path, clean_entries())
    r = run(tmp_path, dbp, app)
    assert r.returncode == 0, r.stderr
    assert "3 Mach-O files checked, 0 problem(s)" in r.stdout


def test_absolute_build_tree_reference_fails_and_names_the_file(tmp_path):
    e = clean_entries()
    e["PlugIns/ovt/libovt_plugin_aom.dylib"]["deps"].append("/Users/x/build/lib/libaom.3.dylib")
    app, dbp = make_bundle(tmp_path, e)
    r = run(tmp_path, dbp, app)
    assert r.returncode == 1
    assert "PlugIns/ovt/libovt_plugin_aom.dylib: forbidden reference /Users/x/build/lib/libaom.3.dylib" in r.stderr


def test_homebrew_reference_fails(tmp_path):
    e = clean_entries()
    e["MacOS/OVT Studio"]["deps"].append("/opt/homebrew/opt/dav1d/lib/libdav1d.7.dylib")
    app, dbp = make_bundle(tmp_path, e)
    assert run(tmp_path, dbp, app).returncode == 1


def test_unresolvable_rpath_reference_fails(tmp_path):
    e = clean_entries()
    e["MacOS/OVT Studio"]["deps"].append("@rpath/libMoltenVK.dylib")  # nothing in Frameworks
    app, dbp = make_bundle(tmp_path, e)
    r = run(tmp_path, dbp, app)
    assert r.returncode == 1
    assert "@rpath/libMoltenVK.dylib does not resolve" in r.stderr


def test_rpath_outside_the_bundle_fails(tmp_path):
    e = clean_entries()
    e["MacOS/OVT Studio"]["rpaths"].append("/Users/x/build/lib")
    app, dbp = make_bundle(tmp_path, e)
    r = run(tmp_path, dbp, app)
    assert r.returncode == 1
    assert "LC_RPATH outside the bundle: /Users/x/build/lib" in r.stderr


def test_fix_repairs_plugin_references_and_rpaths(tmp_path):
    e = clean_entries()
    e["Frameworks/libaom.3.dylib"] = {"deps": ["@rpath/libaom.3.dylib"], "rpaths": []}
    e["Frameworks/QtCore.framework/Versions/A/QtCore"] = {
        "deps": ["@rpath/QtCore.framework/Versions/A/QtCore"],
        "rpaths": [],
    }
    plug = e["PlugIns/ovt/libovt_plugin_aom.dylib"]
    plug["deps"] = [
        "/Users/x/build/lib/libovt_plugin_aom.dylib",
        "/Users/x/build/aom/lib/libaom.3.dylib",
        "/Users/qt/lib/QtCore.framework/Versions/A/QtCore",
        "/usr/lib/libc++.1.dylib",
    ]
    plug["rpaths"] = ["/Users/x/build/lib"]
    app, dbp = make_bundle(tmp_path, e)
    assert run(tmp_path, dbp, app).returncode == 1
    r = run(tmp_path, dbp, app, "--fix")
    assert r.returncode == 0, r.stderr
    db = json.loads(dbp.read_text())
    fixed = db[str(app / "Contents/PlugIns/ovt/libovt_plugin_aom.dylib")]
    assert fixed["deps"] == [
        "@rpath/libovt_plugin_aom.dylib",
        "@rpath/libaom.3.dylib",
        "@rpath/QtCore.framework/Versions/A/QtCore",
        "/usr/lib/libc++.1.dylib",
    ]
    assert fixed["rpaths"] == ["@loader_path/../../Frameworks"]
    qt = db[str(app / "Contents/Frameworks/QtCore.framework/Versions/A/QtCore")]
    assert qt["rpaths"] == ["@loader_path/../../.."]


@pytest.mark.parametrize("delete_all", ["0", "1"])
def test_fix_removes_duplicate_rpaths(tmp_path, delete_all):
    """QtCore carries one LC_RPATH twice (found on the Mac Mini for WI-0.063): each path is named once per call."""
    e = clean_entries()
    e["Frameworks/QtCore.framework/Versions/A/QtCore"] = {
        "deps": ["@rpath/QtCore.framework/Versions/A/QtCore"],
        "rpaths": ["@loader_path/../../../", "@loader_path/../../../", "/Users/qt/lib", "@loader_path/../../.."],
    }
    e["MacOS/OVT Studio"]["rpaths"] = ["@executable_path/../Frameworks"] * 3
    app, dbp = make_bundle(tmp_path, e)
    r = run(tmp_path, dbp, app)
    assert r.returncode == 1
    assert "duplicate LC_RPATH @executable_path/../Frameworks" in r.stderr
    r = run(tmp_path, dbp, app, "--fix", env={"FAKE_DELETE_ALL": delete_all})
    assert r.returncode == 0, r.stderr
    db = json.loads(dbp.read_text())
    assert db[str(app / "Contents/Frameworks/QtCore.framework/Versions/A/QtCore")]["rpaths"] == [
        "@loader_path/../../.."
    ]
    assert db[str(app / "Contents/MacOS/OVT Studio")]["rpaths"] == ["@loader_path/../Frameworks"]


def test_fix_cannot_invent_a_missing_library(tmp_path):
    e = clean_entries()
    e["MacOS/ovt"] = {"deps": ["/opt/homebrew/lib/libnothere.dylib"], "rpaths": []}
    app, dbp = make_bundle(tmp_path, e)
    r = run(tmp_path, dbp, app, "--fix")
    assert r.returncode == 1
    assert "forbidden reference /opt/homebrew/lib/libnothere.dylib" in r.stderr


def test_not_a_bundle_is_a_usage_error(tmp_path):
    r = subprocess.run([sys.executable, "-I", str(SCRIPT), str(tmp_path)], capture_output=True, text=True, check=False)
    assert r.returncode == 2


def test_tool_failure_is_exit_2(tmp_path):
    app, _ = make_bundle(tmp_path, clean_entries())
    r = subprocess.run(
        [sys.executable, "-I", str(SCRIPT), str(app), "--otool", str(tmp_path / "missing")],
        capture_output=True,
        text=True,
        check=False,
    )
    assert r.returncode == 2


@pytest.mark.parametrize("script", ["bundle.sh", "strip.sh"])
def test_shell_scripts_parse_under_bash(script):
    assert subprocess.run(["bash", "-n", str(MAC / script)], check=False).returncode == 0


def test_fix_is_idempotent(tmp_path):
    e = clean_entries()
    e["PlugIns/ovt/libovt_plugin_aom.dylib"]["rpaths"] = ["/Users/x/build/lib"]
    app, dbp = make_bundle(tmp_path, e)
    assert run(tmp_path, dbp, app, "--fix").returncode == 0
    first = dbp.read_text()
    assert run(tmp_path, dbp, app, "--fix").returncode == 0
    assert dbp.read_text() == first


def test_bundle_sh_rejects_bad_arguments():
    assert subprocess.run([str(MAC / "bundle.sh")], capture_output=True, check=False).returncode == 2
    r = subprocess.run([str(MAC / "bundle.sh"), "linux-x64-release"], capture_output=True, text=True, check=False)
    assert r.returncode == 1
    assert "not a macos-arm64-* or macos-x86_64-* preset" in r.stderr


def test_info_plist_lists_the_spec_document_types():
    import plistlib
    import re

    text = (Path(__file__).resolve().parents[2] / "app/resources/mac/Info.plist.in").read_text()
    plist = plistlib.loads(re.sub(r"\$\{[A-Z_]+\}", "x", text).encode())
    exts = {e for t in plist["CFBundleDocumentTypes"] for e in t["CFBundleTypeExtensions"]}
    assert exts == {"mp4", "m4v", "mov", "264", "h264", "265", "hevc", "ivf", "obu", "webm", "mkv", "y4m", "yuv"}
    assert plist["LSMinimumSystemVersion"] == "13.0"
    assert plist["NSHighResolutionCapable"] is True
