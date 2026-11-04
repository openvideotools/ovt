# WI-0.007 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, Python 3.13, pre-commit 4.0.1, clang-format 19.1.6 (hook),
git 2.43. Date: 2026-11-04. Spec version: 1.0.0-rc1. Commit: the head of the WI-0.007 pull request.

Automated tests: `tools/tests/test_check_scripts.py` (17 tests). They run in the `tools_pytest` CTest test
once WI-0.550 is merged, and with `python -m pytest tools/tests` before that.

## Test 1: a mis-formatted file fails pre-commit and clang-format fixes it

```console
$ printf 'int  main( ){int *p=nullptr;return p==nullptr?0:1;}\n' > core/src/zz_misformatted.cpp
$ git add core/src/zz_misformatted.cpp
$ pre-commit run clang-format --files core/src/zz_misformatted.cpp; echo "exit $?"
clang-format.............................................................Failed
- hook id: clang-format
- files were modified by this hook
exit 1
$ cat core/src/zz_misformatted.cpp
int main() {
  int* p = nullptr;
  return p == nullptr ? 0 : 1;
}
$ pre-commit run clang-format --files core/src/zz_misformatted.cpp; echo "exit $?"
clang-format.............................................................Passed
exit 0
```

(The file was removed afterwards.) The spec says `pre-commit run --all-files`. On this tree that also
reformats every C++ and CMake file written before WI-0.007, so the run above names the file instead (see
the PR's deviations). Automated: `test_clang_format_config_reformats`. Result: pass.

## Test 2: a commit message without `Signed-off-by` fails the commit-msg hook with the `git commit -s` hint

```console
$ printf 'WI-0.007: add hooks\n\nWhy.\n' > nosign.txt
$ pre-commit run --hook-stage commit-msg --commit-msg-filename nosign.txt; echo "exit $?"
commit message references a WI id or an issue..................................Passed
DCO Signed-off-by trailer (missing? commit with `git commit -s`)...............Failed
- hook id: dco-signoff
- exit code: 1

nosign.txt
exit 1
$ printf 'WI-0.007: add hooks\n\nWhy.\n\nSigned-off-by: Ada Lovelace <ada@example.org>\n' > sign.txt
$ pre-commit run --hook-stage commit-msg --commit-msg-filename sign.txt; echo "exit $?"
commit message references a WI id or an issue..................................Passed
DCO Signed-off-by trailer (missing? commit with `git commit -s`)...............Passed
exit 0
$ printf 'add hooks\n\nSigned-off-by: Ada Lovelace <ada@example.org>\n' > noref.txt
$ pre-commit run --hook-stage commit-msg --commit-msg-filename noref.txt; echo "exit $?"
commit message references a WI id or an issue..................................Failed
- hook id: check-wi-refs
- exit code: 1

error: commit message references no work item: start the subject with the WI id (`WI-0.007: ...`) or mention an issue (`#123`) (chapter 16 §16.2.3)
exit 1
```

Automated: `test_dco_hook_pattern`, `test_commit_msg_needs_a_reference`. Result: pass.

## Test 3: a dependency on a later milestone fails `--table`, naming both WIs

Automated with a miniature spec in which WI-0.001 (M0.1) depends on WI-0.002 (M0.2):
`test_dependency_on_a_later_milestone_names_both`. The test checks for the line
`WI-0.001 (M0.1) depends on WI-0.002 (M0.2), a later milestone (§16.2.5 milestone-order rule)` and exit
status 1. Result: pass.

On the real specification:

```console
$ python3 -I tools/check_wi_refs.py --table; echo $?
requirements no WI claims (2): FR-PLN-102, FR-VQ-013
open SPEC-ASSUMPTION comments: WI-0.000 2, unattributed 1
§16.4: 380 rows, 300 requirements, 0 error(s)
0
```

This is zero errors after one assumption: WI-4.599's chapter line "all phase-4 WIs" is taken as the
`none` of its §16.4 row (spec question #17). The two unclaimed requirements are the known GAPs of §17.4
(OI-07, OI-08).

## Test 4: a change to `docs/spec/04-requirements.md` without a version bump fails `--spec-version`

Automated in a scratch git repository: `test_spec_change_without_version_bump_fails`. The test edits the
requirements file and expects exit 1 ("version … is still '1.0-rc1 (2026-11-04)'"). It then bumps the
front-matter version, adds a `## 1.0.1-rc1 — …` changelog entry and expects exit 0. On this branch, which
does not touch `docs/spec/`:

```console
$ python3 -I tools/check_wi_refs.py --spec-version --base origin/main; echo $?
0
```

Result: pass.

## Test 5: `--make-issues --dry-run` reports zero issues on its second run

The spec's wording, "run twice … the second run reports zero", needs a real run between the two, because a
dry run creates nothing. `test_make_issues_is_idempotent` therefore makes:

1. a real run against a file-backed issue list, with a stand-in `gh` that records its calls (3 issues
   created);
2. a dry run, which reports `0 issue(s) to create`.

On the real table, with no tracker issues yet:

```console
$ python3 -I tools/check_wi_refs.py --make-issues --dry-run --issues-json none.json
would create: WI-0.000: Repository skeleton and top-level CMake
would create: WI-0.001: CMakePresets.json and custom vcpkg triplets
would create: WI-0.002: vcpkg manifest, submodule and binary caching
...
380 issue(s) to create, 0 already exist
```

The live run against GitHub (`gh issue list` / `gh issue create`) was not made from this session: creating
380 issues is for the coordinator or Karl to decide. Result: pass (idempotency automated).

## `renovate.json`

```console
$ npx --package renovate@39.86.0 -- renovate-config-validator --strict renovate.json
 INFO: Validating renovate.json
 INFO: Config validated successfully
```
