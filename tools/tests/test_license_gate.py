# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests of tools/license_gate.py (chapter 15 §15.4.2 acceptance tests 1-9, WI-0.602 tests 10-12)."""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import sys
import time
import tomllib
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1]
REPO = TOOLS.parent
FIX = Path(__file__).resolve().parent / "fixtures" / "license_gate"
sys.path.insert(0, str(TOOLS))

import license_gate as lg  # noqa: E402

POLICY = TOOLS / "license_policy.toml"
GATE = TOOLS / "license_gate.py"


def run(*args: str, report: Path | None = None) -> tuple[int, dict, str]:
    """Run the gate in-process; returns (exit code, report, stderr)."""
    import contextlib
    import io

    err = io.StringIO()
    extra = ["--report", str(report)] if report else []
    with contextlib.redirect_stderr(err):
        code = lg.main(["--policy", str(POLICY), *extra, *args])
    data = json.loads(report.read_text()) if report and report.exists() else {}
    return code, data, err.getvalue()


def codes(data: dict) -> list[str]:
    return [f["code"] for f in data["findings"]]


@pytest.fixture(scope="module")
def policy() -> dict:
    return lg.load_policy(POLICY)


# --- 1. SPDX expression parser ------------------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("expr", "rendered"),
    [
        ("MIT", "MIT"),
        ("MIT AND BSD-3-Clause", "MIT AND BSD-3-Clause"),
        ("MIT OR BSD-3-Clause", "MIT OR BSD-3-Clause"),
        ("mit or isc", "mit OR isc"),
        ("GPL-3.0-only WITH Bison-exception-2.2", "GPL-3.0-only WITH Bison-exception-2.2"),
        ("(MIT)", "MIT"),
        ("((MIT))", "MIT"),
        ("MIT OR (ISC AND Zlib)", "MIT OR (ISC AND Zlib)"),
        ("MIT AND ISC OR Zlib", "(MIT AND ISC) OR Zlib"),
        ("MIT OR ISC AND Zlib", "MIT OR (ISC AND Zlib)"),
        ("(MIT OR ISC) AND Zlib", "(MIT OR ISC) AND Zlib"),
        ("(MIT OR (ISC OR (Zlib AND 0BSD)))", "MIT OR (ISC OR (Zlib AND 0BSD))"),
        ("(GPL-3.0-only WITH Bison-exception-2.2) AND MIT", "GPL-3.0-only WITH Bison-exception-2.2 AND MIT"),
        ("MIT AND GPL-3.0-only WITH Bison-exception-2.2", "MIT AND GPL-3.0-only WITH Bison-exception-2.2"),
        ("LicenseRef-AOM-Patent-License-1.0", "LicenseRef-AOM-Patent-License-1.0"),
        ("  BSD-2-Clause   AND\tMIT ", "BSD-2-Clause AND MIT"),
    ],
)
def test_parse_valid(expr: str, rendered: str) -> None:
    assert lg.render(lg.parse(expr)) == rendered


@pytest.mark.parametrize(
    "expr",
    ["", "MIT AND", "AND MIT", "(MIT", "MIT)", "MIT OR OR ISC", "MIT WITH", "(MIT) WITH x", "MIT ISC", "()", "WITH"],
)
def test_parse_invalid(expr: str) -> None:
    with pytest.raises(lg.GateError):
        lg.parse(expr)


def test_resolve_picks_most_permissive_allowed(policy: dict) -> None:
    assert lg.resolve("BSD-3-Clause OR GPL-2.0-only", policy) == "BSD-3-Clause"
    assert lg.resolve("GPL-2.0-only OR GPL-3.0-only", policy) == "GPL-3.0-only"
    assert lg.resolve("Apache-2.0 OR BSD-3-Clause", policy) == "BSD-3-Clause"
    assert lg.resolve("MIT AND (ISC OR Zlib)", policy) == "MIT AND ISC"


def test_resolve_rejects_unlisted_alternatives(policy: dict) -> None:
    with pytest.raises(lg.GateError):
        lg.resolve("Foo-1.0 OR Bar-2.0", policy)
    assert lg.resolve("Foo-1.0 OR MIT", policy) == "MIT"


# --- 2. acceptance tests of §15.4.2 -------------------------------------------------------------------------------


def test_1_gpl_in_core_link_graph(tmp_path: Path) -> None:
    code, data, _ = run("--manifest", str(FIX / "license_manifest_gpl_in_core.json"), report=tmp_path / "r.json")
    assert code == 1
    assert [
        f for f in data["findings"] if f["code"] == "E-LINK" and "ovt_core" in f["where"] and "libfoo" in f["where"]
    ]


def test_manifest_ok_and_zstd_choice(tmp_path: Path) -> None:
    code, data, _ = run("--manifest", str(FIX / "license_manifest_ok.json"), report=tmp_path / "r.json")
    assert code == 0 and data["status"] == "PASS"
    zstd = next(e for e in data["inventory"] if e["component"] == "zstd")
    assert zstd["chosen"] == "BSD-3-Clause" and zstd["targets"] == ["ovt_core"]


def test_2_agpl_row_is_denied(tmp_path: Path) -> None:
    code, data, err = run("--versions", str(FIX / "versions_agpl.txt"), report=tmp_path / "r.json")
    assert code == 1
    assert [f for f in data["findings"] if f["code"] == "E-DENY" and f["license"] == "AGPL-3.0-only"]
    assert "E-DENY third_party/VERSIONS.md:MinIO AGPL-3.0-only" in err


def test_3_zstd_or_expression_passes(tmp_path: Path) -> None:
    code, data, _ = run("--versions", str(FIX / "versions_ok.txt"), report=tmp_path / "r.json")
    assert code == 0
    assert next(e for e in data["inventory"] if e["component"] == "zstd")["chosen"] == "BSD-3-Clause"


def test_4_bsl_is_not_busl(tmp_path: Path) -> None:
    code, data, _ = run("--versions", str(FIX / "versions_ok.txt"), report=tmp_path / "ok.json")
    assert code == 0 and next(e for e in data["inventory"] if e["component"] == "Catch2")["chosen"] == "BSL-1.0"
    code, data, _ = run("--versions", str(FIX / "versions_busl.txt"), report=tmp_path / "bad.json")
    assert code == 1 and "E-DENY" in codes(data)


def test_strict_or_denies_unchosen_alternative(tmp_path: Path) -> None:
    versions = tmp_path / "v.md"
    versions.write_text("| Name | Version | Licence |\n|---|---|---|\n| thing | 1 | MIT OR AGPL-3.0-only |\n")
    assert run("--versions", str(versions), report=tmp_path / "a.json")[0] == 0
    code, data, _ = run("--versions", str(versions), "--strict-or", report=tmp_path / "b.json")
    assert code == 1 and codes(data) == ["E-DENY"]


def test_unknown_licence_fails(tmp_path: Path) -> None:
    versions = tmp_path / "v.md"
    versions.write_text("| Name | Version | Licence |\n|---|---|---|\n| thing | 1 | Frobnicate-1.0 |\n")
    code, data, _ = run("--versions", str(versions), report=tmp_path / "r.json")
    assert code == 1 and codes(data) == ["E-UNKNOWN"]


def _repo(tmp_path: Path, files: dict[str, str]) -> Path:
    root = tmp_path / "repo"
    for rel, text in files.items():
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        (root / rel).write_text(text)
    return root


HDR = "// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors\n// SPDX-License-Identifier: {}\n"


def test_5_wrong_header_in_core(tmp_path: Path) -> None:
    root = _repo(
        tmp_path,
        {
            "core/a.cpp": HDR.format("GPL-3.0-or-later"),
            "core/b.cpp": "int x;\n",
            "cli/c.cpp": HDR.format("GPL-3.0-or-later"),
        },
    )
    code, data, _ = run("--sources", "--root", str(root), report=tmp_path / "r.json")
    assert code == 1
    assert sorted((f["code"], f["where"]) for f in data["findings"]) == [
        ("E-HEADER", "core/a.cpp"),
        ("E-NOHEADER", "core/b.cpp"),
    ]


def test_sources_pass(tmp_path: Path) -> None:
    root = _repo(tmp_path, {"core/a.cpp": HDR.format("LGPL-2.1-or-later")})
    assert run("--sources", "--root", str(root), report=tmp_path / "r.json")[0] == 0


def test_6_default_download_with_ffmpeg_plugin(tmp_path: Path) -> None:
    """Also acceptance test 12 of WI-0.602."""
    code, data, _ = run("--release-manifest", str(FIX / "manifest.toml"), report=tmp_path / "r.json")
    assert code == 1
    assert [(f["code"], f["where"], f["license"]) for f in data["findings"]] == [
        ("E-DIST", "bad-default", "ovt_plugin_ffmpeg")
    ]


def test_dist_missing_notices_and_codec_pack_rules(tmp_path: Path) -> None:
    text = (FIX / "manifest.toml").read_text()
    text = text.replace('ovt_plugin_ffmpeg"]\ndefault_download = true', 'ffmpeg-core"]\ndefault_download = true')
    text = text.replace('notices = true\nsign = "minisign"', 'notices = false\nsign = "minisign"')
    text += '\n[licenses]\n"good-default" = { spdx = "GPL-3.0-or-later", notices = ["THIRD_PARTY_NOTICES.md"] }\n'
    path = tmp_path / "m.toml"
    path.write_text(text)
    code, data, _ = run("--release-manifest", str(path), report=tmp_path / "r.json")
    assert code == 1
    assert [f["where"] for f in data["findings"]] == ["bad-default"]
    assert "THIRD_PARTY_NOTICES.md" in data["findings"][0]["message"]
    pack = text.replace("default_download = false\ncodec_pack = true", "default_download = true\ncodec_pack = true")
    path.write_text(pack)
    _, data, _ = run("--release-manifest", str(path), report=tmp_path / "r2.json")
    assert any("codec pack" in f["message"] for f in data["findings"])


def _policy_with_exception(tmp_path: Path, expires: str) -> Path:
    text = POLICY.read_text() + (
        f'\n[[exceptions]]\nid = "AGPL-3.0-only"\nwhere = "third_party/VERSIONS.md:MinIO"\n'
        f'reason = "test"\nexpires = "{expires}"\napproved_by = ["a", "b"]\n'
    )
    path = tmp_path / "policy.toml"
    path.write_text(text)
    return path


def _run_with_policy(policy: Path, *args: str, report: Path) -> tuple[int, dict]:
    import contextlib
    import io

    with contextlib.redirect_stderr(io.StringIO()):
        code = lg.main(["--policy", str(policy), "--report", str(report), *args])
    return code, json.loads(report.read_text())


def test_7_expired_exception(tmp_path: Path) -> None:
    policy = _policy_with_exception(tmp_path, "2026-01-01")
    code, data = _run_with_policy(
        policy, "--versions", str(FIX / "versions_agpl.txt"), "--today", "2026-11-04", report=tmp_path / "r.json"
    )
    assert code == 1 and sorted(codes(data)) == ["E-DENY", "E-EXPIRED"]


def test_live_exception_suppresses_finding(tmp_path: Path) -> None:
    policy = _policy_with_exception(tmp_path, "2027-06-30")
    code, data = _run_with_policy(
        policy, "--versions", str(FIX / "versions_agpl.txt"), "--today", "2026-11-04", report=tmp_path / "r.json"
    )
    assert code == 0 and data["findings"] == []


def test_8_real_repository(tmp_path: Path) -> None:
    """Also acceptance test 11 of WI-0.602: every input that exists, in <= 10 s."""
    args = ["--versions", str(REPO / "third_party" / "VERSIONS.md"), "--sources", "--require-all", "--root", str(REPO)]
    for flag, rel in (
        ("--manifest", "build/license_manifest.json"),
        ("--gradle", "android/app/build/reports/dependency-license/index.json"),
    ):
        if (REPO / rel).exists():
            args += [flag, str(REPO / rel)]
    start = time.monotonic()
    code, data, err = run(*args, report=tmp_path / "r.json")
    assert code == 0, err
    assert time.monotonic() - start <= 10
    assert data["inventory"]


def test_9_policy_parses_with_tomllib() -> None:
    with POLICY.open("rb") as f:
        data = tomllib.load(f)
    for table in ("deny", "allow", "targets", "directories", "distribution", "gradle_license_names"):
        assert table in data
    assert data["schema_version"] == 1


# --- 3. WI-0.602 acceptance tests 10-12 and the rest -------------------------------------------------------------


def test_10_require_all_missing_gradle(tmp_path: Path) -> None:
    code, _, err = run("--require-all", "--gradle", str(tmp_path / "nope.json"), report=tmp_path / "r.json")
    assert code == 2 and "missing required input: gradle" in err


def test_missing_input_without_require_all_is_skipped(tmp_path: Path) -> None:
    code, data, _ = run("--gradle", str(tmp_path / "nope.json"), report=tmp_path / "r.json")
    assert code == 0 and data["inputs"]["gradle"] == "skipped"


def test_usage_errors(tmp_path: Path) -> None:
    bad = tmp_path / "bad.toml"
    bad.write_text("schema_version = ")
    assert lg.main(["--policy", str(bad)]) == 2
    assert lg.main(["--policy", str(tmp_path / "missing.toml")]) == 2
    with pytest.raises(SystemExit) as e:
        lg.main([])
    assert e.value.code == 2


def test_script_runs_under_python_dash_i(tmp_path: Path) -> None:
    out = subprocess.run(
        [sys.executable, "-I", str(GATE), "--policy", str(POLICY), "--versions", str(FIX / "versions_agpl.txt")],
        capture_output=True,
        text=True,
        check=False,
        cwd=tmp_path,
    )
    assert out.returncode == 1 and "E-DENY" in out.stderr


def test_plugin_rules(tmp_path: Path) -> None:
    def manifest(own: str, lib: str) -> Path:
        path = tmp_path / f"{own}-{lib}.json"
        path.write_text(
            json.dumps({"targets": [{"name": "ovt_plugin_aom", "spdx": own, "links": [{"name": "x", "spdx": lib}]}]})
        )
        return path

    ok, bad = manifest("BSD-2-Clause", "MIT"), manifest("BSD-2-Clause", "LGPL-2.1-or-later")
    assert run("--manifest", str(ok), report=tmp_path / "a.json")[0] == 0
    code, data, _ = run("--manifest", str(bad), report=tmp_path / "b.json")
    assert code == 1 and codes(data) == ["E-LINK"]


def test_target_licence_must_match_policy(tmp_path: Path) -> None:
    path = tmp_path / "m.json"
    path.write_text(
        json.dumps(
            {
                "targets": [
                    {"name": "ovt_core", "spdx": "GPL-3.0-or-later", "links": []},
                    {"name": "ovt_mystery", "spdx": "MIT", "links": []},
                ]
            }
        )
    )
    code, data, _ = run("--manifest", str(path), report=tmp_path / "r.json")
    assert code == 1 and codes(data) == ["E-TARGET"] * 3


def test_vcpkg_spdx_overrides_and_copyright_heuristic(tmp_path: Path) -> None:
    vc = tmp_path / "vcpkg_installed"

    def port(name: str, spdx: str | None, copyright_text: str = "x") -> None:
        d = vc / "share" / name
        d.mkdir(parents=True)
        (d / "copyright").write_text(copyright_text)
        if spdx:
            (d / "vcpkg.spdx.json").write_text(
                json.dumps({"packages": [{"name": name, "versionInfo": "1", "licenseConcluded": spdx}]})
            )

    port("fmt", "MIT")
    port("mystery", "NOASSERTION")
    port("evil", "NOASSERTION", "  GNU AFFERO GENERAL PUBLIC LICENSE\n Version 3")
    code, data, _ = run("--vcpkg", str(vc), report=tmp_path / "r.json")
    assert code == 1
    assert sorted((f["code"], f["where"]) for f in data["findings"]) == [
        ("E-DENY", "vcpkg:evil"),
        ("E-UNKNOWN", "vcpkg:evil"),
        ("E-UNKNOWN", "vcpkg:mystery"),
    ]
    policy_text = POLICY.read_text().replace('# zimg = "WTFPL"', 'mystery = "ISC"')
    policy = tmp_path / "p.toml"
    policy.write_text(policy_text)
    shutil.rmtree(vc / "share" / "evil")
    code, data = _run_with_policy(policy, "--vcpkg", str(vc), report=tmp_path / "r2.json")
    assert code == 0 and {e["component"]: e["chosen"] for e in data["inventory"]} == {"fmt": "MIT", "mystery": "ISC"}


def test_gradle_and_pip_inputs(tmp_path: Path) -> None:
    gradle = tmp_path / "index.json"
    gradle.write_text(
        json.dumps(
            {
                "dependencies": [
                    {
                        "moduleName": "androidx:core",
                        "moduleVersion": "1",
                        "moduleLicense": "The Apache Software License, Version 2.0",
                    },
                    {"moduleName": "evil:lib", "moduleVersion": "1", "moduleLicense": "SSPL-1.0"},
                    {"moduleName": "odd:lib", "moduleVersion": "1", "moduleLicense": "Some Custom Terms"},
                ],
                "importedModules": [],
            }
        )
    )
    code, data, _ = run("--gradle", str(gradle), report=tmp_path / "g.json")
    assert code == 1
    assert sorted(codes(data)) == ["E-DENY", "E-UNKNOWN"]
    pip = tmp_path / "pip.json"
    pip.write_text(
        json.dumps(
            [
                {"Name": "ok", "Version": "1", "License": "MIT"},
                {"Name": "bad", "Version": "1", "License": "AGPL-3.0-only"},
            ]
        )
    )
    code, data, _ = run("--pip", str(pip), report=tmp_path / "p.json")
    assert code == 1 and codes(data) == ["E-DENY"]
    reqs = tmp_path / "requirements.txt"
    reqs.write_text("definitely-not-installed-pkg==1.0  # comment\n-r other.txt\n")
    code, data, _ = run("--pip", str(reqs), report=tmp_path / "q.json")
    assert code == 0 and [w["code"] for w in data["warnings"]] == ["W-PIP-UNRESOLVED"]


def test_report_is_deterministic_and_exports_table(tmp_path: Path) -> None:
    outs = []
    for i in range(2):
        rep, table = tmp_path / f"r{i}.json", tmp_path / f"t{i}.md"
        run(
            "--versions",
            str(REPO / "third_party" / "VERSIONS.md"),
            "--manifest",
            str(FIX / "license_manifest_ok.json"),
            "--export-table",
            str(table),
            report=rep,
        )
        outs.append((rep.read_bytes(), table.read_bytes()))
    assert outs[0] == outs[1]
    assert lg.inventory(json.loads(outs[0][0]))
    assert re.search(rb"\| zstd \| 1\.5\.6 \| vcpkg \|", outs[0][1])


def test_default_report_next_to_manifest(tmp_path: Path) -> None:
    shutil.copy(FIX / "license_manifest_ok.json", tmp_path / "license_manifest.json")
    assert run("--manifest", str(tmp_path / "license_manifest.json"))[0] == 0
    assert (tmp_path / "license_gate_report.json").is_file()


def test_vcpkg_gpl3_text_is_not_denied_but_agpl_title_is(tmp_path: Path) -> None:
    vc = tmp_path / "vcpkg_installed"
    gpl = "                    GNU GENERAL PUBLIC LICENSE\n13. Use with the GNU Affero General Public License.\n"
    agpl = "                    GNU AFFERO GENERAL PUBLIC LICENSE\n                       Version 3\n"
    for name, text in (("gplport", gpl), ("agplport", agpl)):
        d = vc / "share" / name
        d.mkdir(parents=True)
        (d / "copyright").write_text(text)
        (d / "vcpkg.spdx.json").write_text(json.dumps({"packages": [{"licenseConcluded": "NOASSERTION"}]}))
    (vc / "share" / "gplport" / "vcpkg.spdx.json").write_text(json.dumps({"packages": [{"licenseConcluded": "MIT"}]}))
    code, data, _ = run("--vcpkg", str(vc), report=tmp_path / "r.json")
    assert code == 1
    assert sorted((f["code"], f["where"]) for f in data["findings"]) == [
        ("E-DENY", "vcpkg:agplport"),
        ("E-UNKNOWN", "vcpkg:agplport"),
    ]
    # an SPDX licence is never second-guessed by the heuristics, even for the AGPL title
    (vc / "share" / "agplport" / "vcpkg.spdx.json").write_text(json.dumps({"packages": [{"licenseConcluded": "MIT"}]}))
    assert run("--vcpkg", str(vc), report=tmp_path / "r2.json")[0] == 0


def test_vcpkg_null_licence_falls_back_to_the_override(tmp_path: Path) -> None:
    # vcpkg writes LicenseRef-vcpkg-null for `"license": null`; it is not a licence, so [vcpkg_overrides] applies.
    vc = tmp_path / "vcpkg_installed"
    for name in ("glslang", "nulled"):
        d = vc / "share" / name
        d.mkdir(parents=True)
        (d / "copyright").write_text("BSD-3-Clause text\n")
        (d / "vcpkg.spdx.json").write_text(
            json.dumps({"packages": [{"licenseConcluded": "LicenseRef-vcpkg-null", "licenseDeclared": "NOASSERTION"}]})
        )
    code, data, _ = run("--vcpkg", str(vc), report=tmp_path / "r.json")
    assert code == 1
    assert [(f["code"], f["where"]) for f in data["findings"]] == [("E-UNKNOWN", "vcpkg:nulled")]


def test_versions_override_wins_over_parseable_cell(tmp_path: Path) -> None:
    versions = tmp_path / "v.md"
    versions.write_text(
        "| Name | Version | Licence |\n|---|---|---|\n| FFmpeg | 1 | MIT (AGPL-3.0-only with a flag) |\n"
    )
    code, data, _ = run("--versions", str(versions), report=tmp_path / "r.json")
    assert code == 0 and data["inventory"][0]["license"] == "LGPL-2.1-or-later OR GPL-2.0-or-later"


# --- 7. corpora mode (WI-0.561; chapter 14 §14.3.4) ----------------------------------------------------------------


def _corpus(**over: object) -> dict:
    entry = {
        "id": "syn_ok",
        "source": "syn",
        "licence": "CC0 test fixture",
        "licence_spdx": "CC0-1.0",
        "redistributable": True,
    }
    return {**entry, **over}


def _corpora(tmp_path: Path, entries: list[dict], *, write: bool = True) -> Path:
    manifest = tmp_path / "manifest.json"
    manifest.write_text(json.dumps({"schema_version": 1, "entries": entries}))
    if write:
        assert run("--corpora", "--manifest", str(manifest), "--write-licenses-md")[0] in (0, 1)
    return manifest


def test_corpora_1_agpl_entry_names_the_id(tmp_path: Path) -> None:
    manifest = _corpora(tmp_path, [_corpus(id="bad_agpl", licence_spdx="AGPL-3.0-only")])
    code, _, err = run("--corpora", "--manifest", str(manifest))
    assert code == 1
    assert "E-DENY corpora:bad_agpl AGPL-3.0-only" in err


def test_corpora_2_itu_conformance_cannot_be_redistributable(tmp_path: Path) -> None:
    manifest = _corpora(tmp_path, [_corpus(id="bad_itu", licence_spdx="LicenseRef-ITU-Conformance")])
    code, _, err = run("--corpora", "--manifest", str(manifest))
    assert code == 1
    assert "E-CORPUS corpora:bad_itu LicenseRef-ITU-Conformance" in err
    # The same licence with redistributable=false is the normal case for conformance streams.
    manifest = _corpora(
        tmp_path, [_corpus(id="ok_itu", licence_spdx="LicenseRef-ITU-Conformance", redistributable=False)]
    )
    assert run("--corpora", "--manifest", str(manifest))[0] == 0


def test_corpora_3_committed_manifest_is_clean_and_licenses_md_unchanged() -> None:
    manifest = REPO / "tests" / "corpora" / "manifest.json"
    before = (manifest.parent / "LICENSES.md").read_bytes()
    code, _, err = run("--corpora", "--manifest", str(manifest))
    assert code == 0, err
    assert (manifest.parent / "LICENSES.md").read_bytes() == before
    assert lg.licenses_md(lg.load_corpora(manifest)).encode() == before


def test_corpora_non_commercial_and_other_denied_ids(tmp_path: Path) -> None:
    for spdx in ("CC-BY-NC-4.0", "SSPL-1.0", "BUSL-1.1", "LicenseRef-Foo-NonCommercial", "LicenseRef-Foo-NC"):
        manifest = _corpora(tmp_path, [_corpus(licence_spdx=spdx, redistributable=False)])
        code, _, err = run("--corpora", "--manifest", str(manifest))
        assert code == 1 and f"E-DENY corpora:syn_ok {spdx}" in err, spdx


def test_corpora_stale_or_missing_licenses_md(tmp_path: Path) -> None:
    manifest = _corpora(tmp_path, [_corpus()])
    assert run("--corpora", "--manifest", str(manifest))[0] == 0
    (tmp_path / "LICENSES.md").write_text("old\n")
    code, _, err = run("--corpora", "--manifest", str(manifest))
    assert code == 1 and "E-STALE" in err
    (tmp_path / "LICENSES.md").unlink()
    assert run("--corpora", "--manifest", str(manifest))[0] == 1


def test_corpora_generator_is_deterministic_and_sorted(tmp_path: Path) -> None:
    entries = [_corpus(id="b", licence="x | y"), _corpus(id="a")]
    text = lg.licenses_md(entries)
    assert text == lg.licenses_md(list(reversed(entries)))
    assert text.index("| a |") < text.index("| b |")
    assert "x \\| y" in text


@pytest.mark.parametrize(
    "content",
    [
        "not json",
        '{"entries": 3}',
        '{"entries": [{"id": "x", "licence": "l", "licence_spdx": "MIT"}]}',
        '{"entries": [{"id": "x", "licence": "l", "licence_spdx": "MIT", "redistributable": "yes"}]}',
    ],
)
def test_corpora_invalid_manifest_exits_4(tmp_path: Path, content: str) -> None:
    manifest = tmp_path / "manifest.json"
    manifest.write_text(content)
    code, _, err = run("--corpora", "--manifest", str(manifest))
    assert code == 4 and "license_gate:" in err


def test_corpora_missing_manifest_exits_4(tmp_path: Path) -> None:
    assert run("--corpora", "--manifest", str(tmp_path / "nope.json"))[0] == 4


def test_write_licenses_md_needs_corpora() -> None:
    assert run("--write-licenses-md")[0] == 2


DENIED_REFS = [
    "LicenseRef-Foo-NC",
    "LicenseRef-NC-1.0",
    "LicenseRef-Acme-NC4",
    "LicenseRef-Foo-nc",
    "LicenseRef-AcmeProprietary",
    "LicenseRef-Proprietary2",
    "LicenseRef-Acme-EvaluationOnly",
    "LicenseRef-Foo-NC AND MIT",
    "(LicenseRef-Foo-NC OR MIT)",
    "LicenseRef-Foo-NC WITH x",
]
PASSING_REFS = [
    "LicenseRef-ITU-Conformance",
    "LicenseRef-MPEG-Conformance",
    "LicenseRef-AOM-Patent-License-1.0",
    "LicenseRef-GPAC-Testsuite-Media",
    "LicenseRef-Bento4-Test-Media",
    "LicenseRef-Foo-NCSA",
]


@pytest.mark.parametrize("ref", DENIED_REFS)
def test_deny_patterns_catch_licenseref_variants(ref: str, policy: dict) -> None:
    assert lg.is_denied(ref, policy), ref


@pytest.mark.parametrize("ref", PASSING_REFS)
def test_deny_patterns_pass_ordinary_licenseref(ref: str, policy: dict) -> None:
    assert not lg.is_denied(ref, policy), ref


def test_corpora_denies_compound_expression(tmp_path: Path) -> None:
    manifest = _corpora(
        tmp_path, [_corpus(id="bad_nc", licence_spdx="LicenseRef-Foo-NC AND MIT", redistributable=False)]
    )
    code, _, err = run("--corpora", "--manifest", str(manifest))
    assert code == 1 and "E-DENY corpora:bad_nc" in err


# --- 3. CI wiring (chapter 13 WI-0.032) ----------------------------------------------------------------------------


def test_ci_bad_versions_fixture_names_the_row(tmp_path: Path) -> None:
    # WI-0.032 acceptance test 1: the fixture the license-gate job feeds to the gate fails and names its row.
    code, data, err = run("--versions", str(FIX.parent / "VERSIONS_bad.md"), report=tmp_path / "r.json")
    assert code == 1
    assert [f for f in data["findings"] if f["code"] == "E-DENY" and "fake-agpl-service" in f["where"]]
    assert "E-DENY third_party/VERSIONS.md:fake-agpl-service AGPL-3.0-only" in err


def test_ci_vcpkg_licence_missing_from_policy_is_unknown(tmp_path: Path) -> None:
    # WI-0.032 acceptance test 2: a vcpkg port whose licence is in no policy table fails with E-UNKNOWN.
    vc = tmp_path / "vcpkg_installed"
    d = vc / "share" / "mysteryport"
    d.mkdir(parents=True)
    (d / "vcpkg.spdx.json").write_text(json.dumps({"packages": [{"licenseConcluded": "Frobnicate-1.0"}]}))
    code, data, err = run("--vcpkg", str(vc), "--strict-or", report=tmp_path / "r.json")
    assert code == 1
    assert [(f["code"], f["where"]) for f in data["findings"]] == [("E-UNKNOWN", "vcpkg:mysteryport")]
    assert "E-UNKNOWN vcpkg:mysteryport" in err


def test_ci_workflow_wires_every_gate_input() -> None:
    text = (REPO / ".github" / "workflows" / "ci.yml").read_text()
    jobs = {m.group(1): m.start() for m in re.finditer(r"^  ([a-z0-9-]+):\n", text, re.M)}
    assert "license-gate" in jobs and "build-linux-x64" in jobs
    gate_job = text[jobs["license-gate"] : jobs["build-linux-x64"]]
    for needle in ("--corpora", "--versions third_party/VERSIONS.md", "--sources", "--strict-or", "--require-all"):
        assert needle in gate_job, needle
    assert "VERSIONS_bad.md" in gate_job and "continue-on-error" not in gate_job
    assert "ovt_license_manifest" in text
    assert "--manifest build/linux-x64-release/license_manifest.json" in text


def test_manifest_library_file_takes_the_licence_of_its_vcpkg_port(tmp_path: Path) -> None:
    # tp::placebo links vcpkg's libglslang.a by absolute path; the manifest has no licence for it (NOASSERTION), the
    # port's vcpkg.spdx.json says null, so the reviewed [vcpkg_overrides] entry applies. WI-0.032.
    installed = tmp_path / "vcpkg_installed"
    root = installed / "x64-linux-ovt"
    (root / "share" / "glslang").mkdir(parents=True)
    (root / "share" / "glslang" / "vcpkg.spdx.json").write_text(
        json.dumps({"packages": [{"versionInfo": "16", "licenseConcluded": "LicenseRef-vcpkg-null"}]})
    )
    (installed / "vcpkg" / "info").mkdir(parents=True)
    (installed / "vcpkg" / "info" / "glslang_16.4.0_x64-linux-ovt.list").write_text("x64-linux-ovt/lib/libglslang.a\n")
    lib = str(root / "lib" / "libglslang.a")

    def manifest(path_name: str) -> Path:
        m = tmp_path / "m.json"
        m.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "preset": "x",
                    "options": {},
                    "targets": [
                        {
                            "name": "ovt_render",
                            "type": "STATIC",
                            "spdx": "LGPL-2.1-or-later",
                            "directory": "render/",
                            "links": [
                                {
                                    "name": path_name,
                                    "source": "system",
                                    "version": "",
                                    "spdx": "NOASSERTION",
                                    "linkage": "static",
                                    "patched": False,
                                }
                            ],
                        }
                    ],
                }
            )
        )
        return m

    code, data, _ = run("--manifest", str(manifest(lib)), "--vcpkg", str(root), report=tmp_path / "ok.json")
    assert code == 0, data
    # a file that no port installed stays unknown
    code, data, _ = run("--manifest", str(manifest(lib + ".x")), "--vcpkg", str(root), report=tmp_path / "bad.json")
    assert code == 1 and codes(data) == ["E-UNKNOWN"]
    # without --vcpkg nothing resolves it
    assert run("--manifest", str(manifest(lib)), report=tmp_path / "novcpkg.json")[0] == 1
