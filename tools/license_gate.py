#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Licence gate (chapter 15 §15.4.2; WI-0.602).

    license_gate.py --policy tools/license_policy.toml [--versions third_party/VERSIONS.md]
        [--vcpkg <vcpkg_installed/triplet>] [--manifest build/<preset>/license_manifest.json]
        [--gradle <index.json>] [--pip <requirements.txt|pip-licenses.json> ...]
        [--release-manifest tools/release/manifest.toml] [--sources] [--require-all] [--strict-or]
        [--export-table <md>] [--report <json>] [--root <repo>]
    license_gate.py --policy tools/license_policy.toml --corpora [--manifest tests/corpora/manifest.json]
        [--write-licenses-md] [--report <json>]

Findings (one per line on stderr; any finding makes the run FAIL, exit 1):
    E-DENY <where> <id>            id matches [deny]
    E-UNKNOWN <where> <id>         id (or expression) not in any allow list, or unparseable
    E-LINK <target> <lib> <id>     licence not allowed in the target's link graph
    E-TARGET <target> <message>    target licence differs from, or is missing in, [targets]
    E-HEADER / E-NOHEADER <file>   source header vs [directories] (tools/check_spdx_dirs.py logic)
    E-DIST <artefact> <message>    release-manifest distribution rule (ADR-015)
    E-EXPIRED <where> <id>         an [[exceptions]] entry is past its `expires` date
    E-DENY <corpora:id> <spdx>     (--corpora) corpus licence matches [deny] (rule 4 of chapter 14 §14.3.4)
    E-CORPUS <corpora:id> <spdx>   (--corpora) `redistributable: true` with a licence outside [corpora] (rule 2)
    E-STALE <path>                 (--corpora) tests/corpora/LICENSES.md differs from the generated text (rule 5)
Warnings (W-*) never fail the run. Exit 2 is a usage error, an unreadable policy, or, with --require-all,
an input flag whose file does not exist (`missing required input: <name>`).

`--corpora` (WI-0.561) is a separate mode: it checks only the corpora manifest (`--manifest` then names
tests/corpora/manifest.json, not a license_manifest.json) and never mixes with the dependency inputs. Exit 0 clean,
1 violation, 4 manifest unreadable or missing a required field.

The module is importable (`license_gate.resolve`, `license_gate.inventory`; used by WI-0.036 and WI-0.603).
Run with `python -I`; the standard library only.
"""

from __future__ import annotations

import argparse
import datetime
import json
import re
import subprocess
import sys
import tomllib
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parent))  # `python -I` drops the script directory

import check_spdx_dirs  # noqa: E402
import spdx_header  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
KEYWORDS = {"AND", "OR", "WITH"}


class GateError(Exception):
    """A licence problem; `code` is the finding code."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


class UsageError(Exception):
    pass


class ManifestInvalid(Exception):
    """The corpora manifest is unreadable or malformed (exit 4)."""


# --- SPDX expressions -------------------------------------------------------------------------------------------

Node = tuple  # ("id", str) | ("and", [Node]) | ("or", [Node])


def tokenize(expr: str) -> list[str]:
    return re.findall(r"\(|\)|[^\s()]+", expr)


def parse(expr: str) -> Node:
    """Parse an SPDX licence expression: `WITH` binds tighter than `AND`, which binds tighter than `OR`.

    A `WITH` expression is a single id ("GPL-3.0-only WITH Bison-exception-2.2") for every later step.
    """
    toks = tokenize(expr)
    if not toks:
        raise GateError("E-UNKNOWN", "empty licence expression")
    pos = 0

    def peek() -> str | None:
        return toks[pos] if pos < len(toks) else None

    def keyword(tok: str | None) -> str | None:
        return tok.upper() if tok is not None and tok.upper() in KEYWORDS else None

    def take() -> str:
        nonlocal pos
        tok = toks[pos]
        pos += 1
        return tok

    def p_or() -> Node:
        items = [p_and()]
        while keyword(peek()) == "OR":
            take()
            items.append(p_and())
        return items[0] if len(items) == 1 else ("or", items)

    def p_and() -> Node:
        items = [p_with()]
        while keyword(peek()) == "AND":
            take()
            items.append(p_with())
        return items[0] if len(items) == 1 else ("and", items)

    def p_with() -> Node:
        parenthesised = peek() == "("
        node = p_atom()
        if keyword(peek()) == "WITH":
            take()
            if parenthesised or node[0] != "id" or peek() is None or peek() == "(" or keyword(peek()) or peek() == ")":
                raise GateError("E-UNKNOWN", f"bad WITH in {expr!r}")
            node = ("id", f"{node[1]} WITH {take()}")
        return node

    def p_atom() -> Node:
        tok = peek()
        if tok is None or tok == ")" or keyword(tok):
            raise GateError("E-UNKNOWN", f"unexpected {tok or 'end'} in {expr!r}")
        take()
        if tok == "(":
            node = p_or()
            if peek() != ")":
                raise GateError("E-UNKNOWN", f"missing ) in {expr!r}")
            take()
            return node
        return ("id", tok)

    node = p_or()
    if pos != len(toks):
        raise GateError("E-UNKNOWN", f"unexpected {toks[pos]!r} in {expr!r}")
    return node


def render(node: Node) -> str:
    if node[0] == "id":
        return node[1]
    sep = f" {node[0].upper()} "
    parts = [render(c) if c[0] == "id" else f"({render(c)})" for c in node[1]]
    return sep.join(parts)


def ids_of(node: Node) -> list[str]:
    """Every id in the expression, alternatives of OR included."""
    if node[0] == "id":
        return [node[1]]
    return [i for c in node[1] for i in ids_of(c)]


# --- policy -----------------------------------------------------------------------------------------------------


def load_policy(path: Path) -> dict:
    try:
        with path.open("rb") as f:
            policy = tomllib.load(f)
    except (OSError, tomllib.TOMLDecodeError) as e:
        raise UsageError(f"cannot read policy {path}: {e}") from e
    if policy.get("schema_version") != 1:
        raise UsageError(f"{path}: schema_version must be 1")
    for table in ("deny", "allow", "targets", "directories"):
        if not isinstance(policy.get(table), dict):
            raise UsageError(f"{path}: missing [{table}] table")
    if not isinstance(policy["deny"].get("patterns"), list) or not isinstance(
        policy["allow"].get("permissiveness"), list
    ):
        raise UsageError(f"{path}: [deny] patterns and [allow] permissiveness are required")
    for ex in policy.get("exceptions", []):
        for key in ("id", "where", "reason", "expires"):
            if key not in ex:
                raise UsageError(f"{path}: [[exceptions]] entry without `{key}`")
        try:
            datetime.date.fromisoformat(str(ex["expires"]))
        except ValueError as e:
            raise UsageError(f"{path}: exception for {ex['id']}: bad expires date") from e
    return policy


def allow_ids(policy: dict, *classes: str) -> set[str]:
    allow = policy["allow"]
    return {i for c in classes for i in allow.get(c, {}).get("ids", [])}


def all_allowed(policy: dict) -> set[str]:
    return allow_ids(policy, "any", "gpl_targets", "plugin_targets")


def is_denied(ident: str, policy: dict) -> str | None:
    for pat in policy["deny"]["patterns"]:
        if re.search(pat, ident, re.I):
            return pat
    return None


def class_allowed(own: str, policy: dict, plugin: bool = False) -> set[str] | None:
    """Ids a target whose own licence is `own` may link; None when the licence has no class."""
    base = allow_ids(policy, "any")
    if plugin:
        if own not in allow_ids(policy, "plugin_targets"):
            return None
        if own.startswith(("BSD", "MIT", "ISC")):
            return {i for i in base if not i.startswith(("LGPL", "GPL"))}
        if own.startswith("LGPL-3"):
            return base | {i for i in allow_ids(policy, "gpl_targets") if i.startswith("LGPL-3")}
        if own.startswith("LGPL"):
            return base
        return base | allow_ids(policy, "gpl_targets")  # a GPL plugin may link anything allowed
    if own.startswith("LGPL"):
        return base
    if own.startswith("GPL"):
        return base | allow_ids(policy, "gpl_targets")
    return None


def resolve_node(node: Node, policy: dict, allowed: set[str]) -> tuple[Node, int]:
    """Choose alternatives; returns (chosen node, rank). Rank is the least permissive id's index."""
    perm = policy["allow"]["permissiveness"]
    if node[0] == "id":
        if node[1] not in allowed or node[1] not in perm:
            raise GateError("E-UNKNOWN", node[1])
        return node, perm.index(node[1])
    if node[0] == "and":
        parts = [resolve_node(c, policy, allowed) for c in node[1]]
        return ("and", [p[0] for p in parts]), max(p[1] for p in parts)
    best = None
    for alt in node[1]:
        try:
            cand = resolve_node(alt, policy, allowed)
        except GateError:
            continue
        if best is None or cand[1] < best[1]:
            best = cand
    if best is None:
        raise GateError("E-UNKNOWN", render(node))
    return best


def resolve(expr: str, policy: dict, allowed: set[str] | None = None) -> str:
    """The licence the gate chooses for an SPDX expression: the `OR` alternative that comes first in
    `[allow].permissiveness` and is allowed (default: any class). Raises GateError(E-UNKNOWN) when none."""
    allowed = all_allowed(policy) if allowed is None else allowed
    return render(resolve_node(parse(expr), policy, allowed)[0])


# --- findings and inventory -------------------------------------------------------------------------------------


@dataclass
class Report:
    findings: list[dict] = field(default_factory=list)
    warnings: list[dict] = field(default_factory=list)
    inventory: list[dict] = field(default_factory=list)
    inputs: dict[str, str] = field(default_factory=dict)

    def add(self, code: str, where: str, lic: str, message: str) -> None:
        self.findings.append({"code": code, "where": where, "license": lic, "message": message})

    def warn(self, code: str, where: str, message: str) -> None:
        self.warnings.append({"code": code, "where": where, "license": "", "message": message})


def check_expression(
    rep: Report,
    policy: dict,
    where: str,
    expr: str,
    allowed: set[str],
    strict_or: bool,
    code: str = "E-UNKNOWN",
    link: str | None = None,
) -> str | None:
    """Deny/unknown checks of one expression; returns the chosen expression or None on a finding."""
    try:
        node = parse(expr)
    except GateError as e:
        rep.add("E-UNKNOWN", where, expr, f"unparseable licence expression: {e}")
        return None
    denied = False
    for ident in ids_of(node):
        if is_denied(ident, policy) and (strict_or or _chosen_ids_contain(node, policy, ident, allowed)):
            rep.add("E-DENY", where, ident, f"{ident} is forbidden by the deny list")
            denied = True
    if denied:
        return None
    try:
        return render(resolve_node(node, policy, allowed)[0])
    except GateError as e:
        # an id that is not allowed in this class: E-UNKNOWN when it is allowed nowhere, else the caller's code
        everywhere = all_allowed(policy)
        known = all(i in everywhere for i in ids_of(node))
        msg = str(e)
        if code == "E-LINK" and known:
            rep.add("E-LINK", f"{where} {link}" if link else where, expr, f"{expr} is not allowed here")
        else:
            rep.add(
                "E-UNKNOWN",
                f"{where} {link}" if link else where,
                expr if known else msg,
                f"licence not in any allow list: {msg}",
            )
        return None


def _chosen_ids_contain(node: Node, policy: dict, ident: str, allowed: set[str]) -> bool:
    try:
        return ident in ids_of(resolve_node(node, policy, allowed)[0])
    except GateError:
        # no alternative is choosable, so a denied id in the expression is reported as denied
        return True


def inventory(report: dict) -> list[dict]:
    """The inventory of a gate report (the JSON object or a path-loaded dict); input of gen_notices.py."""
    return list(report["inventory"])


# --- inputs -----------------------------------------------------------------------------------------------------


def split_row(line: str) -> list[str]:
    return [c.strip() for c in line.strip().strip("|").split("|")]


def read_versions(path: Path, rep: Report, policy: dict, strict_or: bool) -> None:
    lines = [ln for ln in path.read_text(encoding="utf-8").splitlines() if ln.lstrip().startswith("|")]
    if len(lines) < 3:
        rep.add("E-UNKNOWN", str(path), "", "no table found")
        return
    header = [c.lower() for c in split_row(lines[0])]
    try:
        i_name, i_ver, i_lic = header.index("name"), header.index("version"), header.index("licence")
    except ValueError:
        rep.add("E-UNKNOWN", str(path), "", "table needs Name, Version and Licence columns")
        return
    skip = set(policy.get("versions_skip", {}).get("names", []))
    overrides = policy.get("versions_overrides", {})
    allowed = all_allowed(policy)
    for line in lines[2:]:
        cells = split_row(line)
        if len(cells) <= max(i_name, i_ver, i_lic):
            continue
        name, version, cell = cells[i_name], cells[i_ver].strip("`"), cells[i_lic].strip("`")
        where = f"third_party/VERSIONS.md:{name}"
        if name in skip or cell in ("", "-", "–"):
            continue
        expr = overrides.get(name)  # a reviewed override always wins over the cell
        for cand in () if expr else (cell, re.sub(r"\s*\([^)]*\)", "", cell).strip()):
            try:
                parse(cand)
                expr = cand
                break
            except GateError:
                continue
        if expr is None:
            rep.add(
                "E-UNKNOWN",
                where,
                cell,
                f"Licence cell is not an SPDX expression and {name} has no [versions_overrides] entry",
            )
            continue
        chosen = check_expression(rep, policy, where, expr, allowed, strict_or)
        rep.inventory.append(
            {
                "component": name,
                "version": version,
                "license": expr,
                "chosen": chosen,
                "source": "versions",
                "targets": [],
            }
        )


COPYRIGHT_DENY = (
    # Title lines only: the GPL-3.0 text mentions the AGPL in its section 13, and permissive texts say "commercial
    # or non-commercial use", so a loose match would deny every GPL-3 port.
    (re.compile(r"^\s*GNU AFFERO GENERAL PUBLIC LICENSE\s*$", re.M), "AGPL-3.0-only"),
    (re.compile(r"^\s*Server Side Public License\s*$", re.M), "SSPL-1.0"),
    (re.compile(r"^\s*Business Source License 1\.1\s*$", re.M), "BUSL-1.1"),
)


# vcpkg writes LicenseRef-vcpkg-null for a port manifest with `"license": null`.
NO_LICENCE_VALUES = ("NOASSERTION", "NONE", "LicenseRef-vcpkg-null")


def read_vcpkg(root: Path, rep: Report, policy: dict, strict_or: bool) -> None:
    share = root / "share"
    overrides = policy.get("vcpkg_overrides", {})
    allowed = all_allowed(policy)
    for port_dir in sorted(p for p in share.iterdir() if p.is_dir()) if share.is_dir() else []:
        port = port_dir.name
        spdx_file, copyright_file = port_dir / "vcpkg.spdx.json", port_dir / "copyright"
        if not spdx_file.is_file() and not copyright_file.is_file():
            continue
        where = f"vcpkg:{port}"
        expr, version = None, ""
        if spdx_file.is_file():
            try:
                data = json.loads(spdx_file.read_text(encoding="utf-8"))
            except json.JSONDecodeError as e:
                rep.add("E-UNKNOWN", where, "", f"bad vcpkg.spdx.json: {e}")
                continue
            for pkg in data.get("packages", []):
                version = version or str(pkg.get("versionInfo", ""))
                for key in ("licenseConcluded", "licenseDeclared"):
                    val = pkg.get(key)
                    if expr is None and val and val not in NO_LICENCE_VALUES:
                        expr = val
        if expr is None and copyright_file.is_file():  # heuristics only when the SPDX file gives no licence
            text = copyright_file.read_text(encoding="utf-8", errors="replace")
            for rx, ident in COPYRIGHT_DENY:  # heuristics only ever flag; they never grant a licence
                if rx.search(text):
                    rep.add("E-DENY", where, ident, f"copyright file of {port} mentions {rx.pattern!r}")
        if expr is None:
            expr = overrides.get(port)
        if expr is None:
            rep.add(
                "E-UNKNOWN",
                where,
                "NOASSERTION",
                f"{port}: no licence in vcpkg.spdx.json; add a [vcpkg_overrides] entry",
            )
            continue
        chosen = check_expression(rep, policy, where, expr, allowed, strict_or)
        rep.inventory.append(
            {"component": port, "version": version, "license": expr, "chosen": chosen, "source": "vcpkg", "targets": []}
        )


def read_gradle(path: Path, rep: Report, policy: dict, strict_or: bool) -> None:
    data = json.loads(path.read_text(encoding="utf-8"))
    names = policy.get("gradle_license_names", {})
    allowed = all_allowed(policy)
    if data.get("importedModules"):
        rep.add("E-UNKNOWN", "gradle", "", "importedModules must be empty")
    for dep in data.get("dependencies", []):
        name = f"{dep.get('moduleName', '?')}"
        where = f"gradle:{name}"
        raw = dep.get("moduleLicense")
        if raw is None:
            lics = [x.get("moduleLicense") for x in dep.get("moduleLicenses", []) if x.get("moduleLicense")]
            raw = " AND ".join(names.get(x, x) for x in lics) if lics else None
        if not raw:
            rep.add("E-UNKNOWN", where, "", "dependency without a licence")
            continue
        expr = names.get(raw, raw)
        chosen = check_expression(rep, policy, where, expr, allowed, strict_or)
        rep.inventory.append(
            {
                "component": name,
                "version": str(dep.get("moduleVersion", "")),
                "license": expr,
                "chosen": chosen,
                "source": "gradle",
                "targets": [],
            }
        )


def pip_metadata_license(name: str) -> str | None:
    from importlib import metadata

    try:
        md = metadata.metadata(name)
    except metadata.PackageNotFoundError:
        return None
    lic = md["License-Expression"] if "License-Expression" in md else (md["License"] if "License" in md else None)
    return lic if lic and len(lic) <= 100 and "\n" not in lic else None


def read_pip(path: Path, rep: Report, policy: dict, strict_or: bool) -> None:
    """Either `pip-licenses --format=json` output, or a requirements file resolved through installed metadata."""
    allowed = all_allowed(policy)
    overrides = policy.get("pip_overrides", {})
    entries: list[tuple[str, str, str | None]] = []
    if path.suffix == ".json":
        for row in json.loads(path.read_text(encoding="utf-8")):
            entries.append((row.get("Name", "?"), row.get("Version", ""), row.get("License")))
    else:
        for line in path.read_text(encoding="utf-8").splitlines():
            line = line.split("#", 1)[0].strip()
            m = re.match(r"([A-Za-z0-9_.-]+)\s*(?:\[[^\]]*\])?\s*(?:==\s*([^\s;]+))?", line)
            if not m or line.startswith("-"):
                continue
            entries.append(
                (m.group(1), m.group(2) or "", overrides.get(m.group(1)) or pip_metadata_license(m.group(1)))
            )
    for name, version, lic in entries:
        where = f"pip:{name}"
        lic = overrides.get(name) or lic
        if not lic or lic == "UNKNOWN":
            rep.warn("W-PIP-UNRESOLVED", where, f"{name}: licence not found in installed metadata; add [pip_overrides]")
            continue
        chosen = check_expression(rep, policy, where, lic, allowed, strict_or)
        rep.inventory.append(
            {"component": name, "version": version, "license": lic, "chosen": chosen, "source": "pip", "targets": []}
        )


def is_test_target(tgt: dict) -> bool:
    """True for a target defined under a `tests` or `test` directory of the repository (manifest `directory`)."""
    return bool({"tests", "test"} & set(PurePosixPath(tgt.get("directory", "")).parts))


def vcpkg_file_ports(root: Path) -> dict[str, str]:
    """Installed file (relative to the directory that holds the triplet directories) -> vcpkg port, from the `.list`
    files vcpkg keeps in `<installed>/vcpkg/info/`."""
    ports: dict[str, str] = {}
    for lst in sorted((root.parent / "vcpkg" / "info").glob("*.list")):
        port = lst.name.split("_", 1)[0]
        for line in lst.read_text(encoding="utf-8", errors="replace").splitlines():
            ports[line.strip()] = port
    return ports


def vcpkg_port_licence(root: Path, port: str, policy: dict) -> str | None:
    """The licence of an installed vcpkg port: its vcpkg.spdx.json, else the reviewed [vcpkg_overrides] entry."""
    spdx_file = root / "share" / port / "vcpkg.spdx.json"
    if spdx_file.is_file():
        try:
            packages = json.loads(spdx_file.read_text(encoding="utf-8")).get("packages", [])
        except json.JSONDecodeError:
            packages = []
        for pkg in packages:
            for key in ("licenseConcluded", "licenseDeclared"):
                val = pkg.get(key)
                if val and val not in NO_LICENCE_VALUES:
                    return str(val)
    return policy.get("vcpkg_overrides", {}).get(port)


def read_manifest(path: Path, rep: Report, policy: dict, strict_or: bool, vcpkg_root: Path | None = None) -> None:
    """Link-graph rules of §15.4.2 step 4 on `license_manifest.json` (chapter 13 §13.2.3).

    A link that names an installed vcpkg file by absolute path (libglslang.a behind tp::placebo) and has no licence of
    its own takes the licence of the port that installed the file; this needs --vcpkg."""
    data = json.loads(path.read_text(encoding="utf-8"))
    declared = policy["targets"]
    vcpkg_root = vcpkg_root.resolve() if vcpkg_root else None
    file_ports = vcpkg_file_ports(vcpkg_root) if vcpkg_root else {}
    seen: dict[tuple[str, str, str], dict] = {}
    for tgt in data.get("targets", []):
        name, own = tgt["name"], tgt["spdx"]
        plugin = name.startswith("ovt_plugin_")
        # SPEC-ASSUMPTION(#432): a target built under a `tests/` directory is not shipped, so it needs no [targets] row
        # (§15.4.2 step 1 asks for one per target). Its own licence and its whole link graph are still checked.
        if name not in declared:
            if not is_test_target(tgt):
                rep.add("E-TARGET", name, own, f"target {name} is not in policy [targets]")
        elif declared[name] != own:
            rep.add("E-TARGET", name, own, f"{name} is declared {own} but policy [targets] says {declared[name]}")
        allowed = class_allowed(own, policy, plugin)
        if allowed is None:
            rep.add("E-TARGET", name, own, f"{own} is not an allowed own licence for {name}")
            continue
        for lib in tgt.get("links", []):
            lname, expr = lib["name"], lib["spdx"]
            if vcpkg_root and expr in NO_LICENCE_VALUES and lname.startswith("/"):
                try:
                    rel = Path(lname).relative_to(vcpkg_root.parent).as_posix()
                except ValueError:
                    rel = ""
                port = file_ports.get(rel)
                expr = (vcpkg_port_licence(vcpkg_root, port, policy) if port else None) or expr
            chosen = check_expression(rep, policy, name, expr, allowed, strict_or, code="E-LINK", link=lname)
            key = (lname, str(lib.get("version", "")), lib.get("source", ""))
            row = seen.setdefault(
                key,
                {
                    "component": lname,
                    "version": key[1],
                    "license": expr,
                    "chosen": None,
                    "source": key[2] or "manifest",
                    "targets": [],
                },
            )
            row["chosen"] = row["chosen"] or chosen
            if name not in row["targets"]:
                row["targets"].append(name)
    rep.inventory.extend(seen.values())


def list_files(root: Path) -> list[str]:
    try:
        return check_spdx_dirs.tracked_files(root)
    except (subprocess.CalledProcessError, OSError):
        return sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file() and ".git" not in p.parts)


def read_sources(root: Path, rep: Report, policy_path: Path) -> None:
    dirs = spdx_header.load_directories(policy_path)
    for line in check_spdx_dirs.check(list_files(root), root, dirs):
        parts = line.split()
        if parts[0] == "E-HEADER":
            rep.add("E-HEADER", parts[1], parts[5], f"expected {parts[3]} got {parts[5]}")
        else:
            rep.add("E-NOHEADER", parts[1], "", "no SPDX-License-Identifier header")


def read_release_manifest(path: Path, rep: Report, policy: dict) -> None:
    """Distribution rules of §15.4.2 step 6 (ADR-015)."""
    data = tomllib.loads(path.read_text(encoding="utf-8"))
    dist = policy.get("distribution", {})
    forbidden = set(dist.get("default_download_forbidden_plugins", []))
    required = dist.get("required_notices", [])
    licences = data.get("licenses", {})
    for art_id, art in sorted(data.get("artifacts", {}).items()):
        comps = set(art.get("components", []))
        listed = set(licences.get(art_id, {}).get("notices", []))
        embedded = bool(art.get("notices"))
        if art.get("default_download"):
            for bad in sorted(comps & forbidden):
                rep.add("E-DIST", art_id, bad, f"default download contains {bad} (ADR-015)")
        if art.get("codec_pack") and art.get("default_download"):
            rep.add("E-DIST", art_id, "", "a codec pack must have default_download = false")
        needs = ["THIRD_PARTY_NOTICES.md"]
        if comps & {"ovt_plugin_aom", "libaom"}:
            needs.append("LICENSES/LicenseRef-AOM-Patent-License-1.0.txt")
        for notice in needs:
            if notice in required and not embedded and notice not in listed:
                rep.add("E-DIST", art_id, "", f"missing required notice {notice}")


# --- corpora (chapter 14 §14.3.4; WI-0.561) -----------------------------------------------------------------------

CORPORA_REQUIRED = ("id", "licence", "licence_spdx", "redistributable")
CORPORA_MANIFEST = Path("tests/corpora/manifest.json")


def load_corpora(path: Path) -> list[dict]:
    """Rule 1: every entry has `licence`, `licence_spdx` and `redistributable` (a boolean). Raises ManifestInvalid."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        raise ManifestInvalid(f"cannot read corpora manifest {path}: {e}") from e
    entries = data.get("entries") if isinstance(data, dict) else None
    if not isinstance(entries, list):
        raise ManifestInvalid(f"{path}: `entries` must be a list")
    for i, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise ManifestInvalid(f"{path}: entry {i} is not an object")
        label = entry.get("id", f"#{i}")
        for key in CORPORA_REQUIRED:
            if key not in entry:
                raise ManifestInvalid(f"{path}: entry {label} has no `{key}`")
        if not isinstance(entry["redistributable"], bool):
            raise ManifestInvalid(f"{path}: entry {label}: `redistributable` must be a boolean")
        for key in ("id", "licence", "licence_spdx"):
            if not isinstance(entry[key], str) or not entry[key]:
                raise ManifestInvalid(f"{path}: entry {label}: `{key}` must be a non-empty string")
    return entries


def check_corpora(entries: list[dict], rep: Report, policy: dict) -> None:
    """Rules 2 and 4 of §14.3.4. Rule 3 (non-redistributable media stay out of artefacts) is a packaging rule."""
    allowed = set(policy.get("corpora", {}).get("redistributable_spdx", []))
    for e in entries:
        where, spdx = f"corpora:{e['id']}", e["licence_spdx"]
        pattern = is_denied(spdx, policy)
        if pattern:
            rep.add("E-DENY", where, spdx, f"licence matches deny pattern {pattern}")
        elif e["redistributable"] and spdx not in allowed:
            rep.add("E-CORPUS", where, spdx, "redistributable=true needs a licence in policy [corpora]")


def licenses_md(entries: list[dict]) -> str:
    """The text of tests/corpora/LICENSES.md (rule 5): deterministic, sorted by id, no timestamps."""
    esc = lambda t: str(t).replace("|", "\\|").replace("\n", " ")  # noqa: E731
    rows = [
        "# Corpora licences",
        "",
        "Generated by `tools/license_gate.py --corpora --write-licenses-md` from `manifest.json`. Do not edit by hand:",
        "the licence gate fails when this file is stale.",
        "",
        "| Id | Source | Licence | SPDX | Redistributable |",
        "|---|---|---|---|---|",
    ]
    for e in sorted(entries, key=lambda x: x["id"]):
        cells = [e["id"], e.get("source", ""), e["licence"], e["licence_spdx"], "yes" if e["redistributable"] else "no"]
        rows.append("| " + " | ".join(esc(c) for c in cells) + " |")
    return "\n".join(rows) + "\n"


def run_corpora(args: argparse.Namespace) -> tuple[dict, Report]:
    policy = load_policy(args.policy)
    if not isinstance(policy.get("corpora", {}).get("redistributable_spdx"), list):
        raise UsageError(f"{args.policy}: missing [corpora] redistributable_spdx")
    manifest = args.manifest or (args.root / CORPORA_MANIFEST)
    rep = Report()
    entries = load_corpora(manifest)
    check_corpora(entries, rep, policy)
    expected = licenses_md(entries)
    target = manifest.parent / "LICENSES.md"
    if args.write_licenses_md:
        target.write_text(expected, encoding="utf-8")
    elif not target.exists() or target.read_text(encoding="utf-8") != expected:
        rep.add(
            "E-STALE", str(target), "", f"{target.name} is stale; run license_gate.py --corpora --write-licenses-md"
        )
    rep.inputs["corpora"] = "read"
    apply_exceptions(rep, policy, datetime.date.fromisoformat(args.today) if args.today else datetime.date.today())
    return finish(rep), rep


# --- exceptions and output --------------------------------------------------------------------------------------


def apply_exceptions(rep: Report, policy: dict, today: datetime.date) -> None:
    live = []
    for ex in policy.get("exceptions", []):
        if datetime.date.fromisoformat(str(ex["expires"])) < today:
            rep.add("E-EXPIRED", ex["where"], ex["id"], f"exception for {ex['id']} expired on {ex['expires']}")
        else:
            live.append(ex)
    rep.findings = [
        f
        for f in rep.findings
        if f["code"] == "E-EXPIRED"
        or not any(f["license"] == ex["id"] and f["where"].startswith(ex["where"]) for ex in live)
    ]


def export_table(rep: Report) -> str:
    rows = ["| Component | Version | Source | Licence | Chosen | Targets |", "|---|---|---|---|---|---|"]
    for e in rep.inventory:
        cells = [e["component"], e["version"], e["source"], e["license"], e["chosen"] or "", ", ".join(e["targets"])]
        rows.append("| " + " | ".join(c.replace("|", "\\|") for c in cells) + " |")
    return "\n".join(rows) + "\n"


def finish(rep: Report) -> dict:
    key = lambda f: (f["code"], f["where"], f["license"], f["message"])  # noqa: E731
    rep.findings = sorted({json.dumps(f, sort_keys=True): f for f in rep.findings}.values(), key=key)
    rep.warnings.sort(key=key)
    rep.inventory.sort(key=lambda e: (e["component"].lower(), e["version"], e["source"]))
    out = {
        "status": "FAIL" if rep.findings else "PASS",
        "findings": rep.findings,
        "warnings": rep.warnings,
        "inputs": dict(sorted(rep.inputs.items())),
        "inventory": rep.inventory,
    }
    return out


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0], prog="license_gate.py")
    ap.add_argument("--policy", type=Path, required=True)
    ap.add_argument("--versions", type=Path)
    ap.add_argument("--vcpkg", type=Path)
    ap.add_argument("--manifest", type=Path)
    ap.add_argument("--gradle", type=Path)
    ap.add_argument("--pip", type=Path, nargs="+")
    ap.add_argument("--release-manifest", type=Path)
    ap.add_argument("--sources", action="store_true")
    ap.add_argument("--require-all", action="store_true", help="an input flag whose file is missing is an error")
    ap.add_argument("--strict-or", action="store_true", help="deny-check every OR alternative, not only the chosen one")
    ap.add_argument("--export-table", type=Path)
    ap.add_argument("--report", type=Path, help="default: license_gate_report.json next to --manifest")
    ap.add_argument("--root", type=Path, default=REPO_ROOT, help="repository root (for --sources)")
    ap.add_argument("--corpora", action="store_true", help="check the corpora manifest only (chapter 14 §14.3.4)")
    ap.add_argument(
        "--write-licenses-md", action="store_true", help="with --corpora: rewrite tests/corpora/LICENSES.md"
    )
    ap.add_argument("--today", help=argparse.SUPPRESS)  # YYYY-MM-DD; tests only
    return ap


def run(args: argparse.Namespace) -> tuple[dict, Report]:
    policy = load_policy(args.policy)
    today = datetime.date.fromisoformat(args.today) if args.today else datetime.date.today()
    rep = Report()
    inputs = [
        ("versions", args.versions, lambda p: read_versions(p, rep, policy, args.strict_or)),
        ("vcpkg", args.vcpkg, lambda p: read_vcpkg(p, rep, policy, args.strict_or)),
        ("manifest", args.manifest, lambda p: read_manifest(p, rep, policy, args.strict_or, args.vcpkg)),
        ("gradle", args.gradle, lambda p: read_gradle(p, rep, policy, args.strict_or)),
        ("pip", args.pip, None),
        ("release-manifest", args.release_manifest, lambda p: read_release_manifest(p, rep, policy)),
    ]
    for name, value, reader in inputs:
        paths = value if isinstance(value, list) else ([value] if value else [])
        if not paths:
            rep.inputs[name] = "skipped"
            continue
        for p in paths:
            if not p.exists():
                if args.require_all:
                    raise UsageError(f"missing required input: {name}")
                rep.inputs[name] = "skipped"
                continue
            (reader or (lambda q: read_pip(q, rep, policy, args.strict_or)))(p)
            rep.inputs[name] = "read"
    if args.sources:
        read_sources(args.root.resolve(), rep, args.policy)
        rep.inputs["sources"] = "read"
    else:
        rep.inputs["sources"] = "skipped"
    apply_exceptions(rep, policy, today)
    return finish(rep), rep


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.write_licenses_md and not args.corpora:
        print("license_gate: --write-licenses-md needs --corpora", file=sys.stderr)
        return 2
    try:
        result, rep = run_corpora(args) if args.corpora else run(args)
    except ManifestInvalid as e:
        print(f"license_gate: {e}", file=sys.stderr)
        return 4
    except (UsageError, OSError, json.JSONDecodeError, tomllib.TOMLDecodeError) as e:
        print(f"license_gate: {e}", file=sys.stderr)
        return 2
    for f in result["findings"] + result["warnings"]:
        print(f"{f['code']} {f['where']} {f['license'] or f['message']}".rstrip(), file=sys.stderr)
    report_path = args.report or (args.manifest.parent / "license_gate_report.json" if args.manifest else None)
    if args.corpora and not args.report:
        report_path = None  # the corpora manifest sits in the source tree; write a report only when asked
    if report_path is not None:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.export_table:
        args.export_table.write_text(export_table(rep), encoding="utf-8")
    print(
        f"license gate: {result['status']} ({len(result['findings'])} findings, {len(result['inventory'])} components)",
        file=sys.stderr,
    )
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
