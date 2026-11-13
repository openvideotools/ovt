# WI-4.204 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, Clang 18.1.3, FFmpeg/ffprobe from the distribution.
Date: 2026-11-08. Spec version: 1.0-rc2. Spec question: #337.

## Test 1: tsduck-test `input/*.ts` against `tsanalyze --json`

`tsanalyze` (TSDuck) is not installed on this machine and has no CI job yet, so the comparison with its JSON is
**not run here**. The automated test `corpus: tsduck-test input walks to the reference PID statistics`
(`[container][mpegts][needs-corpus][fast]`) compares the walker with counts taken by an independent brute-force
script instead, for corpus `ts_h264_dvb` (`input/test-012.ts`, 373 556 bytes):

```console
$ python3 - <<'PY'
d = open('test-012.ts', 'rb').read()
from collections import Counter
c = Counter(((d[i+1] & 0x1f) << 8) | d[i+2] for i in range(0, len(d), 188))
print(len(d) // 188, sorted(c.items()))
PY
1987 [(0, 78), (160, 77), (1068, 1832)]
```

The PMT lists more PIDs than the file carries (`ffprobe` shows 0x424..0x42c), which is why only three PIDs have
packets. Result: the walker returns 1 987 packets, PIDs 0 / 160 / 1068 with 78 / 77 / 1 832 packets and no
diagnostics. **Owed:** run `tsanalyze --json --pid-analysis` (or the field names of the pinned TSDuck release) on
this file and the other `input/*.ts` corpus entries on a machine that has TSDuck, and append the output here.

## Test 2: 1-byte cut

Automated: `parse: a 1-byte cut resyncs and reports CNT-TS-1001 once` (`[container][mpegts][fast]`): one byte is
deleted inside packet 50 of a 100-packet stream; the walk resynchronises at offset `50*188+187`, counts 100
packets and reports exactly one `CNT-TS-1001`.

## Local run

```console
$ g++ -std=c++20 -fsanitize=address,undefined … ts_packets_test.cpp … && ./t
fails=0
```

(The container has no Catch2 library, so the local run used a macro shim; CI runs the real
`ovt_tests_container`.) The fuzz target `fuzz_mpegts` was driven with 60 000 mutated inputs of its five seeds
under ASan/UBSan without a finding; libFuzzer itself is not installed here and runs in CI.
