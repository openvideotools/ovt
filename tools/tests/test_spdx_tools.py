# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
# REUSE-IgnoreStart (this file quotes SPDX header lines; only the two above are its own)
"""Tests of tools/spdx_header.py and tools/check_spdx_dirs.py (chapter 15 WI-0.601)."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1]
REPO = TOOLS.parent
sys.path.insert(0, str(TOOLS))

import check_spdx_dirs  # noqa: E402
import spdx_header  # noqa: E402

POLICY = REPO / "tools" / "license_policy.toml"


def header(licence: str, comment: str = "//") -> str:
    return (
        f"{comment} SPDX-FileCopyrightText: 2026 The Open Video Tools contributors\n"
        f"{comment} SPDX-License-Identifier: {licence}\n"
    )


def write(root: Path, rel: str, text: str) -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


def run_header(root: Path, *args: str) -> int:
    return spdx_header.main([*args, "--policy", str(POLICY), "--root", str(root), "--year", "2026"])


# ---- policy lookup ------------------------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("path", "licence"),
    [
        ("core/src/x.cpp", "LGPL-2.1-or-later"),
        ("core/include/ovt/abi/decoder.h", "BSD-2-Clause"),
        ("core/include/ovt/abic.h", "LGPL-2.1-or-later"),  # prefix match is per path component
        ("core/schema/ovt_model.fbs", "BSD-2-Clause"),
        ("cli/src/main.cpp", "GPL-3.0-or-later"),
        ("plugins/ovt-plugin-de265/src/plugin.cpp", "LGPL-3.0-or-later"),
        ("plugins/CMakeLists.txt", "LGPL-2.1-or-later"),
        (".github/workflows/ci.yml", "LGPL-2.1-or-later"),
        ("CMakeLists.txt", "LGPL-2.1-or-later"),
    ],
)
def test_longest_prefix(path: str, licence: str) -> None:
    assert spdx_header.expected_licence(path, spdx_header.load_directories(POLICY)) == licence


# ---- spdx_header.py (acceptance test 3) ------------------------------------------------------------------------


def test_shebang_python_gets_the_header_on_line_2(tmp_path: Path) -> None:
    script = write(tmp_path, "tools/run.py", "#!/usr/bin/env python3\nprint('hi')\n")
    assert run_header(tmp_path, str(script)) == 1
    lines = script.read_text().splitlines()
    assert lines[0] == "#!/usr/bin/env python3"
    assert lines[1] == "# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors"
    assert lines[2] == "# SPDX-License-Identifier: LGPL-2.1-or-later"
    before = script.read_text()
    assert run_header(tmp_path, str(script)) == 0
    assert run_header(tmp_path, str(script), "--check") == 0
    assert script.read_text() == before


def test_pragma_once_header_stays_below_the_spdx_lines(tmp_path: Path) -> None:
    h = write(tmp_path, "core/include/ovt/abi/x.h", "#pragma once\nint ovt_x(void);\n")
    assert run_header(tmp_path, str(h)) == 1
    text = h.read_text()
    assert text.startswith(header("BSD-2-Clause"))
    assert "\n#pragma once\n" in text


def test_check_mode_changes_nothing(tmp_path: Path) -> None:
    f = write(tmp_path, "cli/src/a.cpp", "int a;\n")
    assert run_header(tmp_path, str(f), "--check") == 1
    assert f.read_text() == "int a;\n"


def test_wrong_licence_is_reported_not_rewritten(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    f = write(tmp_path, "core/src/x.cpp", header("GPL-3.0-or-later") + "int x;\n")
    assert run_header(tmp_path, str(f)) == 1
    assert "E-HEADER core/src/x.cpp expected LGPL-2.1-or-later got GPL-3.0-or-later" in capsys.readouterr().err
    assert "GPL-3.0-or-later" in f.read_text()


def test_unknown_type_exits_2(tmp_path: Path) -> None:
    f = write(tmp_path, "core/src/data.bin", "x")
    assert run_header(tmp_path, str(f)) == 2


def test_cmake_and_yaml_use_hash_comments(tmp_path: Path) -> None:
    c = write(tmp_path, "cmake/Foo.cmake", "set(X 1)\n")
    y = write(tmp_path, ".github/workflows/x.yml", "name: x\n")
    assert run_header(tmp_path, str(c), str(y)) == 1
    assert c.read_text().startswith(header("LGPL-2.1-or-later", "#"))
    assert y.read_text().startswith(header("LGPL-2.1-or-later", "#"))


# ---- check_spdx_dirs.py (acceptance tests 1 and 2) ---------------------------------------------------------------


def test_gpl_header_in_core_fails(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    write(tmp_path, "core/src/x.cpp", header("GPL-3.0-or-later") + "int x;\n")
    write(tmp_path, "core/src/ok.cpp", header("LGPL-2.1-or-later") + "int ok;\n")
    write(tmp_path, "core/src/none.cpp", "int none;\n")
    write(tmp_path, "core/src/x_generated.h", "// generated\n")
    rc = check_spdx_dirs.main(
        [
            "--policy",
            str(POLICY),
            "--root",
            str(tmp_path),
            str(tmp_path / "core/src/x.cpp"),
            str(tmp_path / "core/src/ok.cpp"),
            str(tmp_path / "core/src/none.cpp"),
            str(tmp_path / "core/src/x_generated.h"),
        ]
    )
    err = capsys.readouterr().err
    assert rc == 1
    assert "E-HEADER core/src/x.cpp expected LGPL-2.1-or-later got GPL-3.0-or-later" in err
    assert "E-NOHEADER core/src/none.cpp" in err
    assert "ok.cpp" not in err and "generated" not in err


def test_derived_file_with_two_copyright_lines_passes(tmp_path: Path) -> None:
    write(
        tmp_path,
        "app/src/derived.cpp",
        "// SPDX-FileCopyrightText: 2015-2024 Institut für Nachrichtentechnik, RWTH Aachen University (YUView)\n"
        + header("GPL-3.0-or-later"),
    )
    assert (
        check_spdx_dirs.main(["--policy", str(POLICY), "--root", str(tmp_path), str(tmp_path / "app/src/derived.cpp")])
        == 0
    )


def test_repository_passes() -> None:
    # Acceptance test 1: the whole tree, as tracked by git.
    if subprocess.run(["git", "rev-parse"], cwd=REPO, capture_output=True).returncode != 0:
        pytest.skip("not a git checkout")
    assert check_spdx_dirs.main(["--policy", str(POLICY), "--root", str(REPO)]) == 0


# REUSE-IgnoreEnd
