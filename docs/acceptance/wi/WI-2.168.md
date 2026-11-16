# WI-2.168 acceptance record

Spec version: 1.0-rc35. Questions and assumptions: issues #596 (the non-WPP corpus stream) and #597 (libde265 flags,
the non-WPP note, where this record lives). Measurements and analysis: the "Performance" section of
`plugins/ovt-plugin-de265/README.md`.

## Test 1: 60-frame 1080p GOP via the host on the M1 reference ≤ 1.5 s

Automated: `bench_analysis_de265_gop60_1080p` (`tests/perf/de265/`), budget row of `tests/perf/budgets.json`, gated by
`tools/perf_compare.py` in `perf-nightly.yml` once the `m1-air-8gb` runner is registered (WI-0.560).

Stand-in record (no `m1-air-8gb` runner yet). 2026-11-16 05:38 UTC, Apple M4 Mac mini (Mac16,10, 4P+6E, macOS 15.8.2),
`macos-arm64-release` build of branch `claude/project-thread-gswioc` at ddb7040. The WI-0.560b harness was a local shim,
and the corpora came from `tests/corpora/synthetic/generate.py` in the oracle image (x265 4.1). The machine was busy
(load 46–85 from three CI runners), so these wall times overstate a quiet machine.

```text
OVT_CORPORA_DIR=$C build/macos-arm64-release/tests/perf/de265/bench_analysis_de265_gop60_1080p
# OVT_DE265_OPT_FLAGS="-O3 -mcpu=apple-m1" (the macOS arm64 default)
bench_analysis_de265_gop60_1080p/iterations:2/repeats:5_median   1402 ms   726 ms   5 frames=60 syn_h265_gop60_1080p
bench_analysis_de265_gop60_1080p/iterations:2/repeats:5_cv       4.04 %    3.24 %
```

Result: 1.40 s ≤ 1.5 s on a busy M4. Still to do: a quiet-machine rerun, then the real M1 on the nightly runner.

## Test 2: Pixel Tablet time recorded and the GUI progress requirement confirmed

Deferred to #610 (WI-2.168b): needs the `pixel-tablet` device (`needs-hardware:pixel-tablet`) and the Android
build of the plugin. The README's estimate is 2–3 s against the 5 s budget. When the device is available, run the same
binary through `tools/perf_run.py --android` and add the time and the GUI progress/cancel check here.
