# WI-0.090 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, CMake 3.28.3, Node.js 22.
Date: 2026-11-08. Spec version: 1.0-rc15. Commit: the head of the WI-0.090 pull request.

## Test 1: export of `tiny64_av1.fa` equals the golden, key set

Automated: CTest `test_aom_json: tiny64_av1.fa exports byte for byte as tiny64_av1.inspect.json` and
`test_aom_json: the per-frame key set is inspect -A's` (key set per spec question #269: the accounting keys are
absent when the record has no `symbols`; a second section adds symbols and checks the full set).

```console
$ ctest --preset linux-x64-debug -R aom_json
100% tests passed, 0 tests failed out of 8
```

## Test 2: `tools/aomanalyzer_smoke.js`

Automated: CTest `test_aom_json_smoke`, run on `core/tests/golden/tiny64_av1.inspect.json` (test 1 proves the
exporter writes exactly that file). `test_aom_json_smoke_rejects_grid` and `test_aom_json_smoke_rejects_text`
check that the script fails on a wrong MI grid and on a file that is not JSON.

```console
$ node tools/aomanalyzer_smoke.js core/tests/golden/tiny64_av1.inspect.json --expect-mi 16x16
core/tests/golden/tiny64_av1.inspect.json: 1 frame(s) OK
$ echo $?
0
```

## Manual step: loading in aomanalyzer

Not yet done. The WI asks for a screenshot of the file loaded in xiph aomanalyzer; the cloud container that
implemented the WI has no browser session for it. Owed on a desktop (or the Mac Mini): open aomanalyzer, load
`core/tests/golden/tiny64_av1.inspect.json`, add the screenshot here as `WI-0.090-aomanalyzer.png`.
