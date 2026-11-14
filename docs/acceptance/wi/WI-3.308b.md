# WI-3.308b acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64 (4 vCPU), GCC 13.3, `linux-x64-debug` configuration with system
packages plus flatbuffers 25.2.10, libvmaf 3.0.0 and json-schema-validator 2.3.0 built from source. Date: 2026-11-14.
Spec version: 1.0-rc24. Spec questions: #419 (this WI).

Automated tests (binary `ovt_tests_metrics`, label `metrics`; source `core/tests/metrics/test_metrics_identity.cpp`):

| Acceptance test | Result |
|---|---|
| 3a. ref == dist: `identity.all == 1`, `sha256_match == 1` | `identity of an identical pair is 1, the sequence verdict is 1` (8-bit and 10-bit, different row padding for the two pictures) |
| 3b. `syn_metrics_1080p_bitflip_f37`: `all < 1`, `sha256_match == 0`, `VQ-DIFF-0001` names the recorded sample | `identity finds the flipped sample of syn_metrics_1080p_bitflip_f37` ([needs-corpus]; reads `tests/corpora/compare_expected.json`). **Not run on the real 1080p corpus here**; run locally against a small stand-in corpus laid out like the real one (64 × 48, 40 frames, flipped sample in frame 37), 203 assertions pass. The same logic is covered without corpus by `identity counts differing samples and reports the first one once` |

Further cases: descriptor and registration of `identity` in `register_builtin_kernels()`, option and input rejection,
block maps (fraction identical with partial edge blocks), 4:0:0, 10-bit high-byte flip, `finish()` without frames.

## Informational throughput

`ovt_tests_metrics "[bench][identity]"`, 1080p 8-bit 4:2:0, one thread, **unoptimised
`linux-x64-debug` build** (so a floor, not the budget machine):

```console
identity 1080p, identical: 4187 fps (1 thread)
identity 1080p, one sample off: 3784 fps (1 thread)
```

FR-VQ-023 asks `identity` for >= 500 fps at 1080p 8-bit on the M1 reference (`needs-hardware:m1-air-8gb`; no
benchmark binary exists yet, WI-0.560).

## Deviations (all in the PR body and issue #419)

* No SHA-256 code: `sha256_match` is derived from the sample comparison (#419 point 1).
* `VQ-DIFF-0001` leaves `identity` through a process-wide `DiagSink`, as the maps leave through the `MapSink` (#419 point 2).
