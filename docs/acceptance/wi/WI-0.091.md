# WI-0.091: disk cache, acceptance test 3 (size and write throughput)

Command: `ovt_tests_cache "[slow]"` (test case `test_cache: AT3 100 frames of 1080p grids take at most 100 MB on
disk`; 100 FrameAnalysis records with a dense 480 x 270 CellGrid, every vector filled).

Output: `AT3: 440 MB of FrameAnalysis -> 26 MB on disk (ratio 16.4), write throughput 734.61 MB/s uncompressed`
(zstd level 3 on one thread, Debug build). Budget: total ≤ 100 MB met; ≥ 200 MB/s met on this machine. The
`bench_cache_write` number for `m1-air-8gb` (chapter 14 §14.9.3) is recorded by the perf harness (WI-0.040
follow-ups) when the Mac Mini runner picks up the benchmark; it is not claimed here.

Date: 2026-11-08. Machine: cloud container, Ubuntu 24.04 x86_64 (4 vCPU), GCC 13.3, system zstd 1.5.5.
