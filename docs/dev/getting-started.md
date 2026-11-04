# Getting started

The bootstrap and first-build instructions arrive with WI-0.006. This page so far covers the pre-commit
hooks (WI-0.007).

## Pre-commit hooks

`.pre-commit-config.yaml` holds the hooks of chapter 13 §13.9.2 at pinned revisions. Install them once per
clone:

```sh
pip install pre-commit==4.0.1
pre-commit install --install-hooks      # installs the pre-commit and commit-msg hooks
```

On every `git commit` they check the staged files:

| Hook | Files | What it does |
|---|---|---|
| `clang-format` | C, C++, Objective-C++ | Reformats with `.clang-format` (Google base, 120 columns, `int* p`). |
| `cmake-format` | `CMakeLists.txt`, `*.cmake` | Reformats with `.cmake-format.yaml`. |
| `ruff`, `ruff-format` | Python | Lint and format, 120 columns. |
| `mypy` | `tools/`, `python/` | Type check. |
| `check-added-large-files` | all | Rejects files over 2 MB (corpora and big goldens are never committed). |
| `end-of-file-fixer`, `trailing-whitespace` | all | Whitespace fixes. |
| `shellcheck` | shell scripts | Lint. |
| `pretty-format-kotlin` | `*.kt` | ktlint; needs a JDK. |
| `check-versions-md` | `third_party/` | `tools/check_versions_md.py`: every `third_party/*.cmake` script has a `VERSIONS.md` row, and the scripts read their pins from `versions.cmake`. |
| `reuse-lint-file` | source files | SPDX header present and its licence text in `LICENSES/` (`docs/licensing.md`). |

A hook that reformats a file fails the commit and leaves the fix in your working tree. Look at the change,
`git add` it and commit again.

The commit-msg stage checks the message:

- `check-wi-refs`: the message must reference a work item (`WI-0.007: …`) or an issue (`#15`). Merge and
  `fixup!` commits are exempt.
- `dco-signoff`: the message needs a `Signed-off-by:` trailer (DCO, chapter 15 §15.7). The hook does not
  add the trailer for you; it stops the commit and tells you to use `git commit -s`.

The files that existed before WI-0.007 have not been reformatted yet. `pre-commit run --all-files` would
rewrite them, so run the hooks on your own changes instead:

```sh
pre-commit run --from-ref origin/main --to-ref HEAD
```

## Specification checks

`tools/check_wi_refs.py` checks the work-item bookkeeping of chapter 16:

```sh
python3 -I tools/check_wi_refs.py --table                  # §16.4 against the chapters and §4.17 (§16.2.5)
python3 -I tools/check_wi_refs.py --spec-version           # a docs/spec change needs a version bump (§16.2.8)
python3 -I tools/check_wi_refs.py --make-issues --dry-run  # tracker issues still to create (§16.2.1)
```

Run `--table` whenever you change a WI block or a §16.4 row.
