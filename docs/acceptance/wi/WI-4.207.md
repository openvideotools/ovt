# WI-4.207 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3 and clang 18.1.3. Date: 2026-11-16. Spec question: #593.

## Test 1: a clean capture shows 0 for all P1 checks

Automated: `tr101290: a clean capture has no priority 1 or 2 error` (synthetic stream, all 14 checks 0) and
`corpus: syn_ts_clean has no priority 1 error` (`[container][mpegts][tr101290][needs-corpus][fast]`, the clean file that
recipe 33 writes beside `syn_ts_cc_errors.ts`). The corpus test skipped in this container because the corpora cannot
be fetched here; CI runs it.

## Test 2: `syn_ts_cc_errors` Continuity_count_error equals TSDuck

Automated: `corpus: syn_ts_cc_errors reports its seven continuity errors` expects 7, the number
`tools/ts_inject_errors.py --cc-errors 7` injects (each drop is exactly one error by construction). The TSDuck
`tsp -P continuity` comparison and `tsanalyze` timing counts of step 3 need the oracle image (chapter 14 §14.2.6);
TSDuck is not installed here, so that comparison is **not done** and stays open for the oracle run.

## Fuzzing

`fuzz_mpegts` now also verifies and cross-checks `Reader::ts_report()`; 60 s local libFuzzer run with ASan and UBSan from
the existing seeds plus `seed_8_tr101290_timers.bin`: no finding.
