#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Generate the libvmaf model table core/src/metrics/vmaf/vmaf_models.inc (chapter 9 §9.5.3, WI-3.309).

    gen_vmaf_ids.py --libvmaf-src <vmaf archive root> [-o core/src/metrics/vmaf/vmaf_models.inc]
    gen_vmaf_ids.py --libvmaf-src <vmaf archive root> --check core/src/metrics/vmaf/vmaf_models.inc

The model list is the one libvmaf compiles in with `-Dbuilt_in_models=true`: the `model_files` lists of
`libvmaf/src/meson.build` (the `+=` list holds the float models, built only with `-Denable_float=true`). Each
model JSON in `model/` gives the features the model reads; the outputs of a metric id are the model score
followed by every feature libvmaf computes for it, named exactly as the `vmaf` CLI writes them in its JSON.

OVT id: the model version without its `_v<a>.<b>.<c>` part, with a trailing `neg` turned into `_neg`
(`vmaf_v0.6.1` -> `vmaf`, `vmaf_4k_v0.6.1neg` -> `vmaf_4k_neg`, `vmaf_float_b_v0.6.3` -> `vmaf_float_b`).
A model collection (bootstrapped model, `vmaf_b`) has the outputs `bagging, ci_p95_lo, ci_p95_hi, stddev`.

An option or feature this script does not know fails the run, so a new libvmaf tag cannot silently produce
wrong output names. --check exits 1 when the checked-in file differs from what the tree generates.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# Feature families of the model JSON feature names (VMAF_[integer_]feature_<name>_score), and every score the
# extractor writes, in the order the vmaf CLI prints them.
_FAMILIES = {
    "adm": ["adm2", "adm_scale0", "adm_scale1", "adm_scale2", "adm_scale3"],
    "motion": ["motion2", "motion"],
    "vif": ["vif_scale0", "vif_scale1", "vif_scale2", "vif_scale3"],
}
_FEATURE_TO_FAMILY = {
    "adm2": "adm",
    "motion2": "motion",
    "vif_scale0": "vif",
    "vif_scale1": "vif",
    "vif_scale2": "vif",
    "vif_scale3": "vif",
}
# Extractor options that rename the outputs (libvmaf feature_name.c: alias plus the value, "%g").
_OPTION_ALIASES = {"adm_enhn_gain_limit": "egl", "vif_enhn_gain_limit": "egl"}
_COLLECTION_OUTPUTS = ["bagging", "ci_p95_lo", "ci_p95_hi", "stddev"]
_FEATURE_RE = re.compile(r"^VMAF_(integer_)?feature_([a-z0-9_]+)_score$")
_VERSION_RE = re.compile(r"^(?P<stem>[a-z0-9_]+?)_v\d+(?:\.\d+)*(?P<suffix>[a-z]*)$")
_ID_RE = re.compile(r"^[a-z0-9_]{1,32}$")


class GenError(Exception):
    """A model or option the generator does not understand."""


def model_files(meson_build: str) -> list[tuple[str, bool]]:
    """(file name, needs enable_float) for each built-in model, in meson.build order."""
    out: list[tuple[str, bool]] = []
    for m in re.finditer(r"model_files\s*(\+?=)\s*\[(.*?)\]", meson_build, re.DOTALL):
        is_float = m.group(1) == "+="
        for name in re.findall(r"'([^']+\.json)'", m.group(2)):
            out.append((name, is_float))
    if not out:
        raise GenError("no model_files list in libvmaf/src/meson.build")
    return out


def ovt_id(version: str) -> str:
    m = _VERSION_RE.match(version)
    if not m:
        raise GenError(f"model version {version!r} does not end in _v<a>.<b>.<c>[suffix]")
    stem, suffix = m.group("stem"), m.group("suffix")
    if suffix not in ("", "neg"):
        raise GenError(f"model version {version!r}: unknown suffix {suffix!r}")
    oid = stem + ("_neg" if suffix else "")
    if not _ID_RE.match(oid):
        raise GenError(f"generated id {oid!r} is not [a-z0-9_]{{1,32}}")
    return oid


def _option_suffix(opts: dict) -> str:
    parts = []
    for key, value in opts.items():
        alias = _OPTION_ALIASES.get(key)
        if alias is None:
            raise GenError(f"unknown feature option {key!r}")
        parts.append(f"_{alias}_{float(value):g}")
    return "".join(parts)


def single_model_outputs(model_dict: dict) -> list[str]:
    names = model_dict.get("feature_names")
    if not names:
        raise GenError("model_dict has no feature_names")
    opts = model_dict.get("feature_opts_dicts") or [{}] * len(names)
    if len(opts) != len(names):
        raise GenError("feature_opts_dicts and feature_names differ in length")
    outputs = ["vmaf"]
    seen: set[str] = set()
    for name, opt in zip(names, opts):
        m = _FEATURE_RE.match(name)
        if not m:
            raise GenError(f"unknown feature name {name!r}")
        prefix = "integer_" if m.group(1) else ""
        family = _FEATURE_TO_FAMILY.get(m.group(2))
        if family is None:
            raise GenError(f"unknown feature {name!r}")
        suffix = _option_suffix(opt)
        key = prefix + family + suffix
        if key in seen:
            continue
        seen.add(key)
        outputs += [prefix + f + suffix for f in _FAMILIES[family]]
    return outputs


def describe(root: Path) -> list[dict]:
    meson = (root / "libvmaf" / "src" / "meson.build").read_text(encoding="utf-8")
    entries = []
    ids: set[str] = set()
    for file_name, needs_float in model_files(meson):
        version = file_name[: -len(".json")]
        doc = json.loads((root / "model" / file_name).read_text(encoding="utf-8"))
        oid = ovt_id(version)
        if oid in ids:
            raise GenError(f"two models map to id {oid!r}")
        ids.add(oid)
        if "model_dict" in doc:
            collection = False
            outputs = single_model_outputs(doc["model_dict"])
        else:
            members = [v for v in doc.values() if isinstance(v, dict) and "model_dict" in v]
            if not members:
                raise GenError(f"{file_name}: neither a model nor a model collection")
            collection = True
            outputs = list(_COLLECTION_OUTPUTS)
        entries.append(
            {"id": oid, "version": version, "collection": collection, "needs_float": needs_float, "outputs": outputs}
        )
    return sorted(entries, key=lambda e: e["id"])


def render(entries: list[dict]) -> str:
    lines = [
        "// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors",
        "// SPDX-License-Identifier: LGPL-2.1-or-later",
        "//",
        "// GENERATED by tools/gen_vmaf_ids.py from the pinned libvmaf tree (chapter 9 §9.5.3, WI-3.309). Do not edit;",
        "// regenerate with `tools/gen_vmaf_ids.py --libvmaf-src <vmaf> -o core/src/metrics/vmaf/vmaf_models.inc`.",
        "// One OVT_VMAF_MODEL(id, model version, is a model collection, needs -Denable_float=true, outputs...) per",
        "// built-in model; outputs are the names libvmaf writes in its JSON, the model score first.",
        "// clang-format off",
    ]
    for e in entries:
        outs = ", ".join(f'"{o}"' for o in e["outputs"])
        lines.append(
            f'OVT_VMAF_MODEL("{e["id"]}", "{e["version"]}", {str(e["collection"]).lower()}, '
            f"{str(e['needs_float']).lower()}, {outs})"
        )
    lines.append("// clang-format on")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--libvmaf-src", required=True, type=Path, help="root of the vmaf source archive")
    group = ap.add_mutually_exclusive_group()
    group.add_argument("-o", "--output", type=Path, help="write the table here (default: stdout)")
    group.add_argument("--check", type=Path, help="compare with this checked-in file")
    args = ap.parse_args(argv)
    try:
        text = render(describe(args.libvmaf_src))
    except (GenError, OSError, json.JSONDecodeError) as exc:
        print(f"gen_vmaf_ids: {exc}", file=sys.stderr)
        return 2
    if args.check:
        current = args.check.read_text(encoding="utf-8") if args.check.exists() else ""
        if current != text:
            print(f"gen_vmaf_ids: {args.check} is stale; regenerate it from {args.libvmaf_src}", file=sys.stderr)
            return 1
        return 0
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
