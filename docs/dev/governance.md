# Governance and review rules

This page implements chapter 15 §15.7.3 (WI-0.605). The people are in [`MAINTAINERS.md`](../../MAINTAINERS.md),
the reviewer routing is in [`.github/CODEOWNERS`](../../.github/CODEOWNERS), and the branch rules are in
[`tools/github/branch_protection.json`](../../tools/github/branch_protection.json).

## Decision rules

- **Pull requests:** lazy consensus. A maintainer approves, or nobody objects within the review window.
- **ADR changes** (chapter 0): a majority of the maintainers listed in `MAINTAINERS.md`.
- **Licensing and trademark:** the owner has a veto. A change to a licence header or a move of code between
  directories with different licences is its own pull request, titled `licence:` (FR-GEN-108).
- **Public interfaces** (`core/include/ovt/`, the plugin ABI, `core/schema/`): two approvals, and ABI or schema
  changes stay open at least 72 hours (`CONTRIBUTING.md`).

## Teams

Create these by hand in the `openvideotools` organisation (GitHub offers no repository-level API for it that
the automation may use). `CODEOWNERS` names them as `@openvideotools/<team>`.

| Team | Owns |
| ---- | ---- |
| `ovt-maintainers` | everything not listed below, `docs/spec/` |
| `ovt-core-api-reviewers` | `core/include/ovt/`, `core/schema/`, with `ovt-licence-reviewers` also `core/include/ovt/abi/` |
| `ovt-licence-reviewers` | `LICENSES/`, `LICENSE`, `DCO`, `REUSE.toml`, the licence gate and policy, `third_party/`, `vcpkg.json` |
| `ovt-build` | `third_party/`, `vcpkg.json`, `.github/` |
| `ovt-render`, `ovt-decoders`, `ovt-app`, `ovt-android`, `ovt-python` | `render/`, `plugins/`, `app/` and `cli/`, `android/`, `python/` |

## What `main` enforces today

The repository is public, and the owner set one ruleset, `Main Protection`: no deletion, no force-push, linear
history, and a pull request for every change with zero required approvals. `tools/github/branch_protection.json`
is that ruleset, so `tools/github/apply_protection.sh --check` reports no drift.

```sh
tools/github/apply_protection.sh --check                      # read-only; exit 1 names each drifted key
tools/github/apply_protection.sh [--repo openvideotools/ovt]  # create or update the ruleset; needs admin rights
```

The merge rules of `CLAUDE.md` §2 (a separate reviewer's `Review verdict: approve`, green CI) are followed by the
agents and the coordinator; GitHub does not enforce them yet.

### Not enforced yet, on purpose

The spec's full target (chapter 13 §13.7.3) adds the items below. Each one is a one-line addition to
`branch_protection.json` followed by `apply_protection.sh`, and each needs the owner's decision:

- **Required status checks** (`lint`, `license-gate`, `build-linux-x64`, `build-macos-arm64`, `asan-ubsan`,
  `fuzz-smoke`, `clang-tidy`, `coverage`, `abi-check`, `DCO`, and the Android, GUI and Python checks once they
  exist). Not yet, because two `asan-ubsan` failures are accepted on `main` and would block every merge, and
  because a check name that no workflow produces yet would block merges forever.
- **Code-owner review** (`require_code_owner_review`) with one approval, two for `core/include/ovt/abi/`,
  `core/schema/`, `LICENSES/` and `tools/license_policy.toml`. Not yet, because the teams above do not exist and
  the project has one maintainer.
- **Signed tags:** a tag ruleset on `v*` requiring signatures. Not yet, because no key fingerprint is published.

**Limitation of the two-approval rule.** GitHub applies one approval count to a whole branch rule, not per path.
The "two approvals for `abi/`" rule is therefore realised by CODEOWNERS: `abi/` lists two teams, code-owner
review requires an approval from each team that owns a changed file, and so a change to `abi/` collects one
approval from each. A person in both teams counts for both, so keep the teams disjoint.

## Verification record

The GitHub UI checks (a pull request touching only `app/` asks for one approval, one touching
`core/include/ovt/abi/` asks for two) apply only after the teams exist and code-owner review is on. They are
recorded as pending in [`docs/acceptance/wi/WI-0.605.md`](../acceptance/wi/WI-0.605.md).
