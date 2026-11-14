# WI-0.033 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, clang 18.1.3 (libFuzzer), Python 3.x. Date: 2026-11-07.
Spec version: 1.0-rc25. Commit: the head of the WI-0.033 pull request.

The container cannot download FFmpeg, so the full `linux-x64-fuzz` build was not possible locally; the canary
target and the nightly shell steps were exercised on stand-alone binaries. The CI runs below are the authority.

## Acceptance test 1: a crashing target fails the smoke job and uploads `crash-*`

Locally, with the canary built the way `ovt_add_fuzz_target` builds it (libFuzzer plus ASan):

```console
$ clang++ -fsanitize=fuzzer,address -o canary core/tests/fuzz/fuzz_canary.cpp
$ ./canary -runs=1000 -artifact_prefix=art/ core/tests/fuzz/corpus/canary; echo $?
0
$ OVT_FUZZ_CANARY=1 ./canary -max_total_time=5 -artifact_prefix=art/ core/tests/fuzz/corpus/canary; echo $?
77
$ ls art
crash-e4420cb5b40f37361219ab86dac3ed14a9c91e1a
```

In CI: Actions, `fuzz-smoke`, Run workflow with `canary` ticked, on `main` after the merge (a manually
dispatched workflow must exist on the default branch). Expected: `fuzz-smoke-shard (0)` and therefore
`fuzz-smoke` fail, and the artefact `fuzz-crashes-shard-0` holds `crash-e4420cb5b40f37361219ab86dac3ed14a9c91e1a`.
Record the run URL in the pull request after the dispatch.

## Acceptance test 2: healthy targets, 60 s each, job within 15 min

The ordinary pull-request run of `fuzz-smoke` on this branch (canary excluded, no dispatch input): the PR's check
list shows `fuzz-smoke-shard (0..3)` and `fuzz-smoke`; each shard's log shows `-max_total_time=60` per target.
