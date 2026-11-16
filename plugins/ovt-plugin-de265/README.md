<!-- SPDX-FileCopyrightText: 2026 The Open Video Tools contributors -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# ovt-plugin-de265

The HEVC analysis plugin: `libovt_plugin_de265.{so,dylib}`. Specified in `docs/spec/07-bitstream-and-decoders.md`
§7.6.2. Licence LGPL-3.0-or-later.

- **Upstream:** libde265 1.0.15, pinned in `third_party/VERSIONS.md`. It is built static and position-independent by
  `third_party/libde265.cmake` with `-DENABLE_SDL=OFF -DENABLE_DECODER=OFF -DENABLE_ENCODER=OFF -DBUILD_SHARED_LIBS=OFF
  -DDE265_INTERNALS=ON`, and `-fno-sanitize=shift-base` under UBSan (issue #236).
- **Patches:** `patches/series`. Patch 0001 adds the read-only internals API (WI-2.162). Patch 0003 adds opt-in
  plane snapshots (WI-2.166): pre-deblocking, pre-SAO, prediction and dequantised coefficients.
- **Planes:** `Reconstruction`, and with patch 0003 `Prediction`, `Residual` (int16, `PreDeblock − Prediction`, so
  `Prediction + Residual == PreDeblock` exactly), `PreDeblock`, `PreSaoOrCdef` and `Coefficients` (int32). PCM and
  `cu_transquant_bypass` coding units have prediction 0 by convention, so their residual is the reconstruction.
  libde265 takes the snapshots while a picture decodes and they cost about four pictures' worth of memory, so they
  are off until a `pull()` asks for one of those stages; from then on every picture that starts decoding carries
  them. Frames decoded before that request carry `Reconstruction` only (the host then reports `ANA-0001`).
- **Profiles:** Main, Main 10 and Main Still Picture. Push refuses RExt, SCC and multi-layer streams with
  `OVT_E_UNSUPPORTED` and Diagnostic `DEC-H265-0010 "use ovt-plugin-hm"`.
- **Sources:** `src/plugin.cpp` holds the ABI glue and builds the FrameAnalysis buffers. `src/decoder.cpp` drives
  libde265. `src/planes.cpp` copies the snapshots out and builds the `PlaneRef`s.
- **Tests:** `tests/`. `ovt_plugin_de265_smoke_tests` loads the plugin through the host's plugin registry and checks
  each reconstruction against the stream's decoded picture hash SEI. `ovt_plugin_de265_planes_tests` checks the
  patch-0003 stages against decodes with libde265's in-loop filters switched off.

HEVC is patent-encumbered, so this plugin is not part of the default downloads (`tools/license_policy.toml`
`[distribution]`).

## Performance

Measured with `bench_analysis_de265_gop60_1080p` (`tests/perf/de265/`, chapter 14 §14.9.3, WI-2.168). One run is a cold
GOP: a new host `Session` without a cache asks for the FrameAnalysis of decode index 59 with the Reconstruction plane.
The host then decodes all 60 pictures from the RAP, the plugin builds a FrameAnalysis for each one, and the host
post-processes the last. The streams are `syn_h265_gop60_1080p` (1080p Main, x265 defaults, so WPP is on) and
`syn_h265_gop60_1080p_nowpp` (the same with `--no-wpp`). Neither stream has tiles. Medians of 5 repetitions × 2
iterations. "Wall" is the budgeted `real_time`; "CPU" is the calling thread's CPU time.

| Machine | Configuration | Stream | 1 | 2 | 4 | 8 threads | Host default |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Apple M4 Mac mini (4P+6E), busy (load 46–85 from CI runners) | `-O3` | WPP, wall / CPU (ms) | 1860 / 751 | 1440 / 760 | 1350 / 788 | 1312 / 758 | 1419 / 747 |
| | | no WPP, wall / CPU (ms) | 1435 / 1099 | 1176 / 980 | 1320 / 1108 | 1346 / 1116 | — |
| | `-O3 -mcpu=apple-m1` (the default since WI-2.168) | WPP | 1674 / 737 | 1457 / 749 | 1458 / 740 | 1701 / 702 | 1402 / 726 |
| | | no WPP | 1515 / 1071 | 1487 / 1113 | 1471 / 1103 | 1530 / 1096 | — |
| x86-64 cloud container (Xeon 2.1 GHz, 4 vCPU, shared) | `-O3`, GCC 13 | WPP | 2297 / 1160 | 1991 / 1306 | 1970 / 1331 | 1741 / 1091 | 1668 / 1124 |
| | | no WPP | 2178 / 1796 | 2004 / 1782 | 2013 / 1792 | 1854 / 1600 | — |

Compiler flags on the M4, CPU time of the host-default WPP row: `-O2` 751 ms, `-O3` 747 ms, `-O3 -mcpu=apple-m1`
726 ms, `-O3 -mcpu=apple-m1 -flto=thin` 763 ms (ThinLTO links but buys nothing). Before WI-2.168 the release presets
(RelWithDebInfo) built libde265 at `-O2`. `third_party/libde265.cmake` now builds it with `OVT_DE265_OPT_FLAGS`: `-O3`,
plus `-mcpu=apple-m1` on macOS arm64 and `-march=armv8-a` on Android arm64, with no fast-math (#597). On x86, `-O2` and
`-O3` measure the same.

Findings:

- **Budget.** §0.7.4 allows 1.5 s on the M1 reference. The busy M4 meets it at 1.40 s wall. A quiet-machine run and the
  real `m1-air-8gb` runner are still to come (`docs/acceptance/wi/WI-2.168.md`). The M1 has slower cores than the M4, so
  expect the M1 figure to come out close to the budget.
- **Where the time goes.** libde265 1.0.15 has no arm64 SIMD: its NEON code is 32-bit ARM assembly that only the
  autotools build compiles. On arm64 it runs the scalar `fallback-*` kernels. Profiled on x86 with callgrind (non-WPP,
  1 thread), about a third of all instructions are the plugin's per-frame mapping (`map_blocks`, `fill_cells` and the
  internals getters of patch 0001). That work runs on the pulling thread between pictures. SAO (`apply_sao_internal`) is
  the largest single libde265 cost at about 10 %, then CABAC/residual decoding and weighted prediction. Bare libde265
  without the plugin decodes the same 60 pictures in 0.5 s (4 threads, WPP) to 0.8 s (1 thread) on that x86 container.
- **Threads.** WPP scales weakly beyond 2 threads, because the serial mapping, not the decoder, sets the pace. A non-WPP,
  non-tiled stream still gains from worker threads: libde265 runs deblocking and SAO as per-CTB-row tasks on the
  workers, while parsing and reconstruction stay on the decoding thread (#597; the GUI wording is "stream has no
  WPP/tiles — limited multi-threading"). The host default (§7.5.3 A3: min(4, hardware threads), 2 on Android) stays.
- **Android (estimate, not measured).** The Pixel Tablet's fastest cores (Tensor G2, Cortex-X1) are roughly 1.7–2× slower
  per thread than an M4 performance core. From the M4's 0.75–1.1 s of calling-thread CPU, the estimate is 2–3 s per
  60-frame GOP with 2 threads, against the 5 s `pixel-tablet` budget. That is within budget, and well short of the "more
  than 2× over budget" mark at which WI-2.168 step 4 would propose NEON work items. So no NEON work items are proposed
  now; the device measurement (acceptance test 2) decides.
- **Follow-ups worth more than compiler flags.** (1) Map frame N on a helper thread while libde265 decodes frame N+1,
  which takes the third of the work spent on mapping off the critical path. (2) If the tablet misses its budget, the
  first NEON candidate is SAO (`sao.cc`), then the `fallback-motion.cc` weighted prediction, offered upstream as a
  separate contribution (§7.6.2 mitigation 6).
