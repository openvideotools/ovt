# The licence gate

`tools/license_gate.py` is the CI job `license-gate` (chapter 15 §15.4.2). It reads
`tools/license_policy.toml` and fails when a dependency, a link, a source header or a release artefact breaks
the licence rules of chapter 15. It uses the Python standard library only and runs under `python -I`.

```sh
python -I tools/license_gate.py --policy tools/license_policy.toml \
    --versions third_party/VERSIONS.md --sources          # what exists today
python -I tools/license_gate.py --policy tools/license_policy.toml \
    --manifest build/linux-x64-release/license_manifest.json --require-all
```

Inputs are all optional except `--policy`. An omitted input is reported as `skipped` in the JSON report. With
`--require-all`, an input flag whose file does not exist exits 2 with `missing required input: <name>`, so the CI
job passes a flag only for inputs whose producing work item has merged. `--strict-or` also deny-checks the
alternatives of an `OR` expression that the gate did not choose (CI uses it). `--export-table <md>` writes the
inventory as a Markdown table, and `--report <json>` overrides the report path (default:
`license_gate_report.json` next to `--manifest`).

Exit codes: 0 pass, 1 at least one finding, 2 usage error (bad flags, unreadable policy, missing required
input). Warnings (`W-*`) never change the exit code.

## Corpora mode

`--corpora` (WI-0.561) checks the test-media registry instead of the dependencies (chapter 14 §14.3.4). It is
a separate invocation: `--manifest` then names `tests/corpora/manifest.json`, not a `license_manifest.json`.

```sh
python -I tools/license_gate.py --policy tools/license_policy.toml --corpora           # CI
python -I tools/license_gate.py --policy tools/license_policy.toml --corpora --write-licenses-md
```

Exit codes: 0 clean, 1 violation, 4 manifest unreadable or an entry lacks `id`, `licence`, `licence_spdx` or a
boolean `redistributable` (rule 1). Rules and findings:

| Code | Rule | Meaning | Resolve |
|---|---|---|---|
| `E-DENY` | 4 | `licence_spdx` matches `[deny]` (`AGPL`, `SSPL`, `BUSL`, `NonCommercial`, `CC-BY-NC`, ...). `where` is `corpora:<id>`. | Drop the entry; such media is never in the manifest. |
| `E-CORPUS` | 2 | `redistributable: true` with an id outside `[corpora] redistributable_spdx`. | Set `redistributable: false`, or, if the licence really is permissive, add it to the list in a reviewed PR. |
| `E-STALE` | 5 | `tests/corpora/LICENSES.md` is missing or differs from the generated text. | Run with `--write-licenses-md` and commit the file. |

Rule 3 (non-redistributable media stay out of release artefacts and public caches) is a packaging rule that no
gate input describes yet; this mode does not check it.

## Finding codes and how to resolve them

Every finding has a `code`, a `where`, the offending `license` and a message. Fix by one of three routes, in this
order of preference: remove the dependency, add the licence to the allow list, or add a dated exception.

| Code | Meaning | Resolve |
|---|---|---|
| `E-DENY` | An id matches `[deny].patterns` (AGPL, SSPL, BUSL, non-commercial, ...). `BSL-1.0` is allowed, `BUSL-1.1` is not. | Remove the dependency. Deny entries are never added to the allow list. |
| `E-UNKNOWN` | An id is in no allow list, an expression does not parse, or a source gave no licence (`NOASSERTION`, a Gradle name without a mapping). | New permissive or weak-copyleft licence: one-line PR adding it to `[allow.any]` (and to `[allow].permissiveness`), reviewed by a licence reviewer. Prose in a `VERSIONS.md` cell: `[versions_overrides]`. vcpkg `NOASSERTION`: `[vcpkg_overrides]`. Gradle name: `[gradle_license_names]`. |
| `E-LINK` | A target links a library whose licence its own licence does not allow (chapter 15 §15.3), for example GPL-3.0-only in `ovt_core`. `where` is `<target> <lib>`. | Remove the link, or move the code to a target of a compatible licence. Never relicense a target to make this pass. |
| `E-TARGET` | A target's licence in `license_manifest.json` differs from `[targets]`, has no `[targets]` entry, or is not an allowed own licence. | Fix `ovt_set_license`, or add the target to `[targets]` in the same PR that adds it. |
| `E-HEADER`, `E-NOHEADER` | A source file's `SPDX-License-Identifier` is not the one `[directories]` requires for its path, or is missing. | Run `tools/spdx_header.py`. |
| `E-DIST` | A release artefact breaks ADR-015: a default download contains `ovt_plugin_ffmpeg`, `ovt_plugin_de265` or `ovt_plugin_hm`, a codec pack is a default download, or a required notice is missing. Needs `--release-manifest`. | Move the plugin to the codec pack, or set `notices`. |
| `E-EXPIRED` | An `[[exceptions]]` entry is past `expires`. | Remove the dependency, or renew the exception in a reviewed PR. |
| `W-PIP-UNRESOLVED` | A pip requirement's licence was not found in installed metadata. | Add `[pip_overrides]`. |

## How an `OR` expression is resolved

The gate picks the alternative that comes first in `[allow].permissiveness` and is allowed for the target class
(`any` for LGPL targets, `any` plus `gpl_targets` for GPL targets, and a per-plugin set). An alternative that is
not in `permissiveness` is never chosen. `BSD-3-Clause OR GPL-2.0-only` resolves to `BSD-3-Clause`. The choice is
recorded as `chosen` in the inventory that `tools/gen_notices.py` reads. `WITH` binds tighter than `AND`, which
binds tighter than `OR`, and a `WITH` expression is one id.

## Exceptions

```toml
[[exceptions]]
id = "GPL-2.0-only"
where = "tests/oracles/bento4"       # prefix of the finding's `where`
reason = "oracle binary invoked as subprocess, never linked"
expires = "2027-06-30"
approved_by = ["maintainer-a", "maintainer-b"]
```

An exception suppresses findings with the same `license` whose `where` starts with its `where`. After `expires`
it stops suppressing and the gate reports `E-EXPIRED`. Two licence reviewers approve the PR that adds one.

## `where` values

`third_party/VERSIONS.md:<Name>`, `vcpkg:<port>`, `gradle:<module>`, `pip:<package>`, a target name for the
manifest, a repository path for source headers, an artefact id for the release manifest.

## Module use

`license_gate.resolve(expr, policy) -> str` returns the chosen expression, and `license_gate.inventory(report)`
returns the inventory list of a loaded report. `tools/release/collect.py --check-licenses` (WI-0.036) and
`tools/gen_notices.py` (WI-0.603) import them. The report is deterministic: the same inputs give the same bytes.

## Tests

`python -m pytest tools/tests/test_license_gate.py` runs the nine acceptance tests of §15.4.2, the three extra
tests of WI-0.602 and the parser tests. Fixtures live in `tools/tests/fixtures/license_gate/`.
