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
TSDuck was not available in the authoring container; the Mac Mini oracle run (below) supplies the comparison.

## Fuzzing

`fuzz_mpegts` now also verifies and cross-checks `Reader::ts_report()`; 60 s local libFuzzer run with ASan and UBSan from
the existing seeds plus `seed_8_tr101290_timers.bin`: no finding.

## TSDuck oracle record (Mac Mini, PR head 31e8b18)

```console
WI-4.207 (PR #595 @ 31e8b18) — TSDuck oracle comparison, Mac Mini, 2026-11-16
Machine: ShafferDix-Mini (Mac16,10, Apple M4, macOS 15.8.2); TSDuck 3.45-4798 in the ovt-oracles image (linux/amd64).

Corpus: recipe 33 generated in the image:
python3 -I tests/corpora/synthetic/generate.py --only syn_ts_cc_errors --out /out --check
generate src_720p_8bit.y4m
generate syn_ts_cc_errors
152b286b1fd4fb7801cccc85894d20aaab125d66fcab082aa93f4eb3888bab5e  syn_ts_cc_errors      (--check passed)

tsp -I file syn_ts_clean.ts -P continuity -O drop
(no output: 0 continuity errors)

tsp -I file syn_ts_cc_errors.ts -P continuity -O drop
* continuity: packet index: 1,077, PID: 0x0100 (256), missing 1 packets
* continuity: packet index: 2,158, PID: 0x0100 (256), missing 1 packets
* continuity: packet index: 3,230, PID: 0x0100 (256), missing 1 packets
* continuity: packet index: 4,307, PID: 0x0100 (256), missing 1 packets
* continuity: packet index: 5,384, PID: 0x0100 (256), missing 1 packets
* continuity: packet index: 6,464, PID: 0x0100 (256), missing 1 packets
* continuity: packet index: 7,537, PID: 0x0100 (256), missing 1 packets
=> 7 continuity errors, equal to Continuity_count_error 7 expected by the corpus test.

tsanalyze --normalized (selected fields):
syn_ts_clean.ts:     packets=8621, invalidsyncs=0, transporterrors=0; PID 256: packets=8533, pcr=40, pcrleap=0, discontinuities=0
syn_ts_cc_errors.ts: packets=8614, invalidsyncs=0, transporterrors=0; PID 256: packets=8526, pcr=30, pcrleap=1 (the injected --pcr-gap 1)

OVT tests (macos-arm64-release, OVT_CORPORA_DIR=<generated>, OVT_REQUIRE_CORPORA=1):
ovt_tests_container "[tr101290]"
All tests passed (191 assertions in 14 test cases)
  corpus: syn_ts_clean has no priority 1 error                 -> 7 assertions, pass
  corpus: syn_ts_cc_errors reports its seven continuity errors -> 3 assertions, pass

Build note: with the preset's -Werror, AppleClang 17 fails on core/tests/container/tr101290_test.cpp:64:38:
  error: implicit conversion changes signedness: 'int' to 'unsigned int' [-Werror,-Wsign-conversion]
  out[3] = static_cast<uint8_t>((tsc << 6U) | ...   ('tsc << 6U' is int)
The results above use -DOVT_WERROR=OFF; the macOS CI build will hit the same error.
```
