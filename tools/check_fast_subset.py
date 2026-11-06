#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Fixture-registry checker (chapter 14 §14.3.5; WI-0.553).

    check_fast_subset.py [--all-chapters] [--spec-dir docs/spec] [--manifest FILE] [--recipes FILE]

Default mode checks the synthetic part of the registry against itself: every id of
``tests/corpora/synthetic/recipes.json`` has a ``source: syn`` manifest entry (and the reverse), every
recipe marked ✓ in §14.3.3 or §14.3.5 carries the ``fast`` tag, and every bare file name of the §14.3.5 alias
index that points at a ``syn`` id is in that entry's ``aliases``.

``--all-chapters`` greps every chapter for cited fixtures and fails on a name that is not in the registry:
``syn_*`` tokens, ``corpus/...`` paths, ``*.fb`` / ``*.fa`` / ``*.bin`` fixture names and cited media file
names. The registry is the manifest (ids and aliases), the recipes (ids, families, every file a recipe step
writes), the ids and paths named in §14.3.1–§14.3.3 (public corpora, fast subset, recipes and the
non-stream fixture table, corpus ids being ``<source>_<upstream stem>``) and the names of the §14.3.5 alias
index. Names with a placeholder (``<...>``) are skipped; names with ``*`` must match at least one registry
entry. ``# fixture-check: ignore <name> -- <reason>`` lines in this file's ``IGNORED`` table list names that
are command outputs or file formats rather than fixtures.

Exit codes: 0 every name known; 1 an unknown or inconsistent name (each is printed with file:line); 2 usage.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SOURCES = (
    "jvt",
    "jctvc",
    "aom",
    "argon",
    "mpegff",
    "gpac",
    "bento4",
    "dashif",
    "cta",
    "apple",
    "netflix",
    "ytugc",
    "hdr",
    "heif",
    "ts",
    "mkv",
    "misc",
    "syn",
)
MEDIA_EXT = "mp4|mkv|webm|m4a|heic|avif|ismv|ivf|264|265|hevc|bit|obu|y4m|yuv|m4s|mov|ts|fb|fa|bin"
TICK = re.compile(r"`([^`\n]+)`")
SYN_TOKEN = re.compile(r"(?<![A-Za-z0-9_/.-])syn_[A-Za-z0-9_]+(?![A-Za-z0-9_*<{])")
FILE_NAME = re.compile(rf"^[A-Za-z0-9_./*<>-]+\.(?:{MEDIA_EXT})$")
SOURCE_ID = re.compile(rf"\b(?:{'|'.join(SOURCES)})_[A-Za-z0-9_]+")

# Names cited in the spec that are outputs, cache files or formats rather than registry fixtures.
IGNORED = {
    "index.fb": "the on-disk stream index written by the core (chapter 6)",
    ".index.fb": "the on-disk stream index written by the core (chapter 6)",
    "ovt.fb": "the FlatBuffers Python package name",
    "labels_zoom32.fb": "a render golden written by chapter 10 tests, not an input",
    "fa_1080p_dense.bin": "built at test time by the perf harness from tools/gen_dense_fa.py",
    "plcache.bin": "the libplacebo shader cache in the user's cache dir",
    "f.mp4": "the mp4fragment intermediate of §14.3.3 recipes 68 and 69",
}


def expand_braces(text: str) -> list[str]:
    m = re.search(r"\{([^{}]*,[^{}]*)\}", text)
    if not m:
        return [text]
    return [x for alt in m.group(1).split(",") for x in expand_braces(text[: m.start()] + alt + text[m.end() :])]


def section(text: str, start: str, end: str) -> str:
    a = text.index(start)
    b = text.index(end, a)
    return text[a:b]


def to_id(source: str, stem: str) -> str:
    return f"{source}_{re.sub(r'[-.]', '_', stem.split('/')[-1])}"


class Registry:
    def __init__(self, spec_dir: Path, manifest: dict, recipes: dict) -> None:
        self.ids: set[str] = set()
        self.names: set[str] = set()  # file names, paths and aliases
        self.families = tuple(recipes.get("families", {}))
        for entry in manifest["entries"]:
            self.ids.add(entry["id"])
            self.names.update(entry.get("aliases", []))
            if "path" in entry:
                self.names.update({entry["path"], Path(entry["path"]).name})
        for rec in recipes["recipes"]:
            self.ids.add(rec["id"])
            self.names.update(rec.get("aliases", []))
            for step in rec["steps"]:
                for argv in [step["cmd"]] if "cmd" in step else step["pipe"]:
                    for arg in [*argv, step.get("stdout", "")]:
                        self.names.update(m.split(":")[0] for m in re.findall(r"[A-Za-z0-9_.$-]+\.[a-z0-9]+", arg))
            self.names.update(rec["files"])
            if rec.get("mp4"):
                self.names.add(Path(rec["mp4"]).with_suffix(".mp4").name)
        for fixture in recipes.get("fixtures", []):
            self.names.update({fixture["path"], Path(fixture["path"]).name})
            for path in fixture.get("outputs", []):
                self.names.update({path, Path(path).name})
        self.stems = {Path(n).stem for n in self.names}
        ch14 = (spec_dir / "14-testing.md").read_text(encoding="utf-8")
        catalogue = section(ch14, "### 14.3.1", "### 14.3.4")
        self.ids.update(SOURCE_ID.findall(catalogue))
        self.names.update(n for cell in TICK.findall(catalogue) for n in expand_braces(cell) if FILE_NAME.match(n))
        for bullet in re.findall(r"^- .*$", section(ch14, "### 14.3.2", "### 14.3.3"), re.MULTILINE):
            src = re.search(r"\(`([a-z0-9]+)`\)", bullet)
            if src and src.group(1) in SOURCES:
                for stem in TICK.findall(bullet):
                    for name in expand_braces(stem):
                        if re.fullmatch(r"[A-Za-z0-9_./-]+", name):
                            self.ids.add(to_id(src.group(1), name))
        for cell in TICK.findall(section(ch14, "**Non-stream fixtures", "Rules for `generate.py`")):
            for name in expand_braces(cell):
                self.names.update({name, Path(name).name})
        self.alias_rows = []
        for row in section(ch14, "### 14.3.5", "\n---").splitlines():
            cells = row.split("|")
            if len(cells) < 6 or not cells[1].strip() or cells[1].strip().startswith(("Name", "---")):
                continue
            cited, target, fast = cells[1], cells[2], "✓" in cells[4]
            self.alias_rows.append((TICK.findall(cited), TICK.findall(target), fast))
            for name in TICK.findall(cited):
                for n in expand_braces(name):
                    self.names.update({n, Path(n).name})
            self.ids.update(t for t in TICK.findall(target) if re.fullmatch(r"[A-Za-z0-9_]+", t))
        self.names.update(IGNORED)

    def known_id(self, name: str) -> bool:
        if "*" in name:
            return any(fnmatch.fnmatchcase(i, name) for i in self.ids) or name.split("_")[0] in SOURCES
        return name in self.ids or name.startswith(self.families) or name in self.stems

    def known_name(self, name: str) -> bool:
        if name in self.names or Path(name).name in self.names or Path(name).stem in self.ids:
            return True
        if "*" in name and any(fnmatch.fnmatchcase(n, Path(name).name) for n in self.names):
            return True
        # the upstream file of a public corpus id <source>_<stem>
        lower = {i.lower() for i in self.ids}
        for src in SOURCES:
            want = to_id(src, Path(name).stem).lower()
            if want in lower or ("*" in want and any(fnmatch.fnmatchcase(i, want) for i in lower)):
                return True
        return False


def scan_chapter(path: Path, reg: Registry) -> list[str]:
    problems = []
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        where = f"{path.name}:{lineno}"
        for token in SYN_TOKEN.findall(line):
            if not token.endswith("_") and not reg.known_id(token):
                problems.append(f"{where}: unknown corpus id {token}")
        for span in TICK.findall(line):
            for name in expand_braces(span.strip()):
                if "<" in name or "…" in name or ".." in name or " " in name or name.startswith("~"):
                    continue
                if re.fullmatch(r"\.[a-z0-9]+|\*\.[a-z0-9]+|corpus/[A-Za-z0-9_]+/", name):
                    continue  # an extension, a format glob or a corpus directory, not a fixture name
                if name.startswith("corpus/"):
                    rest = name[len("corpus/") :].rstrip("/")
                    if not (reg.known_id(rest) if "/" not in rest else reg.known_name(name)):
                        problems.append(f"{where}: unknown fixture {name}")
                elif FILE_NAME.match(name) and "/" not in name.split("*")[0] or re.search(r"\.(fb|fa|bin)$", name):
                    if not reg.known_name(name):
                        problems.append(f"{where}: unknown fixture file {name}")
    return problems


# Stems that §14.3.2 mentions as examples of a *wrong* spelling, not as fixtures (spelling rule of the jvt bullet).
NOT_FIXTURES = {"jvt_FRExt1_Panasonic_C", "jvt_HPCAFL_BCRM_C"}
PROSE = {
    "profile0_core",
    "no_output_of_prior_pics_flag",
    "RPS_A_docomo",
    "RPS_F_docomo",
    "professional_profiles",
}  # prose, not stems
PUBLIC = tuple(s for s in SOURCES if s != "syn")


def named_public_ids(ch14: str) -> dict[str, str]:
    """Every public-corpus id the catalogue names: id -> where (§14.3.1 fixture table or §14.3.2 bullet).

    §14.3.2 bullets that carry ``(`<source>`)`` list upstream stems (`FRExt/X` is id ``<source>_X``; a bare
    ``-16x18`` is the previous name with its size suffix replaced); the ISOBMFF/metrics bullets and the
    §14.3.1 fixture table name ids directly. Wildcards (``*``) and ``X`` ranges are skipped or expanded.
    """
    found: dict[str, str] = {}
    table = section(ch14, "**Named public-corpus fixtures.**", "### 14.3.2")
    for row in table.splitlines():
        cells = row.split("|")
        if len(cells) < 4 or cells[1].strip().startswith(("Fixture", "---")):
            continue
        ticks = [t for t in TICK.findall(cells[1]) if re.fullmatch(rf"(?:{'|'.join(PUBLIC)})_[A-Za-z0-9_]+", t)]
        if len(ticks) == 2 and "…" in cells[1]:  # `mkv_test1` … `mkv_test8`
            stem = re.fullmatch(r"(.*?)(\d+)", ticks[0])
            last = re.search(r"\d+$", ticks[1])
            assert stem is not None and last is not None
            lo, hi = int(stem.group(2)), int(last.group(0))
            ticks = [f"{stem.group(1)}{n}" for n in range(lo, hi + 1)]
        found.update({t: "§14.3.1 fixture table" for t in ticks})
    for bullet in re.findall(r"^- .*$", section(ch14, "### 14.3.2", "### 14.3.3"), re.MULTILINE):
        src = re.search(r"\(`([a-z0-9]+)`\)", bullet)
        previous = ""
        for tick in TICK.findall(bullet):
            for name in expand_braces(tick):
                if "*" in name or not re.fullmatch(r"-?[A-Za-z0-9_./-]+", name) or name.endswith(("_", "/")):
                    continue
                if not re.search(r"[\d_/]", name) or "golden" in name or name.endswith((".py", ".cc")) or name in PROSE:
                    continue  # a tag, a tool or a corpus name; stems carry a digit, an underscore or a folder
                if re.fullmatch(rf"(?:{'|'.join(PUBLIC)})_[A-Za-z0-9_]+", name):
                    found[name] = "§14.3.2 bullet"
                elif src and src.group(1) in PUBLIC:
                    if name.startswith("-"):
                        name = re.sub(r"-?\d+x\d+$", "", previous) + name  # `-16x18` after `…-size-16x16`
                    previous = name
                    found[to_id(src.group(1), name)] = "§14.3.2 bullet"
    return {k: v for k, v in found.items() if k not in NOT_FIXTURES}


def check_named(manifest: dict, ch14: str) -> list[str]:
    """Acceptance 3 of WI-0.552: every stream named in §14.3.1/§14.3.2 has a manifest entry (id or alias)."""
    have = {e["id"] for e in manifest["entries"]} | {a for e in manifest["entries"] for a in e.get("aliases", [])}
    return [
        f"manifest.json: {i} ({where}) has no entry"
        for i, where in sorted(named_public_ids(ch14).items())
        if i not in have
    ]


def check_registry(reg: Registry, manifest: dict, recipes: dict) -> list[str]:
    problems = []
    entries = {e["id"]: e for e in manifest["entries"] if e["source"] == "syn" and e.get("kind", "stream") == "stream"}
    rec_ids = {r["id"]: r for r in recipes["recipes"]}
    problems += [f"recipes.json: {i} has no manifest entry" for i in rec_ids if i not in entries]
    problems += [f"manifest.json: {i} has no recipe" for i in entries if i not in rec_ids and "kind" in entries[i]]
    for cited, targets, fast in reg.alias_rows:
        # Path aliases (corpus/<dir>/<file>) are resolved by this checker, not by the fetcher; a row that maps
        # n names to n ids pairs them in order.
        bare = [] if any("/" in name for name in cited) else [n for n in cited if "…" not in n]
        paired = len(targets) == len(bare) > 1
        for index, target in enumerate(targets):
            entry = entries.get(target)
            if entry is None:
                continue
            if fast and "fast" not in entry.get("tags", []):
                problems.append(f"manifest.json: {target} is ✓ in §14.3.5 but has no fast tag")
            for name in [bare[index]] if paired else bare:
                if name not in entry.get("aliases", []):
                    problems.append(f"manifest.json: {target} lacks alias {name}")
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument(
        "--all-chapters",
        action="store_true",
        help="grep every chapter for cited fixtures",
    )
    ap.add_argument("--spec-dir", type=Path, default=REPO / "docs" / "spec")
    ap.add_argument("--manifest", type=Path, default=REPO / "tests" / "corpora" / "manifest.json")
    ap.add_argument(
        "--recipes",
        type=Path,
        default=REPO / "tests" / "corpora" / "synthetic" / "recipes.json",
    )
    args = ap.parse_args(argv)
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    recipes = json.loads(args.recipes.read_text(encoding="utf-8"))
    reg = Registry(args.spec_dir, manifest, recipes)
    problems = check_registry(reg, manifest, recipes)
    problems += check_named(manifest, (args.spec_dir / "14-testing.md").read_text(encoding="utf-8"))
    if args.all_chapters:
        for path in sorted(args.spec_dir.glob("*.md")):
            if path.name != "CHANGELOG.md":
                problems += scan_chapter(path, reg)
    for problem in problems:
        print(problem)
    print(f"{len(problems)} problem(s)", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
