#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Third-party notices generator (chapter 15 §15.6; WI-0.603).

    gen_notices.py --report build/<preset>/license_gate_report.json --versions third_party/VERSIONS.md
        --licenses-dir <prefix>/share/ovt/licenses [--licenses-dir ...] [--qt-licenses <QT_ROOT_DIR>/Licenses]
        [--vcpkg-share <prefix>/share] [--derived tools/derived_files.toml] --artifact <id>
        --out THIRD_PARTY_NOTICES.md [--patent-notice] [--version <v>] [--root <repo>]

Writes the notices file of one artefact: summary table, required statements, full licence texts, files
derived from other projects. With `--patent-notice` (the codec pack) it also writes `PATENT_NOTICE.md`
next to `--out`. The output is a pure function of the inputs: sorted, no timestamps, `\\n` line ends, so two
builds of one tag are byte-identical.

Licence texts are looked up, in this order, and never fetched from the network:
  1. `<licenses-dir>/<key>/*`            (ovt_tp_declare LICENSE_FILES, installed per dependency; also
                                          `<licenses-dir>/<key>/share/ovt/licenses/<key>/*`)
  2. `<vcpkg-share>/<key>/copyright`     (vcpkg ports)
  3. `<qt-licenses>/*GPL*`               (component `Qt` only)
`<key>` is `versions_to_cmake.dep_id(name)` (`ONNX Runtime` -> `onnx_runtime`). A component with no text is a build failure: `E-NOTICE-MISSING <dep>`, exit 1.
Exit 2 is a usage error. Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # `python -I` drops the script directory

import license_gate as lg  # noqa: E402
from versions_to_cmake import dep_id  # noqa: E402

# SPEC-ASSUMPTION(#57): the artefact component set comes from tools/release/manifest.toml (chapter 13 WI-0.036,
# M0.5), which does not exist yet. Until it does, the components listed are those some build target links: gate
# inventory rows with a non-empty `targets` (license_manifest.json `targets[].links`, §15.6). WI-0.036 narrows
# that set per artefact.

# §15.5.3, verbatim.
AV1_SENTENCE = (
    "This software includes libaom and/or dav1d, implementations of the AV1 video codec. AV1 is published by the "
    "Alliance for Open Media under the Alliance for Open Media Patent License 1.0, a copy of which is included as "
    "`LICENSES/LicenseRef-AOM-Patent-License-1.0.txt`; that licence terminates for any party that initiates patent "
    "litigation alleging that an AV1 implementation infringes. libaom is additionally licensed under the "
    "BSD-2-Clause licence and dav1d under the BSD-2-Clause licence; their notices are reproduced below."
)
FFMPEG_SENTENCE = (
    "This software uses libraries from the FFmpeg project under the LGPLv2.1. FFmpeg is a trademark of Fabrice "
    "Bellard, originator of the FFmpeg project. The exact source code and configuration used are included in the "
    "Corresponding Source archive published with this release."
)
PATENT_NOTICE = (
    "This codec pack contains software decoders for H.264/AVC and H.265/HEVC (FFmpeg's `h264` and `hevc` decoders, "
    "libde265, and optionally the HM reference decoder). These video coding standards are covered by patents held "
    "by many parties and administered in part by patent pools (Via Licensing Alliance, Access Advance) and by other "
    "licensors (Avanci Video and independent patent holders). The Open Video Tools project does not hold or grant "
    "any patent licence. Installing and using this codec pack may require a patent licence in your jurisdiction, in "
    "particular for commercial use or redistribution. You are solely responsible for determining whether you need "
    "one. The Open Video Tools application works without this pack for AV1 and for bitstream, container and syntax "
    "inspection of all supported codecs."
)


class UsageError(Exception):
    pass


# CMake target or library names of the manifest -> the port / dependency they come from, where stripping the
# namespace and `lib` prefix is not enough.
ALIASES = {
    "hwy": "highway",
    "libzstd_static": "zstd",
    "libzstd_shared": "zstd",
    # The prebuilt ONNX Runtime has one VERSIONS.md row per archive (third_party/onnxruntime.cmake) but is one component,
    # named after its CMake target tp::onnxruntime.
    "onnx_runtime": "onnxruntime",
    "onnx_runtime_linux_aarch64": "onnxruntime",
    "onnx_runtime_osx_arm64": "onnxruntime",
    "onnx_runtime_osx_x86_64": "onnxruntime",
}


def key_of(name: str) -> str:
    """The <dep> id of a component (`versions_to_cmake.dep_id`), also the staging directory name.

    Rows of third_party/VERSIONS.md give `ONNX Runtime` -> `onnx_runtime`. Names from license_manifest.json are CMake
    targets or libraries: `fmt::fmt` -> `fmt`, `PkgConfig::_ovt_pc_xxHash` and `/usr/lib/libxxhash.so` -> `xxhash`,
    `zstd::libzstd_static` -> `zstd`, so one component reached by several spellings is one key.
    """
    n = name
    if n.startswith("/"):  # a library file
        n = re.sub(r"\.(so|a|dylib|lib)(\.[\d.]+)?$", "", n.rsplit("/", 1)[1])
        n = re.sub(r"^lib", "", n)
    elif "::" in n:
        n = n.rsplit("::", 1)[1]
        n = re.sub(r"^_ovt_pc_", "", n)
        n = re.sub(r"^lib(?=\w+_(static|shared)$)", "", n)
        n = re.sub(r"_(static|shared)$", "", n)
    try:
        key = dep_id(n)
    except Exception:  # a name without letters or digits  # noqa: BLE001
        return ""
    return ALIASES.get(key, key)


def cell(text: str) -> str:
    return text.replace("|", "\\|").replace("\n", " ")


def normalise(text: str) -> str:
    """`\\n` line ends, no trailing blanks at the end, one final newline (macOS and Linux give the same bytes)."""
    return text.replace("\r\n", "\n").replace("\r", "\n").rstrip() + "\n"


# --- inputs -----------------------------------------------------------------------------------------------------


def read_pins(path: Path, root: Path) -> dict[str, dict]:
    """Name -> {url, patches[(name, upstream status)]} from the VERSIONS.md table."""
    lines = [ln for ln in path.read_text(encoding="utf-8").splitlines() if ln.lstrip().startswith("|")]
    if len(lines) < 3:
        raise UsageError(f"{path}: no table found")
    header = [c.lower() for c in lg.split_row(lines[0])]
    try:
        i_name, i_url, i_patch = (
            header.index("name"),
            header.index("url"),
            header.index("patches"),
        )
    except ValueError as e:
        raise UsageError(f"{path}: table needs Name, URL and Patches columns") from e
    pins: dict[str, dict] = {}
    for line in lines[2:]:
        cells = lg.split_row(line)
        if len(cells) <= max(i_name, i_url, i_patch):
            continue
        m = re.search(r"https?://[^\s<>`)]+", cells[i_url])
        patches = []
        for p in re.findall(r"[^\s,`]+\.patch", cells[i_patch]):
            patches.append((Path(p).name, upstream_status(root / p)))
        pins.setdefault(key_of(cells[i_name]), {"url": m.group(0) if m else "", "patches": patches})
    return pins


def upstream_status(patch: Path) -> str:
    try:
        for ln in patch.read_text(encoding="utf-8", errors="replace").splitlines()[:60]:
            if ln.startswith("Upstream-Status:"):
                return ln.split(":", 1)[1].strip()
    except OSError:
        pass
    return ""


def read_derived(path: Path | None) -> list[dict]:
    if path is None or not path.exists():
        return []
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as e:
        raise UsageError(f"cannot read {path}: {e}") from e
    rows = data.get("files", [])
    for r in rows:
        for k in ("path", "upstream", "commit", "license"):
            if not isinstance(r.get(k), str) or not r[k]:
                raise UsageError(f"{path}: every [[files]] entry needs a non-empty string `{k}`")
    return sorted(rows, key=lambda r: r["path"])


def components(report: dict) -> list[dict]:
    """Inventory rows that some build target links, one per component, sorted by name."""
    seen: dict[str, dict] = {}
    for e in lg.inventory(report):
        if e.get("targets"):
            seen.setdefault(key_of(e["component"]), e)
    return sorted(seen.values(), key=lambda e: (e["component"].lower(), e["component"]))


# --- licence texts ----------------------------------------------------------------------------------------------


def read_files(files: list[Path]) -> list[tuple[str, str]]:
    return [(f.name, normalise(f.read_text(encoding="utf-8", errors="replace"))) for f in files]


def regular(d: Path, pattern: str = "*") -> list[Path]:
    return sorted((p for p in d.glob(pattern) if p.is_file()), key=lambda p: p.name) if d.is_dir() else []


def find_texts(name: str, args: argparse.Namespace) -> list[tuple[str, str]]:
    key = key_of(name)
    spellings = [key, key.replace("_", "-")]  # vcpkg ports use hyphens (`nlohmann-json`)
    for base in args.licenses_dir:
        for k in spellings:
            for d in (base / k, base / k / "share" / "ovt" / "licenses" / k):
                files = regular(d)
                if files:
                    return read_files(files)
    if args.vcpkg_share:
        for k in spellings:
            files = regular(args.vcpkg_share / k, "copyright")
            if files:
                return read_files(files)
    if key == "qt" and args.qt_licenses:
        files = [p for p in regular(args.qt_licenses) if "gpl" in p.name.lower()]
        if files:
            return read_files(files)
    return []


# --- writer -----------------------------------------------------------------------------------------------------


def fence_for(text: str) -> str:
    longest = max((len(m) for m in re.findall(r"`+", text)), default=0)
    return "`" * max(3, longest + 1)


def modified_cell(patches: list[tuple[str, str]]) -> str:
    if not patches:
        return "no"
    listed = ", ".join(f"{n} (Upstream-Status: {s})" if s else n for n, s in patches)
    return f"**yes** — patches: {listed}"


def render(args: argparse.Namespace, report: dict, pins: dict, derived: list[dict]) -> tuple[str, list[str]]:
    comps = components(report)
    missing: list[str] = []
    texts: dict[str, list[tuple[str, str]]] = {}
    for c in comps:
        found = find_texts(c["component"], args)
        if found:
            texts[c["component"]] = found
        else:
            missing.append(c["component"])

    out: list[str] = []
    out.append(f"# Third-party notices for Open Video Tools {args.version} ({args.artifact})\n")
    out.append(
        "This document lists the third-party components included in this artefact, their licences and required "
        "notices. Open Video Tools itself is licensed as described in LICENSE and LICENSES/. Generated by "
        "tools/gen_notices.py; do not edit.\n"
    )
    out.append("## Summary\n")
    out.append("| Component | Version | Licence | Modified | Homepage |")
    out.append("|---|---|---|---|---|")
    for c in comps:
        pin = pins.get(key_of(c["component"]), {})
        lic = c.get("chosen") or c["license"]
        out.append(
            f"| {cell(c['component'])} | {cell(str(c.get('version', '')))} | {cell(lic)} | "
            f"{cell(modified_cell(pin.get('patches', [])))} | {cell(pin.get('url') or '–')} |"
        )
    out.append("")
    out.append("## Required statements\n")
    statements = [AV1_SENTENCE, FFMPEG_SENTENCE]
    if args.patent_notice:
        statements.append(PATENT_NOTICE)
    out.append("\n\n".join(statements) + "\n")
    out.append("## Licence texts\n")
    for c in comps:
        files = texts.get(c["component"])
        if not files:
            continue
        out.append(f"### {c['component']}\n")
        for fname, body in files:
            if len(files) > 1:
                out.append(f"**{fname}**\n")
            fence = fence_for(body)
            out.append(f"{fence}text\n{body}{fence}\n")
    out.append("## Files derived from other projects\n")
    if derived:
        out.append("| File in OVT | Upstream | Commit | Licence |")
        out.append("|---|---|---|---|")
        for r in derived:
            out.append(f"| {cell(r['path'])} | {cell(r['upstream'])} | {cell(r['commit'])} | {cell(r['license'])} |")
        out.append("")
    else:
        out.append("None.\n")
    return "\n".join(out), missing


def atomic_write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(text.encode("utf-8"))
    tmp.replace(path)


# --- command line -----------------------------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(prog="gen_notices.py", description="Generate THIRD_PARTY_NOTICES.md (§15.6).")
    ap.add_argument("--report", type=Path, required=True, help="license_gate_report.json")
    ap.add_argument("--versions", type=Path, required=True, help="third_party/VERSIONS.md")
    ap.add_argument("--licenses-dir", type=Path, action="append", default=[], help="repeatable")
    ap.add_argument("--qt-licenses", type=Path)
    ap.add_argument("--vcpkg-share", type=Path, help="<vcpkg prefix>/share (ports' copyright files)")
    ap.add_argument("--derived", type=Path, help="tools/derived_files.toml")
    ap.add_argument("--artifact", required=True, help="artefact id, shown in the heading")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument(
        "--patent-notice",
        action="store_true",
        help="codec pack: add the statement, write PATENT_NOTICE.md",
    )
    ap.add_argument("--version", help="product version (default: the VERSION file under --root)")
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    return ap


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.version is None:
            try:
                args.version = (args.root / "VERSION").read_text(encoding="utf-8").strip()
            except OSError as e:
                raise UsageError(f"no --version and cannot read {args.root / 'VERSION'}: {e}") from e
        try:
            report = json.loads(args.report.read_text(encoding="utf-8"))
            report["inventory"]
        except (OSError, ValueError, KeyError) as e:
            raise UsageError(f"cannot read gate report {args.report}: {e}") from e
        try:
            pins = read_pins(args.versions, args.root)
        except OSError as e:
            raise UsageError(f"cannot read {args.versions}: {e}") from e
        derived = read_derived(args.derived)
    except UsageError as e:
        print(f"gen_notices: {e}", file=sys.stderr)
        return 2
    text, missing = render(args, report, pins, derived)
    if missing:
        for name in missing:
            print(f"E-NOTICE-MISSING {name}", file=sys.stderr)
        return 1
    atomic_write(args.out, text)
    if args.patent_notice:
        atomic_write(
            args.out.with_name("PATENT_NOTICE.md"),
            "# Patent notice\n\n" + PATENT_NOTICE + "\n",
        )
    print(
        f"gen_notices: wrote {args.out} ({len(components(report))} components)",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
