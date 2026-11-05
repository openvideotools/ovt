#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Work-item reference checks (chapter 13 WI-0.007; chapter 16 §16.2.1, §16.2.5, §16.2.8).

    check_wi_refs.py --table [docs/spec/16-roadmap-and-work-items.md]
    check_wi_refs.py --make-issues [--dry-run] [--repo owner/name]
    check_wi_refs.py --spec-version [--base origin/main]
    check_wi_refs.py --commit-msg <file>

--table checks the §16.4 dependency table against the chapters (§16.2.5):
  * every WI of §16.4 has exactly one `### WI-...` block, in the chapter its "Chapter §ref" names, and every
    block has a row;
  * every WI id cited anywhere in docs/spec/ and every dependency id exists in §16.4;
  * the "Depends on" column equals the block's **Depends on:** line (ranges expanded);
  * no non-superseded WI depends on a superseded WI, and no chapter 4 §4.17 row cites one;
  * milestone-order rule: milestone(dependency) <= milestone(WI); MA.n takes the ordinal of the first core
    milestone in its title (§16.0);
  * the FRs column is the exact inverse of chapter 4 §4.17;
  * every requirement has a realising WI whose scheduled phase is not later than the requirement's phase.
  It reports requirements no WI claims and prints the open SPEC-ASSUMPTION count per WI (attributed through
  the `WI-x.nnn:` subject of the commit that last touched the line). Exit 1 on any error.

--make-issues creates one tracker issue per §16.4 row titled `WI-x.nnn: <title>` (milestone, phase,
  dependencies and FRs in the body; no labels, so it needs none to exist), skipping ids that already have an
  issue (open or closed), so a second run creates nothing. It uses the `gh` CLI; --dry-run only
  prints what it would create. --issues-json FILE reads the existing issues from a file instead of gh
  (`gh issue list --state all --limit 2000 --json title`), which tests use.

--spec-version fails when docs/spec/** changed against --base without both a version change in
  docs/spec/00-front-matter.md and a new docs/spec/CHANGELOG.md entry (§16.2.8 rule 5).

--commit-msg is the commit-msg hook of .pre-commit-config.yaml: the message must reference a `WI-x.nnn` id
  or an `#issue` (§16.2.3). Merge and fixup commits are exempt.

Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SPEC_DIR = REPO_ROOT / "docs" / "spec"
TABLE_DOC = "16-roadmap-and-work-items.md"
REQ_DOC = "04-requirements.md"

WI_RE = re.compile(r"WI-([0-9A])\.(\d{3})([a-z]?)")
WI_RANGE_RE = re.compile(r"WI-([0-9A])\.(\d{3})\s*[–-]\s*(?:WI-)?(?:[0-9A]\.)?(\d{3})")
REQ_RE = re.compile(r"\b(?:FR|NFR)-[A-Z0-9-]*\d")
BLOCK_RE = re.compile(r"^### (WI-[0-9A]\.\d{3}[a-z]?)\b")
MILESTONE_RE = re.compile(r"\bM([0-9A])\.(\d+)\b")


class CheckError(Exception):
    pass


@dataclass
class Row:
    wi: str
    chapter: str  # "13"
    title: str
    phase: str
    milestone: str
    depends: list[str]
    frs: list[str]
    line: int

    @property
    def superseded(self) -> bool:
        return "superseded" in self.title.lower()


@dataclass
class Block:
    wi: str
    file: str
    line: int
    depends_text: str = ""


@dataclass
class Spec:
    rows: dict[str, Row]
    blocks: dict[str, list[Block]]
    reqs: dict[str, tuple[str, list[str], int]]  # requirement -> (phase, WIs, line)
    milestone_titles: dict[str, str]
    citations: list[tuple[str, int, str]] = field(default_factory=list)  # (file, line, WI id)


def _cells(line: str) -> list[str]:
    return [c.strip() for c in line.strip().strip("|").split("|")]


def wi_ids(text: str) -> list[str]:
    return ["WI-%s.%s%s" % m.groups() for m in WI_RE.finditer(text)]


def expand_ranges(text: str, known: set[str]) -> list[str]:
    """Ids in text, with `WI-1.205–1.216` expanded to the known ids of that span."""
    out: list[str] = []
    for m in WI_RANGE_RE.finditer(text):
        phase, lo, hi = m.group(1), int(m.group(2)), int(m.group(3))
        out += sorted(w for w in known if w.startswith(f"WI-{phase}.") and lo <= int(w[5:8]) <= hi)
    rest = WI_RANGE_RE.sub(" ", text)
    out += wi_ids(rest)
    seen: dict[str, None] = {}
    for w in out:
        seen.setdefault(w)
    return list(seen)


def _strip_parentheses(text: str) -> str:
    while True:
        stripped = re.sub(r"\([^()]*\)", " ", text)
        if stripped == text:
            return text
        text = stripped


def _is_reserved_or_example(line: str, start: int, end: int) -> bool:
    """`e.g. WI-2.014` (an example id), `WI-0.097 reserved` and `WI-0.096–0.099 are reserved` (reserved
    ids) are not citations."""
    return bool(
        re.search(r"e\.g\.\s*`?$", line[max(0, start - 8) : start])
        or re.match(r"^[`)\s–0-9.-]*(are |is )?(re)?reserved\b", line[end : end + 30])
    )


def parse_spec(spec_dir: Path) -> Spec:
    table_path = spec_dir / TABLE_DOC
    rows: dict[str, Row] = {}
    milestone_titles: dict[str, str] = {}
    in_table = False
    for n, line in enumerate(table_path.read_text(encoding="utf-8").splitlines(), start=1):
        if line.startswith("## 16.4"):
            in_table = True
        elif line.startswith("## ") and in_table:
            in_table = False
        m = re.match(r"^\| \*\*(M[0-9A]\.\d+)\*\* \| ([^|]*)\|", line)
        if m:
            milestone_titles[m.group(1)] = m.group(2).strip()
        if in_table and line.startswith("| WI-"):
            c = _cells(line)
            if len(c) != 8:
                raise CheckError(f"{TABLE_DOC}:{n}: §16.4 row has {len(c)} columns, expected 8")
            wi = wi_ids(c[0])[0]
            if wi in rows:
                raise CheckError(f"{TABLE_DOC}:{n}: {wi} has two §16.4 rows")
            chapter = c[1].split()[0]
            rows[wi] = Row(wi, chapter, c[2], c[4], c[5], [], REQ_RE.findall(c[7]), n)
            rows[wi].depends = c[6]  # type: ignore[assignment]  # expanded below, once all ids are known
    known = set(rows)
    for row in rows.values():
        row.depends = [] if str(row.depends).strip().lower() == "none" else expand_ranges(str(row.depends), known)

    blocks: dict[str, list[Block]] = defaultdict(list)
    citations: list[tuple[str, int, str]] = []
    for path in sorted(spec_dir.glob("*.md")):
        current: Block | None = None
        for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            bm = BLOCK_RE.match(line)
            if bm:
                current = Block(bm.group(1), path.name, n)
                blocks[current.wi].append(current)
            elif line.startswith("#"):
                current = None
            elif current is not None and line.startswith("**Depends on:**"):
                current.depends_text = line[len("**Depends on:**") :].strip()
            for cm in WI_RE.finditer(line):
                if not _is_reserved_or_example(line, cm.start(), cm.end()):
                    citations.append((path.name, n, "WI-%s.%s%s" % cm.groups()))

    reqs: dict[str, tuple[str, list[str], int]] = {}
    in_417 = False
    for n, line in enumerate((spec_dir / REQ_DOC).read_text(encoding="utf-8").splitlines(), start=1):
        if line.startswith("## 4.17"):
            in_417 = True
            continue
        if in_417 and line.startswith("## "):
            break
        if in_417 and line.startswith("|"):
            c = _cells(line)
            if len(c) >= 5 and REQ_RE.fullmatch(c[1]):
                reqs[c[1]] = (c[2], expand_ranges(c[4], known), n)
    return Spec(rows, dict(blocks), reqs, milestone_titles, citations)


def milestone_ordinal(milestone: str, titles: dict[str, str]) -> tuple[int, int]:
    """(phase, n) for Mp.n; an MA.n milestone takes the ordinal of the first core milestone in its title."""
    m = MILESTONE_RE.fullmatch(milestone.strip())
    if not m:
        raise CheckError(f"unknown milestone {milestone!r}")
    phase, n = m.groups()
    if phase == "A":
        core = [x for x in MILESTONE_RE.finditer(titles.get(milestone, "")) if x.group(1) != "A"]
        if not core:
            raise CheckError(f"milestone {milestone} names no core milestone in its title")
        return int(core[0].group(1)), int(core[0].group(2))
    return int(phase), int(n)


def _scheduled_phase(row: Row) -> int:
    m = re.match(r"\s*([0-9A])", row.phase)
    if not m or m.group(1) == "A":
        return 0  # track A runs from phase 0 onward
    return int(m.group(1))


def check_table(spec: Spec) -> list[str]:
    errors: list[str] = []
    rows, blocks = spec.rows, spec.blocks
    known = set(rows)
    for wi, row in rows.items():
        bl = blocks.get(wi, [])
        if len(bl) != 1:
            where = ", ".join(f"{b.file}:{b.line}" for b in bl) or "none"
            errors.append(f"{wi}: expected exactly one `### {wi}` block, found {len(bl)} ({where})")
        elif not bl[0].file.startswith(f"{int(row.chapter):02d}-"):
            errors.append(f"{wi}: §16.4 says chapter {row.chapter}, block is in {bl[0].file}:{bl[0].line}")
        for dep in row.depends:
            if dep not in known:
                errors.append(f"{wi}: dependency {dep} has no §16.4 row")
        if len(bl) == 1:
            text = bl[0].depends_text
            # Ids in parentheses only state ownership or context ("the libplacebo path arrives with
            # WI-A.401"); §16.4 does not list them as dependencies.
            block_deps = expand_ranges(_strip_parentheses(text), known)
            if not text:
                errors.append(f"{wi}: {bl[0].file}:{bl[0].line} block has no **Depends on:** line")
            elif sorted(block_deps) != sorted(row.depends):
                errors.append(
                    f"{wi}: Depends on differs: §16.4 {TABLE_DOC}:{row.line} has "
                    f"{', '.join(sorted(row.depends)) or 'none'}; {bl[0].file}:{bl[0].line} has "
                    f"{', '.join(sorted(block_deps)) or 'none'}"
                )
    for wi, bl in blocks.items():
        if wi not in rows:
            errors.append(f"{wi}: block {bl[0].file}:{bl[0].line} has no §16.4 row")
    for file, line, wi in spec.citations:
        if wi not in known:
            errors.append(f"{file}:{line}: cites {wi}, which has no §16.4 row")

    for wi, row in rows.items():
        if row.superseded:
            continue
        for dep in row.depends:
            if dep in rows and rows[dep].superseded:
                errors.append(f"{wi}: depends on superseded {dep}")
        try:
            mine = milestone_ordinal(row.milestone, spec.milestone_titles)
            for dep in row.depends:
                if dep in rows:
                    theirs = milestone_ordinal(rows[dep].milestone, spec.milestone_titles)
                    if theirs > mine:
                        errors.append(
                            f"{wi} ({row.milestone}) depends on {dep} ({rows[dep].milestone}), "
                            "a later milestone (§16.2.5 milestone-order rule)"
                        )
        except CheckError as e:
            errors.append(f"{wi}: {e}")

    inverse: dict[str, set[str]] = defaultdict(set)
    for req, (phase, wis, line) in spec.reqs.items():
        for wi in wis:
            if wi in rows and rows[wi].superseded:
                errors.append(f"{REQ_DOC}:{line}: {req} cites superseded {wi}")
            inverse[wi].add(req)
        live = [w for w in wis if w in rows and not rows[w].superseded]
        if wis and phase.strip().isdigit() and not any(_scheduled_phase(rows[w]) <= int(phase) for w in live):
            errors.append(f"{REQ_DOC}:{line}: {req} (phase {phase}) has no realising WI scheduled by phase {phase}")
    for wi, row in rows.items():
        expected = set() if row.superseded else inverse.get(wi, set())
        if set(row.frs) != expected:
            errors.append(
                f"{wi}: FRs column {', '.join(sorted(row.frs)) or '—'} is not the §4.17 inverse "
                f"{', '.join(sorted(expected)) or '—'}"
            )
    return errors


def unclaimed_requirements(spec: Spec) -> list[str]:
    return sorted(req for req, (_, wis, _) in spec.reqs.items() if not wis)


def spec_assumption_counts(root: Path) -> dict[str, int]:
    """Open SPEC-ASSUMPTION comments per WI, from the subject of the commit that last touched each line."""
    if shutil.which("git") is None:
        return {}
    grep = subprocess.run(
        ["git", "grep", "-nE", r"SPEC-ASSUMPTION\(#[0-9]+\)", "--", ".", ":!docs/spec"],
        cwd=root,
        capture_output=True,
        text=True,
    )
    counts: dict[str, int] = defaultdict(int)
    for hit in grep.stdout.splitlines():
        path, lineno, _ = hit.split(":", 2)
        blame = subprocess.run(
            ["git", "blame", "--porcelain", "-L", f"{lineno},{lineno}", "--", path],
            cwd=root,
            capture_output=True,
            text=True,
        )
        summary = next((ln[8:] for ln in blame.stdout.splitlines() if ln.startswith("summary ")), "")
        ids = wi_ids(summary)
        counts[ids[0] if ids else "unattributed"] += 1
    return dict(counts)


# ---- --make-issues -----------------------------------------------------------------------------------


def existing_issue_ids(issues_json: Path | None, repo: str | None) -> set[str]:
    if issues_json is not None:
        titles = [i["title"] for i in json.loads(issues_json.read_text(encoding="utf-8"))]
    else:
        cmd = ["gh", "issue", "list", "--state", "all", "--limit", "5000", "--json", "title"]
        if repo:
            cmd += ["--repo", repo]
        titles = [
            i["title"] for i in json.loads(subprocess.run(cmd, check=True, capture_output=True, text=True).stdout)
        ]
    found = set()
    for t in titles:
        m = re.match(r"^(WI-[0-9A]\.\d{3}[a-z]?)\b", t)
        if m:
            found.add(m.group(1))
    return found


def make_issues(spec: Spec, dry_run: bool, issues_json: Path | None, repo: str | None) -> int:
    have = existing_issue_ids(issues_json, repo)
    todo = [r for r in spec.rows.values() if r.wi not in have]
    for row in todo:
        title = f"{row.wi}: {re.sub(r'`', '', row.title)}"
        body = (
            f"Chapter {row.chapter} work item, milestone {row.milestone}, phase {row.phase}.\n\n"
            f"Depends on: {', '.join(row.depends) or 'none'}\n\nFRs: {', '.join(row.frs) or '—'}\n\n"
            f"Created by tools/check_wi_refs.py --make-issues from §16.4 ({TABLE_DOC}:{row.line})."
        )
        if dry_run:
            print(f"would create: {title}")
            continue
        cmd = ["gh", "issue", "create", "--title", title, "--body", body]
        if repo:
            cmd += ["--repo", repo]
        subprocess.run(cmd, check=True)
        if issues_json is not None:  # keep a file-backed tracker in step (tests, offline use)
            data = json.loads(issues_json.read_text(encoding="utf-8"))
            data.append({"title": title})
            issues_json.write_text(json.dumps(data), encoding="utf-8")
    print(f"{len(todo)} issue(s) to create, {len(spec.rows) - len(todo)} already exist")
    return 0


# ---- --spec-version --------------------------------------------------------------------------------------


def _git(root: Path, *args: str) -> str:
    return subprocess.run(["git", *args], cwd=root, check=True, capture_output=True, text=True).stdout


def spec_version_of(text: str) -> str:
    m = re.search(r"^\| \*\*Version\*\* \| ([^|]+)\|", text, re.M)
    return m.group(1).strip() if m else ""


def check_spec_version(root: Path, base: str) -> list[str]:
    merge_base = _git(root, "merge-base", base, "HEAD").strip()
    changed = [p for p in _git(root, "diff", "--name-only", merge_base, "--", "docs/spec").splitlines() if p]
    if not changed:
        return []
    front = "docs/spec/00-front-matter.md"
    old = _git(root, "show", f"{merge_base}:{front}")
    new = (root / front).read_text(encoding="utf-8")
    errors = []
    if spec_version_of(old) == spec_version_of(new):
        errors.append(
            f"docs/spec changed ({', '.join(changed)}) but the version in {front} is still "
            f"{spec_version_of(new)!r} (§16.2.8 rule 5)"
        )
    log = "docs/spec/CHANGELOG.md"
    old_heads = set(re.findall(r"^## .+$", _git(root, "show", f"{merge_base}:{log}"), re.M))
    new_heads = set(re.findall(r"^## .+$", (root / log).read_text(encoding="utf-8"), re.M))
    if not new_heads - old_heads:
        errors.append(f"docs/spec changed but {log} has no new `## <version> — <date>` entry (§16.2.8 rule 2)")
    return errors


# ---- --commit-msg ----------------------------------------------------------------------------------------


def check_commit_msg(text: str) -> str | None:
    lines = [ln for ln in text.splitlines() if not ln.startswith("#")]
    subject = lines[0] if lines else ""
    if re.match(r"^(Merge |fixup! |squash! |amend! |Revert \")", subject):
        return None
    body = "\n".join(lines)
    if WI_RE.search(body) or re.search(r"(?<![\w&])#\d+\b", body):
        return None
    return (
        "commit message references no work item: start the subject with the WI id "
        "(`WI-0.007: ...`) or mention an issue (`#123`) (chapter 16 §16.2.3)"
    )


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument(
        "--table",
        nargs="?",
        const=str(SPEC_DIR / TABLE_DOC),
        metavar="CHAPTER16",
        help="check §16.4 against the chapters",
    )
    mode.add_argument("--make-issues", action="store_true", help="one tracker issue per §16.4 row")
    mode.add_argument("--spec-version", action="store_true", help="spec change needs a version bump")
    mode.add_argument("--commit-msg", type=Path, metavar="FILE", help="commit-msg hook")
    ap.add_argument("--dry-run", action="store_true", help="--make-issues: print, create nothing")
    ap.add_argument("--issues-json", type=Path, help="--make-issues: existing issues from this file")
    ap.add_argument("--repo", help="--make-issues: owner/name for gh")
    ap.add_argument("--base", default="origin/main", help="--spec-version: compare against this ref")
    ap.add_argument("--root", type=Path, default=REPO_ROOT, help="repository root")
    args = ap.parse_args(argv)
    try:
        if args.commit_msg:
            problem = check_commit_msg(args.commit_msg.read_text(encoding="utf-8"))
            if problem:
                print(f"error: {problem}", file=sys.stderr)
                return 1
            return 0
        if args.spec_version:
            errors = check_spec_version(args.root, args.base)
            for e in errors:
                print(f"error: {e}", file=sys.stderr)
            return 1 if errors else 0
        spec_dir = Path(args.table).parent if args.table else args.root / "docs" / "spec"
        spec = parse_spec(spec_dir)
        if args.make_issues:
            return make_issues(spec, args.dry_run, args.issues_json, args.repo)
        errors = check_table(spec)
        for e in errors:
            print(f"error: {e}", file=sys.stderr)
        unclaimed = unclaimed_requirements(spec)
        if unclaimed:
            print(f"requirements no WI claims ({len(unclaimed)}): {', '.join(unclaimed)}")
        counts = spec_assumption_counts(args.root)
        if counts:
            print("open SPEC-ASSUMPTION comments: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())))
        print(f"§16.4: {len(spec.rows)} rows, {len(spec.reqs)} requirements, {len(errors)} error(s)")
        return 1 if errors else 0
    except (CheckError, OSError, subprocess.CalledProcessError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
