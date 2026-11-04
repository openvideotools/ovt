#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Parse third_party/VERSIONS.md into CMake variables (chapter 13 §13.3.7, WI-0.010).

    versions_to_cmake.py [--versions VERSIONS.md] --out build/<preset>/third_party/versions.cmake
    versions_to_cmake.py [--versions VERSIONS.md] --update-hashes [--downloads DIR]
    versions_to_cmake.py [--versions VERSIONS.md] --check [--source-dir DIR]

VERSIONS.md is the single source of truth for third-party pins. The table rows become one variable triple
per row, OVT_TP_<dep>_{VERSION,URL,SHA256}, where <dep> is the row Name lower-cased with every run of
characters outside [a-z0-9] replaced by one underscore ("libaom", "ffmpeg", "vcpkg_baseline",
"android_ndk"). No other spelling is emitted. OVT_TP_<dep>_LICENSE (Licence column), OVT_TP_<dep>_PATCHES
(;-list of the Patches column) and OVT_TP_<dep>_ROW (the row, for error messages) come with it, and
OVT_TP_DEPS lists every <dep>.

Validation (every mode): duplicate <dep> ids, a SHA-256 cell holding a hex string that is not 64
lower-case hex digits, and a URL that embeds a version different from the Version column (WI-0.010 pitfall) are errors
naming the row. Placeholders (`<sha>`, `<commit>`) are allowed in the file; they become an empty SHA256 and
ovt_tp_declare() refuses to build a row without a hash.

--update-hashes downloads every archive row whose SHA-256 is still `<sha>` into --downloads, writes the
hash into the row and, for libaom, the `PATENTS sha256=` note (the archive's PATENTS file). It is the only
way hashes enter the file; never type one by hand.

--check is the VERSIONS.md <-> script consistency check (hooked into pre-commit by WI-0.007): for every row
whose Build column names an existing third_party/*.cmake script, the script must read OVT_TP_<dep>_URL and
OVT_TP_<dep>_SHA256 and must not spell the version literally.

Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
import tarfile
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_VERSIONS = REPO_ROOT / "third_party" / "VERSIONS.md"

PLACEHOLDERS = ("<sha>", "<commit>")
ARCHIVE_SUFFIXES = (".tar.gz", ".tgz", ".tar.xz", ".txz", ".tar.bz2", ".tbz2", ".tar", ".zip")
COLUMNS = ("Name", "Version", "Licence", "URL", "Patches", "SHA-256", "Build", "Notes")

_SHA_RE = re.compile(r"^[0-9a-f]{64}$")
_URL_RE = re.compile(r"(?:https?|file)://[^\s`|)]+")  # file:// for local mirrors and tests
_VERSION_TOKEN_RE = re.compile(r"(?<![0-9.])\d+(?:\.\d+)+(?![0-9])")


class VersionsError(Exception):
    """A VERSIONS.md problem; the message names the row."""


@dataclass
class Row:
    name: str
    dep: str
    version: str
    licence: str
    url_cell: str
    patches: str
    sha_cell: str
    build: str
    notes: str
    line: int  # 1-based line number in VERSIONS.md

    @property
    def url(self) -> str:
        """The first http(s) URL in the URL column, or "" (rows such as `sdkmanager` have none)."""
        m = _URL_RE.search(self.url_cell)
        return m.group(0) if m else ""

    @property
    def sha256(self) -> str:
        """The hash, or "" while the cell holds a placeholder or "–"."""
        cell = _strip_code(self.sha_cell).split()[0] if self.sha_cell.strip() else ""
        cell = _strip_code(cell)
        return cell if _SHA_RE.match(cell) else ""

    @property
    def sha_placeholder(self) -> bool:
        return "<sha>" in self.sha_cell

    @property
    def is_archive(self) -> bool:
        return self.url.lower().endswith(ARCHIVE_SUFFIXES)

    @property
    def patch_list(self) -> list[str]:
        """Patches column -> relative paths ("–" means none)."""
        return [p.strip("`") for p in re.split(r"[\s,;]+", self.patches) if p.strip("`") and p not in ("–", "-")]

    def where(self) -> str:
        return f"VERSIONS.md:{self.line} row '{self.name}'"


def dep_id(name: str) -> str:
    """Row Name -> the <dep> of OVT_TP_<dep>_*: lower case, runs of other characters become '_'.

    SPEC-ASSUMPTION(#15): §13.3.7 says only "lower-cased"; names with spaces need the '_' mapping.
    """
    ident = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")
    if not ident:
        raise VersionsError(f"row name {name!r} has no letters or digits")
    return ident


def _strip_code(text: str) -> str:
    return text.strip().strip("`").strip()


def _version_value(cell: str) -> str:
    """Version column -> VERSION value: backticks dropped, a trailing '(...)' remark dropped."""
    value = cell.replace("`", "").strip()
    value = re.sub(r"\s*\(.*\)\s*$", "", value)
    return value


def _split_row(line: str) -> list[str]:
    body = line.strip()
    if body.startswith("|"):
        body = body[1:]
    if body.endswith("|"):
        body = body[:-1]
    return [c.strip() for c in body.split("|")]


def parse(text: str) -> list[Row]:
    """Parses the dependency table: the first Markdown table whose header starts with | Name | Version |."""
    rows: list[Row] = []
    in_table = False
    for lineno, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        if not in_table:
            if stripped.startswith("|") and tuple(_split_row(stripped)) == COLUMNS:
                in_table = True
            continue
        if not stripped.startswith("|"):
            if rows:
                break
            continue
        cells = _split_row(stripped)
        if all(re.fullmatch(r":?-+:?", c) for c in cells):
            continue  # separator line
        if len(cells) != len(COLUMNS):
            raise VersionsError(f"VERSIONS.md:{lineno}: expected {len(COLUMNS)} columns, found {len(cells)}")
        name, version, licence, url, patches, sha, build, notes = cells
        rows.append(Row(name, dep_id(name), version, licence, url, patches, sha, build, notes, lineno))
    if not in_table:
        raise VersionsError("VERSIONS.md: no table with the columns " + " | ".join(COLUMNS))
    validate(rows)
    return rows


def validate(rows: list[Row]) -> None:
    seen: dict[str, Row] = {}
    for row in rows:
        if row.dep in seen:
            raise VersionsError(f"{row.where()}: id '{row.dep}' already used by {seen[row.dep].where()}")
        seen[row.dep] = row
        sha = _strip_code(row.sha_cell)
        first = _strip_code(sha.split()[0]) if sha else ""
        # Free text ("per file", "– (git)") is allowed; something that looks like a hash must be one.
        if re.fullmatch(r"[0-9A-Fa-f]{8,}", first) and not _SHA_RE.match(first):
            raise VersionsError(f"{row.where()}: SHA-256 '{row.sha_cell}' is not 64 lower-case hex digits")
        check_url_version(row)


def check_url_version(row: Row) -> None:
    """Rejects a URL that embeds a version other than the Version column (a dotted prefix such as Qt's
    `qt/6.8/` for 6.8.3 is allowed)."""
    version_tokens = _VERSION_TOKEN_RE.findall(_version_value(row.version))
    if not version_tokens or not row.url:
        return
    version = version_tokens[0]
    for token in _VERSION_TOKEN_RE.findall(row.url):
        if token != version and not version.startswith(token + "."):
            raise VersionsError(
                f"{row.where()}: URL {row.url} embeds version {token}, but the Version column is {version}")


def _cmake_quote(value: str, keep_list: bool = False) -> str:
    out = value.replace("\\", "\\\\").replace('"', '\\"').replace("$", "\\$")
    return '"' + (out if keep_list else out.replace(";", "\\;")) + '"'


def to_cmake(rows: list[Row], source: Path) -> str:
    out = [
        f"# Generated by tools/versions_to_cmake.py from {source.as_posix()}. Do not edit.",
        "# One OVT_TP_<dep>_{VERSION,URL,SHA256,LICENSE} set per row (chapter 13 §13.3.7).",
    ]
    for row in rows:
        out.append(f"set(OVT_TP_{row.dep}_VERSION {_cmake_quote(_version_value(row.version))})")
        out.append(f"set(OVT_TP_{row.dep}_URL {_cmake_quote(row.url)})")
        out.append(f"set(OVT_TP_{row.dep}_SHA256 {_cmake_quote(row.sha256)})")
        out.append(f"set(OVT_TP_{row.dep}_LICENSE {_cmake_quote(row.licence)})")
        out.append(f"set(OVT_TP_{row.dep}_PATCHES {_cmake_quote(';'.join(row.patch_list), keep_list=True)})")
        out.append(f"set(OVT_TP_{row.dep}_ROW {_cmake_quote(row.where())})")
    out.append("set(OVT_TP_DEPS " + " ".join(row.dep for row in rows) + ")")
    return "\n".join(out) + "\n"


def write_if_changed(path: Path, content: str) -> None:
    """Keeps the timestamp of an unchanged file, so configure does not trigger needless rebuilds."""
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


# ---- downloads ---------------------------------------------------------------------------------------

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def archive_name(row: Row) -> str:
    return row.url.rstrip("/").rsplit("/", 1)[-1]


def download(url: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".part")
    with urllib.request.urlopen(url, timeout=120) as resp, tmp.open("wb") as f:  # noqa: S310 (https pins)
        while chunk := resp.read(1 << 20):
            f.write(chunk)
    tmp.replace(dest)


def member_sha256(archive: Path, member_name: str) -> str | None:
    """SHA-256 of the first archive member whose basename is member_name and that sits at the top of the
    archive's root directory, or None."""
    def top_level(name: str) -> bool:
        parts = [p for p in name.split("/") if p not in ("", ".")]
        return len(parts) <= 2 and parts[-1] == member_name

    if archive.name.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            for info in z.infolist():
                if top_level(info.filename):
                    return hashlib.sha256(z.read(info)).hexdigest()
        return None
    with tarfile.open(archive) as t:
        for member in t:
            if member.isfile() and top_level(member.name):
                f = t.extractfile(member)
                return hashlib.sha256(f.read()).hexdigest() if f else None
    return None


def update_hashes(path: Path, downloads: Path) -> int:
    text = path.read_text(encoding="utf-8")
    rows = parse(text)
    lines = text.splitlines(keepends=True)
    failures = 0
    for row in rows:
        if not (row.sha_placeholder and row.is_archive):
            continue
        dest = downloads / archive_name(row)
        try:
            if not dest.exists():
                print(f"downloading {row.url}", file=sys.stderr)
                download(row.url, dest)
        except OSError as e:
            print(f"error: {row.where()}: download of {row.url} failed: {e}", file=sys.stderr)
            failures += 1
            continue
        digest = sha256_file(dest)
        line = lines[row.line - 1]
        cells = line.split("|")
        # cells[0] is the text before the first '|'; column i is cells[i + 1].
        sha_index = COLUMNS.index("SHA-256") + 1
        cells[sha_index] = cells[sha_index].replace("<sha>", digest, 1)
        if row.dep == "libaom":
            patents = member_sha256(dest, "PATENTS")
            notes_index = COLUMNS.index("Notes") + 1
            if patents:
                cells[notes_index] = cells[notes_index].replace("PATENTS sha256=<sha>", f"PATENTS sha256={patents}")
            else:
                print(f"error: {row.where()}: {dest.name} has no top-level PATENTS file", file=sys.stderr)
                failures += 1
        lines[row.line - 1] = "|".join(cells)
        print(f"{row.name}: {digest}")
    path.write_text("".join(lines), encoding="utf-8")
    parse(path.read_text(encoding="utf-8"))  # the edit must still parse
    return 1 if failures else 0


# ---- consistency check (WI-0.007 hooks it) -------------------------------------------------------------

def check_scripts(rows: list[Row], source_dir: Path) -> list[str]:
    problems = []
    for row in rows:
        m = re.search(r"third_party/[A-Za-z0-9_.-]+\.cmake", row.build)
        if not m:
            continue
        script = source_dir / m.group(0)
        if not script.exists():
            continue  # the script arrives with its own work item
        body = script.read_text(encoding="utf-8")
        for var in (f"OVT_TP_{row.dep}_URL", f"OVT_TP_{row.dep}_SHA256"):
            if var not in body:
                problems.append(f"{row.where()}: {m.group(0)} does not use {var}")
        version = _version_value(row.version)
        if re.search(r"\d", version) and version in body:
            problems.append(f"{row.where()}: {m.group(0)} spells the version {version} literally; "
                            f"use OVT_TP_{row.dep}_VERSION")
        if re.search(r"\bOVT_[A-Z0-9]+_(VERSION|URL|SHA256)\b", body.replace("OVT_TP_", "")):
            problems.append(f"{m.group(0)}: uses an OVT_<DEP>_* spelling; only OVT_TP_<dep>_* exists")
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--versions", type=Path, default=DEFAULT_VERSIONS, help="VERSIONS.md to read")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--out", type=Path, help="write the CMake variables to this file")
    mode.add_argument("--update-hashes", action="store_true", help="fill the <sha> placeholders")
    mode.add_argument("--check", action="store_true", help="validate the table and the consuming scripts")
    ap.add_argument("--downloads", type=Path, default=REPO_ROOT / "build" / "third_party" / "downloads",
                    help="archive cache for --update-hashes (OVT_TP_DOWNLOADS)")
    ap.add_argument("--source-dir", type=Path, default=REPO_ROOT, help="repository root for --check")
    args = ap.parse_args(argv)
    try:
        if args.update_hashes:
            return update_hashes(args.versions, args.downloads)
        rows = parse(args.versions.read_text(encoding="utf-8"))
        if args.check:
            problems = check_scripts(rows, args.source_dir)
            for p in problems:
                print(f"error: {p}", file=sys.stderr)
            return 1 if problems else 0
        write_if_changed(args.out, to_cmake(rows, args.versions))
        return 0
    except (VersionsError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
