#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate the ISOBMFF box registry core/src/container/isobmff/box_registry.gen.cpp (chapter 08 §8.2.10, WI-1.203).

    python3 -I tools/gen_box_registry.py            # regenerate from the committed inputs
    python3 -I tools/gen_box_registry.py --check    # exit 1 when a committed output differs (CI `registry-check`)
    python3 -I tools/gen_box_registry.py --fetch    # refresh tools/registry_inputs/ from upstream, then regenerate

Inputs (tools/registry_inputs/; the upstream file list is tools/mp4ra_sources.json):
  upstream/mp4ra_*.csv     MP4RA code lists: names of boxes, brands, handlers, item types, schemes, sample entries
  upstream/ffc/*.json      MPEG FileFormatConformance `standard_features`: type, containers, versions, flags, syntax
  ovt_extra_boxes.json     hand-maintained boxes absent from both (same entry shape; wins over the conformance JSON)
  INPUTS.sha256            provenance (`# <name> <url> <commit> <date>` lines) and the SHA-256 of every input file

SPEC-ASSUMPTION(#574): the upstream snapshots are not committed until the owner rules on their licences (MP4RA states
none, the conformance data is BSD-3-Clause-Clear). `upstream/` is a git-ignored cache: when a file is missing the
generator clones the commit pinned in INPUTS.sha256 into a new, empty directory, and every file must match its
committed SHA-256 before it is read. --fetch moves the pins to the upstream HEADs.

Outputs: box_registry.gen.cpp (committed, so the build needs no Python) and tools/registry_inputs/REGISTRY_DIFF.md.
Deterministic: no timestamps other than the snapshot date recorded by --fetch; tables are sorted. Standard library
only.
"""

from __future__ import annotations

import argparse
import csv
import datetime
import difflib
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INPUTS = ROOT / "tools" / "registry_inputs"
SOURCES = ROOT / "tools" / "mp4ra_sources.json"
OUT_CPP = ROOT / "core" / "src" / "container" / "isobmff" / "box_registry.gen.cpp"
OUT_DIFF = INPUTS / "REGISTRY_DIFF.md"
DECODER_DIRS = [ROOT / "core" / "src" / "container" / "isobmff"]

# §8.2.10 step 4: the hand-maintained kind lists.
K_CONTAINERS = set(
    "moov trak edts mdia minf dinf stbl mvex moof traf mfra udta meta ipco iprp sinf schi tref iref ilst wave gmhd "
    "tapt strk strd".split()
)
K_OPAQUE = set("mdat idat free skip wide".split())
K_CONTAINER_WITH_PREFIX = set("stsd dref iinf ipma meta".split())
# Boxes whose payload may legitimately end with bytes the syntax does not describe (avcC: the profile-dependent
# extension is often missing or padded).
K_TRAILING_DATA_OK = {"avcC"}
# FullBox subclasses of the conformance JSON `type` column (they all extend FullBox in the spec text).
FULLBOX_TYPES = {
    "FullBox",
    "ItemFullProperty",
    "EntityToGroupBox",
    "TrackGroupTypeBox",
    "DataEntryBaseBox",
    "LoudnessBaseBox",
}
# Conformance JSON types that are not boxes at all (an `infe` extension record).
NOT_BOX_TYPES = {"ItemInfoExtension"}
# Context of a container named by its class rather than its fourcc.
SAMPLE_ENTRY_PATTERN = "*/stsd/*"
FIELD_TOKENS = re.compile(r"\b(unsigned\s+int|signed\s+int|int|uint|bit|utf8string|string|utfstring|char|float)\s*\(")
NAME_TABLES = [  # (C++ array, input CSV)
    ("kBrands", "mp4ra_brands.csv"),
    ("kHandlers", "mp4ra_handlers.csv"),
    ("kItemTypes", "mp4ra_item_types.csv"),
    ("kSchemes", "mp4ra_schemes.csv"),
]
BOX_NAME_CSVS = ["mp4ra_boxes.csv", "mp4ra_boxes_udta.csv", "mp4ra_boxes_qt.csv"]
KIND_NAMES = {"Container", "ContainerWithPrefix", "Leaf", "Opaque"}
# SPEC-ASSUMPTION(#574): MP4RA states no licence for its data, so until the owner rules (licence question of
# WI-1.203) its description column is not copied into the generated file: box rows take the conformance JSON or
# extra description, and the name tables carry codes and specification names only. Flip to restore them.
USE_MP4RA_DESCRIPTIONS = False


class InputError(Exception):
    pass


def norm_fourcc(code: str) -> str:
    """§8.2.10 step 1: `$20` -> space, `(c)` -> ©; the result is exactly four characters (Latin-1 bytes)."""
    if code.startswith("uuid:"):
        return code.lower()
    s = code.replace("$20", " ").replace("(c)", "©")
    if len(s) != 4 or any(ord(c) > 0xFF for c in s):
        raise InputError(f"fourcc {code!r} does not normalise to four Latin-1 characters")
    return s


# ---------------------------------------------------------------------------------------------------------------
# Inputs


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as f:
        return [{k: (v or "").strip() for k, v in row.items()} for row in csv.DictReader(f)]


def load_names(inputs: Path, files: list[str]) -> dict[str, tuple[str, str]]:
    """fourcc -> (description, specification). A code listed twice keeps its ISO row, else the first."""
    names: dict[str, tuple[str, str]] = {}
    for name in files:
        for row in read_csv(inputs / name):
            cc = norm_fourcc(row["code"])
            entry = (row["description"], row["specification"])
            if cc not in names or (row["specification"] == "ISO" and names[cc][1] != "ISO"):
                names[cc] = entry
    return names


@dataclass
class Def:
    """One conformance-JSON (or extra) entry of a fourcc."""

    fourcc: str
    type: str
    description: str
    containers: list = field(default_factory=list)
    versions: list | None = None
    flags: list | None = None
    syntax: str = ""
    spec_ref: str = ""
    kind: str = ""
    origin: str = ""


def spec_label(info: dict) -> str:
    date = info.get("date", -1)
    return f"ISO/IEC {info['ISO']}" + (f":{date}" if isinstance(date, int) and date > 0 else "")


def load_ffc(inputs: Path) -> list[Def]:
    ffc = inputs / "ffc"
    labels = {p.name.split("_")[0]: spec_label(json.loads(p.read_text("utf-8"))) for p in ffc.glob("*_spec_info.json")}
    out: list[Def] = []
    for path in sorted(ffc.glob("*.json")):
        spec, _, kind = path.stem.partition("_")
        if kind == "spec_info":
            continue
        if spec not in labels:
            raise InputError(f"{path.name}: no {spec}_spec_info.json")
        for e in json.loads(path.read_text("utf-8"))["entries"]:
            if e.get("type") is None or e.get("type") in NOT_BOX_TYPES:
                continue  # deprecated rows (no type) and records that are not boxes
            out.append(
                Def(
                    fourcc=norm_fourcc(e["fourcc"]),
                    type=e["type"],
                    description=e.get("description") or "",
                    containers=e.get("containers") or [],
                    versions=e.get("versions"),
                    flags=e.get("flags"),
                    syntax=e.get("syntax") or "",
                    spec_ref=labels[spec],
                    origin=path.name,
                )
            )
    return out


def load_extras(path: Path) -> list[Def]:
    data = json.loads(path.read_text("utf-8"))
    out = []
    for e in data["entries"]:
        kind = e.get("kind", "")
        if kind and kind not in KIND_NAMES:
            raise InputError(f"{path.name}: {e['fourcc']}: unknown kind {kind!r}")
        out.append(
            Def(
                fourcc=norm_fourcc(e["fourcc"]),
                type=e["type"],
                description=e.get("description") or "",
                containers=e.get("containers") or [],
                versions=e.get("versions"),
                flags=e.get("flags"),
                syntax=e.get("syntax") or "",
                spec_ref=e.get("spec_ref", ""),
                kind=kind,
                origin=path.name,
            )
        )
    return out


# ---------------------------------------------------------------------------------------------------------------
# Classification (§8.2.10 steps 2-4)


def is_fullbox(d: Def) -> bool:
    return d.type in FULLBOX_TYPES or re.search(r"extends\s+FullBox\b", d.syntax) is not None


def is_sample_entry(d: Def) -> bool:
    return d.type.endswith("SampleEntry") or d.type.endswith("SampleEntryV1") or d.origin.endswith("_codecs.json")


def classify_kind(d: Def | None, cc: str) -> str:
    if d is not None and d.kind:
        return d.kind
    if cc in K_OPAQUE:
        return "Opaque"
    if cc in K_CONTAINER_WITH_PREFIX:
        return "ContainerWithPrefix"
    if cc in K_CONTAINERS:
        return "Container"
    if d is None:
        return "Leaf"
    if is_sample_entry(d):
        return "ContainerWithPrefix"  # fixed prefix, then child boxes (§8.3.2)
    if d.type == "Box" and only_child_boxes(d.syntax):
        return "Container"
    return "Leaf"


# SPEC-ASSUMPTION(#574): strict container heuristic (item 4).
def only_child_boxes(syntax: str) -> bool:
    """§8.2.10 step 4 heuristic, made strict: the first class body declares only members whose type is a box
    (`MovieHeaderBox mvhd;`, `Box other_boxes[];`) and no `int(`/`bit(`/string fields."""
    if FIELD_TOKENS.search(syntax) is not None or "{" not in syntax:
        return False
    body = syntax[syntax.index("{") + 1 :]
    body = body[: body.index("}")] if "}" in body else body
    body = re.sub(r"//[^\n]*|/\*.*?\*/", "", body, flags=re.S)
    members = [m.strip() for m in body.split(";") if m.strip()]
    return bool(members) and all(re.match(r"^\w*Box\b", m) is not None for m in members)


def versions_mask(d: Def | None, full: bool) -> int:
    if not full:
        return 0
    if d is None or not d.versions:
        return 0xFF  # SPEC-ASSUMPTION(#574): unknown, so do not check (item 4)
    mask = 0
    for v in d.versions:
        if not 0 <= int(v) <= 7:
            raise InputError(f"{d.fourcc}: version {v} out of range")
        mask |= 1 << int(v)
    return mask


def flag_mask(d: Def | None) -> int:
    if d is None or not d.flags:
        return 0xFFFFFF
    mask = 0
    for f in d.flags:
        if "value" not in f:  # {"wildcard": "any"}
            return 0xFFFFFF
        mask |= int(str(f["value"]), 16)
    return mask or 0xFFFFFF  # §8.2.10 step 4: 0 means "do not check"


def container_parts(c) -> tuple[list[str], list[str], bool]:
    """One `containers` item -> (parent fourccs, context patterns, resolvable)."""
    if isinstance(c, dict):
        fourccs = [norm_fourcc(x) for v in c.values() for x in v]
        return fourccs, [f"*/{x}" for x in fourccs], True
    if c == "file":
        return ["file"], [], True  # top level: only the "*" row matches an empty ancestor chain
    if c == "*":
        return [], [], False
    if len(c) == 4:
        cc = norm_fourcc(c)
        return [cc], [f"*/{cc}"], True
    if c.endswith("SampleEntry"):
        return [], [SAMPLE_ENTRY_PATTERN], False
    return [], [], False


# SPEC-ASSUMPTION(#574): container class names (item 3).
def allowed_parents(d: Def | None) -> str:
    """Comma list of parent fourccs ("file" = top level); "" when any container is unknown or named by class."""
    if d is None:
        return ""
    parents: list[str] = []
    for c in d.containers:
        fourccs, _, ok = container_parts(c)
        if not ok:
            return ""
        parents.extend(fourccs)
    return ",".join(sorted(set(parents)))


@dataclass(frozen=True)
class Row:
    fourcc: str
    pattern: str
    kind: str
    full: bool
    vmask: int
    fmask: int
    trailing: bool
    spec_ref: str
    description: str
    parents: str

    def signature(self) -> tuple:
        return (self.kind, self.full, self.vmask, self.fmask, self.spec_ref)


def make_row(cc: str, pattern: str, d: Def | None, names: dict, parents: str) -> Row:
    full = is_fullbox(d) if d is not None else False
    mp4ra = names.get(cc)
    desc = mp4ra[0] if mp4ra and USE_MP4RA_DESCRIPTIONS else (d.description if d else "")
    spec = d.spec_ref if d is not None and d.spec_ref else (mp4ra[1] if mp4ra else "")
    return Row(
        fourcc=cc,
        pattern=pattern,
        kind=classify_kind(d, cc),
        full=full,
        vmask=versions_mask(d, full),
        fmask=flag_mask(d),
        trailing=cc in K_TRAILING_DATA_OK,
        spec_ref=spec,
        description=desc,
        parents=parents,
    )


# SPEC-ASSUMPTION(#574): one "*" row per fourcc, plus context rows only for a definition that differs (item 2).
def build_rows(names: dict, defs: list[Def], extras: list[Def], sample_entry_names: dict) -> list[Row]:
    by_cc: dict[str, list[Def]] = {}
    for d in defs:
        by_cc.setdefault(d.fourcc, []).append(d)
    for cc in {d.fourcc for d in extras}:  # step 3: an extra replaces the conformance entries of its fourcc
        by_cc[cc] = [d for d in extras if d.fourcc == cc]
    rows: dict[tuple[str, str], Row] = {}
    for cc in sorted(set(names) | set(by_cc)):
        entries = by_cc.get(cc, [])
        if not entries:
            rows[(cc, "*")] = make_row(cc, "*", None, names, "")
            continue
        all_names = names if cc in names else {**sample_entry_names, **names}
        primary = entries[0]
        union = Def(cc, primary.type, primary.description, [c for d in entries for c in d.containers])
        rows[(cc, "*")] = make_row(cc, "*", primary, all_names, allowed_parents(union))
        base = make_row(cc, "*", primary, all_names, "").signature()
        covered = {p for c in primary.containers for p in container_parts(c)[1]}
        # Step 2: a definition that differs from the primary one gets a row per container it alone names (the
        # first such definition wins a clash; the primary definition answers through the "*" row).
        for d in entries[1:]:
            if make_row(cc, "*", d, all_names, "").signature() == base:
                continue
            for c in d.containers:
                for pattern in container_parts(c)[1]:
                    if pattern not in covered:
                        rows.setdefault((cc, pattern), make_row(cc, pattern, d, all_names, allowed_parents(d)))
    return [rows[k] for k in sorted(rows, key=lambda k: (k[0].encode("latin-1", "replace"), k[1]))]


def load_name_table(path: Path) -> list[tuple[str, str, str]]:
    seen: dict[str, tuple[str, str, str]] = {}
    for row in read_csv(path):
        cc = norm_fourcc(row["code"])
        seen.setdefault(cc, (cc, row["description"] if USE_MP4RA_DESCRIPTIONS else "", row["specification"]))
    return [seen[k] for k in sorted(seen, key=lambda s: s.encode("latin-1"))]


# ---------------------------------------------------------------------------------------------------------------
# Output


def c_str(s: str, latin1: bool = False) -> str:
    """A C string literal; non-ASCII as octal escapes (never ambiguous with a following character)."""
    data = s.encode("latin-1" if latin1 else "utf-8")
    out = []
    for b in data:
        ch = chr(b)
        if ch in '"\\':
            out.append("\\" + ch)
        elif 0x20 <= b < 0x7F and not (ch == "?" and out and out[-1] == "?"):  # no trigraphs
            out.append(ch)
        else:
            out.append(f"\\{b:03o}")
    return '"' + "".join(out) + '"'


def read_provenance(inputs: Path) -> dict[str, str]:
    prov: dict[str, str] = {}
    path = inputs / "INPUTS.sha256"
    if not path.exists():
        return prov
    for line in path.read_text("utf-8").splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[0] == "#" and parts[1] in ("mp4ra", "ffc"):
            prov[parts[1]] = parts[3][:12]
        elif len(parts) >= 3 and parts[:2] == ["#", "snapshot"]:
            prov["date"] = parts[2]
    return prov


def upstream_files() -> list[tuple[dict, str, str]]:
    """(repository, upstream path, local name under upstream/) for every file of tools/mp4ra_sources.json."""
    sources = json.loads(SOURCES.read_text("utf-8"))
    return [(repo, up, local) for repo in sources["repositories"] for up, local in sorted(repo["files"].items())]


def input_files(inputs: Path) -> list[Path]:
    files = sorted(inputs / "upstream" / local for _, _, local in upstream_files())
    files.append(inputs / "ovt_extra_boxes.json")
    return files


def pinned(inputs: Path) -> tuple[dict[str, str], dict[str, str]]:
    """INPUTS.sha256 -> ({repo name: commit}, {relative path: sha256})."""
    commits: dict[str, str] = {}
    hashes: dict[str, str] = {}
    path = inputs / "INPUTS.sha256"
    for line in path.read_text("utf-8").splitlines() if path.exists() else []:
        parts = line.split()
        if len(parts) >= 4 and parts[0] == "#" and parts[1] in ("mp4ra", "ffc"):
            commits[parts[1]] = parts[3]
        elif len(parts) == 2 and not line.startswith("#"):
            hashes[parts[1]] = parts[0]
    return commits, hashes


def clone_into(url: str, commit: str | None, files: list[tuple[str, str]], dest_root: Path) -> tuple[str, str]:
    """Clone `url` at `commit` (HEAD when None) into a new, empty directory and copy `files` (upstream -> local)."""
    with tempfile.TemporaryDirectory(prefix="ovt-registry-") as tmp:
        dest = Path(tmp) / "src"
        if commit is None:
            git("clone", "--quiet", "--depth", "1", url, str(dest))
        else:
            dest.mkdir()
            git("init", "--quiet", cwd=dest)
            git("fetch", "--quiet", "--depth", "1", url, commit, cwd=dest)
            git("checkout", "--quiet", "FETCH_HEAD", cwd=dest)
        sha = git("rev-parse", "HEAD", cwd=dest)
        date = git("log", "-1", "--format=%cs", cwd=dest)
        for upstream, local in files:
            target = dest_root / local
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(dest / upstream, target)
    return sha, date


def ensure_upstream(inputs: Path) -> None:
    """Fetch the pinned snapshots that are missing or stale, then verify every upstream file's SHA-256."""
    commits, hashes = pinned(inputs)
    up = inputs / "upstream"
    wanted = upstream_files()
    for name in sorted({repo["name"] for repo, _, _ in wanted}):
        files = [(u, loc) for repo, u, loc in wanted if repo["name"] == name]
        stale = [
            loc for _, loc in files if not (up / loc).exists() or sha256(up / loc) != hashes.get(f"upstream/{loc}")
        ]
        if stale:
            if name not in commits:
                raise InputError(f"INPUTS.sha256 pins no {name} commit; run --fetch")
            repo = next(r for r, _, _ in wanted if r["name"] == name)
            clone_into(repo["url"], commits[name], files, up)
    for _, _, loc in wanted:
        if sha256(up / loc) != hashes.get(f"upstream/{loc}"):
            raise InputError(f"upstream/{loc} does not match its SHA-256 in INPUTS.sha256")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


# REUSE-IgnoreStart
SPDX_LINES = [  # REUSE: INPUTS.sha256 is our own file (tools/ licence)
    "# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors",
    "# SPDX-License-Identifier: LGPL-2.1-or-later",
]
# The generated table carries facts and descriptions derived from FileFormatConformance (BSD-3-Clause-Clear), so its
# header names both licences; the first identifier stays OVT's own for tools/check_spdx_dirs.py.
CPP_SPDX_LINES = [
    "// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors",
    "// SPDX-FileCopyrightText: 2023 Apple Inc.",
    "// SPDX-License-Identifier: LGPL-2.1-or-later",
    "// SPDX-License-Identifier: BSD-3-Clause-Clear",
]
# REUSE-IgnoreEnd


def render_inputs_sha(inputs: Path) -> str:
    """INPUTS.sha256: the provenance comment lines kept as they are, then one hash line per input file."""
    path = inputs / "INPUTS.sha256"
    head = [ln for ln in path.read_text("utf-8").splitlines() if ln.startswith("#")] if path.exists() else []
    head = [*SPDX_LINES, *(ln for ln in head if ln not in SPDX_LINES)]
    body = [f"{sha256(p)}  {p.relative_to(inputs).as_posix()}" for p in input_files(inputs)]
    return "\n".join(head + body) + "\n"


def render_cpp(rows: list[Row], tables: dict[str, list], inputs: Path) -> str:
    prov = read_provenance(inputs)
    extra = sha256(inputs / "ovt_extra_boxes.json")[:12]
    lines = [
        *CPP_SPDX_LINES,
        "//",
        f"// GENERATED by tools/gen_box_registry.py from mp4ra@{prov.get('mp4ra', 'unknown')} "
        f"ffc@{prov.get('ffc', 'unknown')} ovt_extra@{extra}",
        f"// (snapshot {prov.get('date', 'unknown')}; input hashes in tools/registry_inputs/INPUTS.sha256). DO NOT EDIT.",
        "// Chapter 08 §8.2.10. Regenerate with `python3 -I tools/gen_box_registry.py`; CI runs it with --check.",
        "// Box facts and descriptions are derived from MPEG FileFormatConformance, Copyright (c) 2023 Apple Inc.,",
        "// used under BSD-3-Clause-Clear (LICENSES/BSD-3-Clause-Clear.txt).",
        "",
        '#include "container/isobmff/box_registry.h"',
        "",
        "namespace ovt::container::isobmff {",
        "",
        "// clang-format off",
        "// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)",
        "const RegistryEntry kEntries[] = {",
    ]
    for r in rows:
        lines.append(
            "    {"
            + ",".join(
                [
                    c_str(r.fourcc, latin1=not r.fourcc.startswith("uuid:")),
                    c_str(r.pattern),
                    f"BoxKind::{r.kind}",
                    "true" if r.full else "false",
                    f"0x{r.vmask:02X}",
                    f"0x{r.fmask:06X}",
                    "true" if r.trailing else "false",
                    c_str(r.spec_ref),
                    c_str(r.description),
                    c_str(r.parents),
                    "nullptr",
                ]
            )
            + "},"
        )
    lines += ["};", "const size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);", ""]
    for name, rows_ in tables.items():
        lines.append(f"const RegistryName {name}[] = {{")
        for cc, desc, spec in rows_:
            lines.append(f"    {{{c_str(cc, latin1=True)},{c_str(desc)},{c_str(spec)}}},")
        lines += ["};", f"const size_t {name[1:]}Count = sizeof({name}) / sizeof({name}[0]);", ""]
    lines += [
        "// NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)",
        "// clang-format on",
        "",
        "}  // namespace ovt::container::isobmff",
        "",
    ]
    return "\n".join(lines)


DECODER_RE = re.compile(r'OVT_BOX_DECODER\(\s*"([^"]*)"')


def hand_decoders() -> list[tuple[str, str]]:
    """(fourcc, file) of every OVT_BOX_DECODER( registration under core/src/container/isobmff (step 6)."""
    found = []
    for d in DECODER_DIRS:
        for path in sorted(d.rglob("*.cpp")):
            for m in DECODER_RE.finditer(path.read_text("utf-8")):
                found.append((m.group(1), path.relative_to(ROOT).as_posix()))
    return found


def decoder_has_entry(fourcc: str, codes: set[str]) -> bool:
    if fourcc.endswith("/*"):
        return fourcc[:-2] in codes  # "ilst/*": the parent must be registered
    return fourcc.lower() in codes if fourcc.startswith("uuid:") else fourcc in codes


def render_diff(names: dict, defs: list[Def], extras: list[Def], rows: list[Row]) -> tuple[str, list[str]]:
    in_json = {d.fourcc for d in defs}
    codes = {r.fourcc for r in rows}
    shell_only = sorted(set(names) - in_json - {d.fourcc for d in extras})
    now_registered = sorted({d.fourcc for d in extras if d.fourcc in names})
    errors = [f"`{cc}` ({path})" for cc, path in hand_decoders() if not decoder_has_entry(cc, codes)]

    def fmt(ccs: list[str]) -> str:  # wrapped at 120 columns (markdownlint MD013)
        if not ccs:
            return "none"
        lines, cur = [], ""
        for item in (f"`{c.replace(' ', '$20')}`," for c in ccs):  # MP4RA spelling: no spaces in code spans
            if cur and len(cur) + 1 + len(item) > 120:
                lines.append(cur)
                cur = item
            else:
                cur = f"{cur} {item}" if cur else item
        lines.append(cur[:-1])
        return "\n".join(lines)

    text = "\n".join(
        [
            "# Box registry diff",
            "",
            "<!-- GENERATED by tools/gen_box_registry.py. DO NOT EDIT. -->",
            "",
            "Chapter 08 §8.2.10 step 6. Regenerated with the registry; CI fails when the error list is not empty.",
            "A space in a fourcc is written `$20`.",
            "",
            f"Registry rows: {len(rows)}; fourccs: {len(codes)}.",
            "",
            "## In MP4RA but in no conformance JSON or extra entry (generic shell only)",
            "",
            fmt(shell_only),
            "",
            "## In ovt_extra_boxes.json and also registered by MP4RA",
            "",
            "MP4RA registers the name only; the extra entry stays while it carries type, container or version",
            "information the conformance JSON lacks.",
            "",
            fmt(now_registered),
            "",
            "## Errors: hand decoders (`OVT_BOX_DECODER`) with no registry entry",
            "",
            "\n".join(f"- {e}" for e in errors) if errors else "none",
            "",
        ]
    )
    return text, errors


def generate(inputs: Path) -> tuple[dict[Path, str], list[str]]:
    up = inputs / "upstream"
    names = load_names(up, BOX_NAME_CSVS)
    se_names = load_names(up, ["mp4ra_sample_entries.csv"])
    defs = load_ffc(up)
    extras = load_extras(inputs / "ovt_extra_boxes.json")
    rows = build_rows(names, defs, extras, se_names)
    tables = {name: load_name_table(up / csv_name) for name, csv_name in NAME_TABLES}
    diff, errors = render_diff(names, defs, extras, rows)
    outputs = {
        OUT_CPP: render_cpp(rows, tables, inputs),
        inputs / "REGISTRY_DIFF.md": diff,
        inputs / "INPUTS.sha256": render_inputs_sha(inputs),
    }
    return outputs, errors


# ---------------------------------------------------------------------------------------------------------------
# Fetch


def git(*args: str, cwd: Path | None = None) -> str:
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def fetch(inputs: Path) -> None:
    up = inputs / "upstream"
    old = set(load_names(up, BOX_NAME_CSVS)) | {d.fourcc for d in load_ffc(up)} if (up / "ffc").exists() else set()
    prov = [
        *SPDX_LINES,
        "# Inputs of tools/gen_box_registry.py (chapter 08 §8.2.10). The commit lines are written by --fetch; the",
        "# hash lines are rewritten by every generator run and compared by --check.",
    ]
    wanted = upstream_files()
    for name in sorted({repo["name"] for repo, _, _ in wanted}):
        repo = next(r for r, _, _ in wanted if r["name"] == name)
        sha, date = clone_into(repo["url"], None, [(u, loc) for r, u, loc in wanted if r["name"] == name], up)
        prov.append(f"# {name} {repo['url']} {sha} {date}")
    prov.append(f"# snapshot {datetime.date.today().isoformat()}")
    (inputs / "INPUTS.sha256").write_text("\n".join(prov) + "\n", encoding="utf-8")
    body = [f"{sha256(p)}  {p.relative_to(inputs).as_posix()}" for p in input_files(inputs)]
    (inputs / "INPUTS.sha256").write_text("\n".join(prov + body) + "\n", encoding="utf-8")
    new = set(load_names(up, BOX_NAME_CSVS)) | {d.fourcc for d in load_ffc(up)}
    print(f"added fourccs: {sorted(new - old)}\nremoved fourccs: {sorted(old - new)}")


# ---------------------------------------------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="fail when a committed output differs")
    mode.add_argument("--fetch", action="store_true", help="refresh the inputs from upstream, then regenerate")
    ap.add_argument("--inputs", type=Path, default=INPUTS, help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    try:
        if args.fetch:
            fetch(args.inputs)
        ensure_upstream(args.inputs)
        outputs, errors = generate(args.inputs)
    except (InputError, OSError, KeyError, ValueError, subprocess.CalledProcessError) as e:
        print(f"gen_box_registry: {e}", file=sys.stderr)
        return 2
    if args.check:
        stale = False
        for path, text in outputs.items():
            current = path.read_text("utf-8") if path.exists() else ""
            if current != text:
                stale = True
                rel = path.relative_to(ROOT).as_posix() if path.is_relative_to(ROOT) else str(path)
                sys.stdout.writelines(
                    difflib.unified_diff(current.splitlines(True), text.splitlines(True), rel, rel + " (generated)")
                )
        for err in errors:
            print(f"error: hand decoder without registry entry: {err}", file=sys.stderr)
        if stale:
            print(
                "gen_box_registry: committed outputs are stale; run python3 -I tools/gen_box_registry.py",
                file=sys.stderr,
            )
        return 1 if stale or errors else 0
    for path, text in outputs.items():
        path.write_text(text, encoding="utf-8", newline="\n")
    for err in errors:
        print(f"error: hand decoder without registry entry: {err}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
