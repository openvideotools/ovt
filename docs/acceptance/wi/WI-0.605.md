# WI-0.605 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, jq 1.7, Python 3.13, pytest 8.3.4. Date: 2026-11-07. Spec version:
1.0.0-rc15. Commit: the head of the WI-0.605 pull request.

## Test 1: one approval for `app/`, two for `core/include/ovt/abi/`

**Pending.** The CODEOWNERS teams (`ovt-maintainers`, `ovt-core-api-reviewers`, `ovt-licence-reviewers`, …) do not
exist yet, and the live ruleset has `require_code_owner_review: false` and zero required approvals, as the owner
set it. Creating the teams is a manual owner step (`docs/dev/governance.md`); the check is then to open a draft
pull request touching `app/` and one touching `core/include/ovt/abi/` and record the GitHub UI screenshots here.

## Test 2: `apply_protection.sh --check` against the live repository

```console
$ tools/github/apply_protection.sh --check; echo "exit $?"
no drift: ruleset 'Main Protection' of openvideotools/ovt matches branch_protection.json
exit 0
```

A changed setting is reported by key and exits 1. This is covered without touching the live repository by
`tools/tests/test_apply_protection.py` (fake `gh`), for example:

```console
$ pytest tools/tests/test_apply_protection.py -q
9 passed in 0.26s
```

`test_check_names_a_changed_setting` asserts the line
`drift: rules.pull_request.required_approving_review_count live=2 wanted=0`.

## Not run

`apply_protection.sh` without `--check` was not run: it changes repository settings and needs admin rights.
Its PUT and POST calls are covered by the fake-`gh` tests.
