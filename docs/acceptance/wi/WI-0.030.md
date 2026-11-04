# WI-0.030 acceptance record

Date: 2026-11-04. Spec version: 1.0.0-rc1. Commit: the head of the WI-0.030 pull request. Spec question: #27.

## Static checks (cloud container, Ubuntu 24.04 x86_64)

```console
$ check-jsonschema --builtin-schema vendor.github-workflows .github/workflows/ci.yml
ok -- validation done
$ check-jsonschema --builtin-schema vendor.github-actions .github/actions/setup-ovt/action.yml
ok -- validation done
```

Every `run:` block passes `shellcheck -S warning` (extracted with `${{ }}` expressions replaced).

## Test 1: the three jobs run and pass on a PR

The PR's own checks show the result. The jobs also run on `ci/m01-integration`, a branch that merges every
open M0.1 pull request, to check the jobs against the complete M0.1 tree. The results are linked from the PR.

`build-macos-arm64` needs the self-hosted runner on the Mac Mini to be online with the labels
`self-hosted, macOS, ARM64`. Without it, the job waits in the queue.

## Test 2: `build-linux-x64` finishes in at most 40 minutes with a warm cache

Measured on the second run of the job, after the first run has saved the caches. The time is recorded in the
PR.
