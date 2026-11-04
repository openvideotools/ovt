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

Result: pass. On PR #28 at `821ee7f`, `lint`, `build-linux-x64` and `build-macos-arm64` all passed
(2026-11-04). The same three jobs passed on `ci/m01-integration` at `b7f5409`, a branch that merges every
open M0.1 pull request (run 41791237849). On the Mac Mini runner, `ctest` ran 8 of 8 tests and passed,
including `downstream_consumer`.

`build-macos-arm64` needs the self-hosted runner on the Mac Mini to be online with the labels
`self-hosted, macOS, ARM64`. Without it, the job waits in the queue.

## Test 2: `build-linux-x64` finishes in at most 40 minutes with a warm cache

Result: pass. The cold run on PR #28 took 7 min 10 s (07:18:56 to 07:26:06 UTC). The warm run on
`ci/m01-integration`, with the vcpkg and ccache caches restored, took 1 min 33 s (07:20:27 to 07:22:00 UTC).
Both are well under the 40 min budget. The times will grow as the third-party builds of WI-0.011 to
WI-0.015 land.
