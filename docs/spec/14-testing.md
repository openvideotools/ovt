# 14 — Testing and quality assurance

> **Scope.** This chapter defines how every part of OVT is tested: the test pyramid and the Definition of Done, the test infrastructure (frameworks, directories, corpus fetching, tags), the corpora catalogue, oracle-based golden tests, decoded-picture correctness, `FrameAnalysis` validation, fuzzing, sanitizers and static analysis, performance tests, GUI tests, the per-phase acceptance procedure, and the work items that build all of it. It conforms to the backbone (§0.4 layout, §0.7 rules, §0.8 phases). Where this chapter names a module, a schema table or a CLI subcommand, the name is the one in the backbone or in chapters 06, 07, 08, 09 and 12.
>
> **Audience.** Implementers (coding agents) and reviewers. Every rule here is testable; every oracle has an exact command.

---

## 14.1 Test strategy and Definition of Done

### 14.1.1 The pyramid

OVT uses five test layers. Lower layers are more numerous and faster; upper layers are fewer and slower. A work item (WI) is never "done" on the strength of an upper layer alone.

```
            +---------------------------+
            |  Acceptance (per phase)   |  manual scripts + CI replays, §14.11
            +---------------------------+
            |  GUI / platform tests     |  Qt Quick Test, screenshots, Android smoke, §14.10
            +---------------------------+
            |  Performance tests        |  budgets from §0.7.4, §14.9
            +---------------------------+
            |  Golden / oracle tests    |  OVT output == reference tool output, §14.4–14.6
            +---------------------------+
            |  Unit tests + fuzz        |  Catch2, libFuzzer, sanitizers, §14.2, §14.7, §14.8
            +---------------------------+
```

| Layer | What it proves | Runs where | Time budget |
|---|---|---|---|
| Unit | one function/class behaves per its contract (BitReader, box decoders, kernels, pooling) | every PR, every platform in the CI matrix | whole suite ≤ 5 min per platform |
| Golden / oracle | OVT's parse/analysis/metric output equals an independent reference on real streams | every PR (`fast` subset) and nightly (`slow`, whole corpora) | fast ≤ 10 min; nightly ≤ 3 h |
| Fuzz | parsers never crash, hang or read out of bounds on hostile input | 60 s smoke per target on every PR; continuous on OSS-Fuzz | smoke ≤ 15 min total |
| Performance | §0.7.4 budgets are met and no > 20 % regression | nightly on reference hardware (self-hosted runners) | ≤ 1 h |
| GUI / platform | QML models, screenshots, interaction scripts, Android install/render | every PR (models + screenshots on Linux), nightly (Android emulator + device farm) | ≤ 20 min |
| Acceptance | a phase's exit demo (§0.8) works end to end | at phase end, by a human following §14.11, recorded in `docs/acceptance/<phase>.md` | — |

### 14.1.2 Definition of Done (normative)

A work item is **Done** only when all of the following hold. Reviewers MUST reject a PR that claims completion without them. CI enforces items 1–5 mechanically.

1. **Unit tests pass** on every CI platform (`ctest --preset <platform> -L fast` green; see §14.2.5 for labels).
2. **Golden tests pass** for every module the WI touches (`ctest -L golden` green, including `needs-corpus` tests on Linux x86_64 and macOS arm64).
3. **A fuzz target exists** for every new or modified parser (anything that consumes untrusted bytes: syntax parsers, box decoders, splitters, importers, JSON option parsers). The target is listed in `core/tests/fuzz/CMakeLists.txt` (backbone §0.4, D14) and ran for 60 s in CI without a finding (§14.7).
4. **Sanitizer-clean:** the `linux-x64-asan-ubsan` preset job is green for the WI's tests; code touching threads is also green under `linux-x64-tsan` (§14.8).
5. **Docs updated:** the chapter(s) in `docs/spec/` that specify the behaviour are amended in the same PR when the implementation deviates from them (an "Open question" is filed instead if the deviation needs the owner); public C API changes are reflected in `docs/api/`.
6. **The WI's acceptance tests were executed and recorded.** Each Given/When/Then line of the WI is either an automated test (name the test in the PR description) or a manual step whose result is recorded in `docs/acceptance/wi/WI-<phase>.<nnn>.md` with the command, the output, the date and the machine.
7. **No new warnings** under the `-Werror` set of chapter 13 §13.2.5 (the `ovt::flags` interface target; §14.8.3 only points there, D24) and no new clang-tidy findings on changed lines.
8. **Licence gate green:** `tools/license_gate.py` reports no forbidden licence in the dependency tree or in `tests/corpora/manifest.json` (§14.3.4).

### 14.1.3 Determinism rule for tests

Because §0.7.5 requires byte-identical FlatBuffers for identical input, every golden test for a FlatBuffer output compares **bytes** after the normalisation in §14.4 (JSON is only used where the oracle is JSON). A test that compares floating-point metric scores states its tolerance explicitly in the test name (`..._tol1e-6`). Tests MUST NOT depend on wall-clock time, locale, the current directory, environment variables other than `OVT_CORPORA_DIR`, or thread scheduling (run analysis with `threads=1` in golden tests; a separate `threads=4` test asserts equality with the `threads=1` output).

---

## 14.2 Test infrastructure

### 14.2.1 Frameworks

| Language / layer | Framework | Version pin | Why |
|---|---|---|---|
| C++ (core, render, plugins, CLI) | **Catch2 v3** (`catch2` vcpkg port, ≥ 3.5.0) | pinned in `vcpkg.json` | header-light, tags, generators, built-in `BENCHMARK` (used for micro-benchmarks only; macro performance uses §14.9 harness), JUnit reporter for CI |
| Python (`tools/`, `python/ovt`, oracle drivers) | **pytest** ≥ 8.0 with `pytest-xdist` | `tools/requirements-test.txt` | standard; parametrised corpus tests |
| QML / GUI | **Qt Quick Test** (`QUICK_TEST_MAIN_WITH_SETUP`) from Qt 6.8 | Qt pin in `CMakePresets.json` | runs QML `TestCase` items headless with `QT_QPA_PLATFORM=offscreen` |
| Android | **AndroidX Test + UI Automator 2.3** (instrumentation APK) | `android/app/build.gradle.kts` | install/open/render/screenshot smoke |
| Fuzzing | **libFuzzer** (clang `-fsanitize=fuzzer`), `OVT_ENABLE_FUZZ=ON` | clang ≥ 17 | OSS-Fuzz compatible |
| Performance | **Google Benchmark** ≥ 1.8.3 (`benchmark` vcpkg port) | `vcpkg.json` | JSON output, `--benchmark_compare` tooling, stable statistics |
| Screenshot diff | own `tools/imgdiff.py` (NumPy + `scikit-image` SSIM) | `tools/requirements-test.txt` | perceptual threshold, see §14.10.2 |

Decision: Google Benchmark is used for macro performance tests (§14.9) because its JSON output and `compare.py` give regression comparison for free; Catch2 `BENCHMARK` is allowed inside unit tests only for developer micro-measurements and is never gated.

**Catch2 linkage rule (D24; stated here and in WI-0.550 only, chapter 13 §13.2.1 cites it).** Every ordinary test executable (`ovt_tests_<module>` under `render/tests/`, `plugins/*/tests/`, `cli/tests/`, `app/tests/models/`, and every `core/tests/<module>/` executable other than the golden runner) links `Catch2::Catch2WithMain` and has no `main` of its own. The single exception is the golden runner `ovt_core_tests` (`core/tests/main.cpp`): it links `Catch2::Catch2` and supplies its own `main`, because it must install the `ovt_set_logger` hook and read `OVT_UPDATE_GOLDEN`, `OVT_REQUIRE_CORPORA` and `OVT_CORPORA_DIR` before the Catch2 session starts. No other test target defines `main`.

### 14.2.2 Directory layout

The backbone fixes `/core/tests/` and `/tests/corpora/`. This chapter adds the siblings under `/tests/`. All paths are repository-relative.

```
/core/tests/                       Catch2 unit tests for libovt-core, one directory per module
  CMakeLists.txt                   add_executable(ovt_core_tests ...) + catch_discover_tests()
  main.cpp                         own main of the golden runner (links Catch2::Catch2; the only test main, §14.2.1)
  test_support/                    helpers: corpus lookup, FlatBuffer normalisation, temp dirs
    corpus.h / corpus.cpp          ovt::test::corpus_path(id) → std::filesystem::path (skips test if absent)
    fb_normalize.h / .cpp          strip/zero non-deterministic fields; verify FlatBuffer with Verifier
    golden.h / .cpp                compare_or_update(golden_path, bytes) honouring OVT_UPDATE_GOLDEN=1
  bitreader/   test_bitreader_basic.cpp, test_bitreader_expgolomb.cpp, test_bitreader_epb.cpp
  container/   test_container_isobmff_boxes.cpp, test_container_isobmff_sampletable.cpp, test_container_ivf.cpp, ...
  syntax/      test_syntax_h264_sps.cpp, test_syntax_h265_slice.cpp, test_syntax_av1_frame_header.cpp, ...
  index/       test_index_h264_poc.cpp, test_index_av1_hidden_frames.cpp, ...
  analysis/    test_analysis_host.cpp, test_analysis_validator.cpp
  metrics/     test_metrics_psnr.cpp, test_metrics_ssim.cpp, test_metrics_pooling.cpp, test_metrics_bdrate.cpp
  color/       test_color_nits.cpp
  cache/, jobs/, io/, model/
  fuzz/                            libFuzzer targets (§14.7; backbone §0.4 — NOT under /tests/)
    CMakeLists.txt                 ovt_add_fuzz_target(name)
    fuzz_<target>.cpp              source of the executable ovt_fuzz_<target> (chapter 13 §13.2.1 naming)
    corpus/<target>/               committed minimal seed corpus (≤ 64 KiB each, ≤ 2 MiB per target; chapter 13 §13.7.1 fuzz-smoke reads it)
    dictionaries/<target>.dict
    regressions/<target>/          minimised crash inputs replayed by fuzz_<target>_regress
    oss-fuzz/                      project.yaml, Dockerfile, build.sh (WI-0.569)
/render/tests/                     Catch2 tests for libovt-render (offscreen Vulkan/MoltenVK; tag needs-gpu)
/plugins/<plugin>/tests/           per-plugin Catch2 tests (tag needs-corpus)
/app/tests/                        Qt Quick Test (QML) + screenshot + interaction scripts (§14.10)
  qml/tst_*.qml
  screenshots/<platform>/<test>.png
  scripts/*.py                     interaction scripts driving ovt-studio via the test IPC socket
/android/app/src/androidTest/      instrumentation smoke tests
/python/tests/                     pytest for the Python module
/tests/
  CMakeLists.txt                   adds golden and perf subdirectories conditionally (fuzz lives in core/tests/fuzz/)
  golden/                          golden files + the CMake/ctest glue that produces and compares them
    CMakeLists.txt
    h264/   <corpus-id>.<tool>.<format>  e.g. jvt_BA1_Sony_D.trace_headers.txt, jvt_BA1_Sony_D.ovt-syntax.json
    h265/
    av1/
    isobmff/
    metrics/
    README.md                      how to regenerate (OVT_UPDATE_GOLDEN=1), which oracle version produced each file
  oracles/                         oracle programs and drivers (§14.4; chapter 9 §9.11.2 uses the same directory)
    gst_parse_oracle.c             GStreamer codecparsers oracle (§14.4.2)
    ffmpeg_mvs.c                   libavcodec motion-vector dumper (§14.4.3a)
    hm_cu_dump.cpp                 HM getter dumper (§14.4.4)
    x264_ssim/                     vendored x264 SSIM kernel oracle (§14.4.12; GPL, oracle only)
    mp4boxjs_dump.mjs              mp4box.js oracle script (§14.4.9)
    mpv_capture.py                 mpv/libplacebo render capture (§14.4.15)
    build_jm.sh, build_hm.sh, build_libaom_inspect.sh, build_ffmpeg_oracle.sh
    requirements.txt               pinned Python oracle packages (bjontegaard, siti-tools, uvq, tf2onnx)
    normalize/*.py                 oracle-text → canonical JSON converters
  perf/                            Google Benchmark binaries + baselines + budgets.json (§14.9; WI-0.560 harness, names per §14.9.3; D14)
    CMakeLists.txt
    budgets.json                   the §14.9.3 table in machine form (single source for every chapter's budget)
    core/      bench_index.cpp, bench_first_frame.cpp, bench_analysis.cpp, bench_container.cpp, bench_memory.cpp
    render/    bench_render_video.cpp, bench_overlay.cpp (chapter 10 §10.9.3)
    metrics/   bench_metrics.cpp (chapter 9 §9.11.3)
    gui/       bench_gui_open_first_frame.cpp, bench_gui_scrub.cpp, bench_gui_cold_start.cpp (chapter 11 §11.9)
    data/      fa_4k_dense.bin and other generated inputs (never committed when > 5 MiB; see §14.3.3 "Non-stream fixtures")
    baselines/<hardware-id>.json
  corpora/
    manifest.json                  all corpus entries (§14.2.4)
    synthetic/                     generator scripts for OVT's own streams (§14.3.3)
      generate.py, recipes.json
    LICENSES.md                    per-corpus licence/terms summary (generated by tools/license_gate.py)
/tools/
  fetch_corpora.py                 downloads/verifies corpora (§14.2.4)
  license_gate.py                  dependency + corpora licence check
  imgdiff.py                       screenshot perceptual diff
  perf_run.py, perf_compare.py     benchmark driver and baseline comparison for §14.9
  gen_dense_fa.py, gen_fixture_fb.py  generators of the non-stream fixtures of §14.3.3
  check_fast_subset.py             fixture-registry checker (§14.3.5; WI-0.553)
  corrupt.py, make_broken.py, sei_inject.py, y4m_poke.py, av1_set_level.py, make_multi_pps.py
                                   deterministic fixture mutators used by §14.3.3 recipes (WI-0.553)
  yuview_csv_lint.py               YUView statistics-CSV linter (WI-2.584; D27)
  ref_annexb.py, extract_samples.py  container oracles/helpers produced by chapter 8 WI-1.226 / WI-1.231 (D27; listed here for completeness)
  tests/fixture_server.py          local HTTP fixture server for the fetcher tests and DASH/HLS acceptance tests (WI-0.551; D27)
```

### 14.2.3 Naming conventions

| Thing | Rule | Example |
|---|---|---|
| Unit test file | `test_<module>_<topic>.cpp` | `test_syntax_h265_sps.cpp` |
| Catch2 test case name | `"<module>/<topic>: <behaviour>"` | `"syntax/h265_sps: parses conformance_window_flag"` |
| Golden file | `<corpus-id>.<tool>.<format>` where `<tool>` is the oracle id from §14.4 or `ovt-<subcommand>` | `jctvc_AMP_A_Samsung_7.trace_headers.txt`, `jctvc_AMP_A_Samsung_7.ovt-syntax.json`, `gpac_counter.mp4box-diso.xml` |
| Corpus id | `<source>_<name>`, `[A-Za-z0-9_]+`, ≤ 64 chars (`<source>` lower-case; `<name>` keeps the upstream file stem's case with `-`/`.` mapped to `_`); `<source>` ∈ {`jvt`, `jctvc`, `aom`, `argon`, `mpegff`, `gpac`, `bento4`, `dashif`, `cta`, `apple`, `netflix`, `ytugc`, `hdr`, `heif`, `ts`, `mkv`, `misc`, `syn`}; derived synthetic files keep the `syn_` prefix (`syn_broken_*`, `syn_mp4_broken_*`, `syn_metrics_*`, `syn_level_violation_*`); conformance ids are the prefix plus the upstream file stem (`jvt_CVSE2_Sony_B`, `jctvc_RPS_F_docomo_2`; AOM vector `av1-1-b8-22-svc-L2T2` → `aom_av1_1_b8_22_svc_L2T2`) | `aom_av1_1_b8_01_size_16x16`, `syn_h265_tiles_2x2` |
| Fuzz target | source `fuzz_<module>_<unit>.cpp`, executable `ovt_fuzz_<module>_<unit>` (chapter 13 §13.2.1) | `fuzz_syntax_h264_sps.cpp` → `ovt_fuzz_syntax_h264_sps` |
| Benchmark | `bench_<area>_<what>` | `bench_index_h264_1h_1080p` |
| QML test | `tst_<Component>.qml` | `tst_SyntaxTreeModel.qml` |
| Screenshot | `<platform>/<test>.<theme>.<dpi>.png` | `linux/syntax_tree_selected.dark.96.png` |

### 14.2.4 How tests get corpus files

Corpus files are never committed. `tools/fetch_corpora.py` downloads them on demand into a cache directory and verifies SHA-256. Tests look files up through `ovt::test::corpus_path(id)`; if the file is absent and the test is tagged `needs-corpus`, the test is **skipped** (Catch2 `SKIP()`), not failed, except in CI jobs where `OVT_REQUIRE_CORPORA=1` turns a skip into a failure.

**Manifest** — `tests/corpora/manifest.json` (JSON Schema at `tests/corpora/manifest.schema.json`):

```json
{
  "schema_version": 1,
  "entries": [
    {
      "id": "jctvc_AMP_A_Samsung_7",
      "source": "jctvc",
      "url": "https://www.itu.int/wftp3/av-arch/jctvc-site/bitstream_exchange/draft_conformance/HEVC_v1/AMP_A_Samsung_7.zip",
      "sha256": "<64 hex chars>",
      "size_bytes": 123456,
      "licence": "ITU-T conformance bitstream; free to download; redistribution not permitted — do not mirror",
      "licence_spdx": "LicenseRef-ITU-Conformance",
      "redistributable": false,
      "codecs": ["h265"],
      "containers": ["annexb"],
      "extract": { "archive": "zip", "member": "AMP_A_Samsung_7.bit", "md5_member": "AMP_A_Samsung_7_yuv.md5" },
      "features": ["amp", "main"],
      "tests": ["syntax", "index", "md5"],
      "tags": ["slow"]
    }
  ]
}
```

Field rules: `id` unique; `url` is the exact download URL (HTTPS or a `gs://`/`https://storage.googleapis.com/` URL); `sha256` is the hash of the **downloaded file** (archive or stream); `extract` is optional and names the member to use; `licence` is a human sentence; `licence_spdx` is an SPDX id or a `LicenseRef-*`; `redistributable=false` means CI must download from the origin, never from an OVT mirror; `size_bytes` is used for the CI cache budget; `features` are lower-case feature tags from §14.3.3's vocabulary; `tests` names which golden suites consume the entry.

**Fetcher CLI** (`tools/fetch_corpora.py`, Python ≥ 3.11, stdlib + `requests`):

```
fetch_corpora.py [--manifest tests/corpora/manifest.json] [--cache-dir DIR]
                 [--ids ID[,ID...]] [--sources SRC[,SRC...]] [--tags TAG[,TAG...]]
                 [--max-total-bytes N] [--verify-only] [--jobs N] [--retries 3]
```

- Cache dir precedence: `--cache-dir` > `$OVT_CORPORA_DIR` > `<cache_root>/corpora`, where `<cache_root>` is the platform cache root of chapter 06 §6.7 (D32): `$XDG_CACHE_HOME/ovt/` on Linux (`~/.cache/ovt/` when unset), `~/Library/Caches/org.openvideotools.ovt/` on macOS. Layout: `<cache>/<id>/<member-or-filename>` plus `<cache>/<id>/.ok` written only after SHA-256 verification.
- Exit codes: 0 all present/verified; 2 hash mismatch (file deleted, re-download attempted up to `--retries`); 3 network failure; 4 manifest invalid.
- `--verify-only` re-hashes without network; used by the CI cache-restore step.
- Downloads are untrusted: the fetcher never executes anything, extracts only the named `member` with path-traversal checks (`zipfile` member names validated against `..` and absolute paths), and writes to a temp file first.

**CI cache.** GitHub Actions caches `<cache>` keyed by `corpora-${{ hashFiles('tests/corpora/manifest.json') }}-${{ runner.os }}`; PR jobs fetch `--tags fast` only (≤ 300 MiB total, enforced by `--max-total-bytes 314572800`); nightly jobs fetch everything (≈ 12 GiB, see §14.3). Cache misses download from origin; a nightly `corpora-health` job runs `--verify-only` on the full set and opens an issue when a URL has gone dead.

### 14.2.5 Test tags

Catch2 tags (and the equivalent ctest `LABELS`) partition the suite. Every test case carries exactly one speed tag and any number of resource tags.

| Tag | Meaning | PR CI | Nightly |
|---|---|---|---|
| `[fast]` | ≤ 1 s, no external files beyond `tests/golden` and committed seeds | run | run |
| `[slow]` | > 1 s or iterates a corpus | skipped unless `-L slow` | run |
| `[needs-corpus]` | calls `corpus_path()`; skipped if the file is absent | run on fast corpus | run on full corpus |
| `[needs-gpu]` | needs Vulkan/MoltenVK (offscreen) | run on macOS runner; Linux via lavapipe (`VK_ICD_FILENAMES` set) | run |
| `[needs-display]` | needs a window system (not offscreen) | skipped | run on self-hosted macOS |
| `[needs-android]` | runs on an emulator/device | skipped | run (emulator x86_64 API 36 `google_apis` image — chapter 13 §13.7.1 `android-emulator-smoke` — plus device farm arm64) |
| `[needs-android-hdr]` | needs the `android-hdr` (or `chromebook-hdr`) device of §14.9.2 | skipped | run weekly on the lab device |
| `[needs-oracle:<name>]` | requires an oracle binary (`ffmpeg`, `ffprobe`, `MP4Box`, `mp4ff-info`, `mediainfo`, `node`, `vmaf`, `ldecod`, `TAppDecoder`, `inspect`, `aomdec`, `dav1d`, `hm_cu_dump`, `gst_parse_oracle`, `ffmpeg_mvs`, `x264_ssim`, `mpv`, `mkvinfo`, `mkvmerge`, `tsp`, `tstables`, `tsanalyze`, `mp4dump`) | skipped if absent | required (oracle Docker image) |
| `[golden]` | compares against `tests/golden` | run | run |
| `[fuzz-regress]` | replays a crash input from `core/tests/fuzz/regressions/` | run | run |

ctest labels mirror the tags (`fast`, `slow`, `golden`, `perf`, `needs_corpus`, `needs_display`, `needs_android`, `no_sanitizer`, `canary`). The ctest test presets used by this chapter are exactly the lane presets of chapter 13 §13.2.4 (D23; that file is the single source and this chapter only names them): `pr` (label `fast`, excluding `needs_display|needs_android|canary`), `nightly` (everything except `canary`), `sanitizers` (labels `fast|golden` under the `linux-x64-asan-ubsan` configure preset and the ASan/UBSan environment), `perf` (label `perf`, one job at a time); the macOS variants are `pr-macos`, `nightly-macos`, `sanitizers-macos` (configure preset `macos-arm64-asan`) and `perf-macos`. Configure presets named in this chapter: `linux-x64-debug`, `linux-x64-release`, `linux-x64-asan-ubsan`, `linux-x64-tsan`, `linux-x64-fuzz`, `macos-arm64-release`, `macos-arm64-asan`, `android-arm64-release`, `android-x86_64-debug`.

### 14.2.6 Oracle Docker image

Nightly and golden-regeneration jobs run in `ghcr.io/openvideotools/ovt-oracles:<yyyymmdd>` built from `tests/oracles/Dockerfile` (Ubuntu 24.04) containing exact versions of: FFmpeg (built from source at a pinned tag with `--enable-gpl --enable-libvmaf --enable-libx264 --enable-libx265 --enable-libaom`), GStreamer codecparsers (`libgstreamer-plugins-bad1.0-dev`), GPAC (`MP4Box` built from a pinned tag), `mp4ff-info` (Go), `mediainfo` CLI, Node 20 + `mp4box` npm package, `vmaf` CLI, JM `ldecod` (TRACE build), HM `TAppDecoder` (ENC_DEC_TRACE build), libaom `inspect` (CONFIG_INSPECTION), Daala `dump_psnrhvs`/`dump_ciede2000`, the vendored x264 SSIM kernel oracle `x264_ssim` (§14.4.12), `ffmpeg_mvs` (§14.4.3a), mpv with libplacebo, **TSDuck** (`tsp`, `tstables`, `tsanalyze`; the MPEG-TS oracle of chapter 8 WI-4.205–WI-4.207 and WI-4.597; installed from the pinned upstream `.deb` release, version in `ORACLE_VERSIONS.md`), **MKVToolNix** (`mkvmerge`, `mkvinfo`), **Bento4** (`mp4dump`, `mp4encrypt`, `mp4fragment`; used by §14.3.3 recipes), `dav1d`, the standalone encoders `x264`, `x265`, `aomenc` and HM `TAppEncoder` of §14.3.3, Python with the `tests/oracles/requirements.txt` pins (`bjontegaard`, `siti-tools`, `uvq` TensorFlow CPU requirements, `tf2onnx`). Third-party tools are installed by WI-0.554; the in-house oracle programs (`gst_parse_oracle`, `hm_cu_dump`, `ffmpeg_mvs`, `x264_ssim`, `mpv_capture.py`) are added to the image by the WIs that write them (WI-0.562, WI-0.563, WI-0.565, WI-1.575), each extending the Dockerfile's `oracles` stage. `tiny_ssim` is **not** an oracle (chapter 9 §9.4.4, D10). Versions are listed in `tests/oracles/ORACLE_VERSIONS.md` and printed into every golden file header (`# oracle: ffmpeg n7.1.1 ...`).

---
## 14.3 Corpora catalogue

### 14.3.1 Public corpora

Sizes are approximate totals of the entries OVT uses (not of the whole upstream set). "Terms" summarises the usage condition recorded in `manifest.json`; `tools/license_gate.py` fails CI if an entry has `redistributable=true` without an SPDX identifier that permits redistribution.

| Source id | Corpus | URL | Licence / terms | Codecs / containers | Size | What it tests |
|---|---|---|---|---|---|---|
| `jvt` | JVT H.264 conformance (ITU-T H.264.1 bitstreams, `professional_profiles` and base `draft_conformance`) | https://www.itu.int/wftp3/av-arch/jvt-site/draft_conformance/ | ITU-T conformance material; free download for testing; not redistributable (`redistributable=false`) | H.264 Baseline/Main/High/High10/High422/High444, Annex B; some `.264`/`.jsv`/`.avc` | ≈ 1.9 GiB full; `fast` subset 25 streams ≈ 60 MiB | SPS/PPS/slice-header syntax (CAVLC+CABAC, MBAFF, PAFF, FMO/ASO in Baseline, weighted pred, long-term refs, multiple slices, 8×8 transform, scaling lists), POC derivation, decoded MD5 (`*_yuv.md5` or `.yuv` reference) |
| `jctvc` | JCT-VC HEVC_v1 conformance (plus `RExt`, `SCC`, `MV-HEVC` folders for the HM oracle) | https://www.itu.int/wftp3/av-arch/jctvc-site/bitstream_exchange/draft_conformance/HEVC_v1/ | ITU-T conformance; free download; not redistributable | HEVC Main/Main10 (v1), RExt 4:2:2/4:4:4 up to 16-bit, SCC; Annex B `.bit`/`.bin`; each zip has `*_yuv.md5` and often a `.txt` describing the stream | ≈ 2.5 GiB (v1 ≈ 1.1 GiB); `fast` subset 30 streams ≈ 80 MiB | VPS/SPS/PPS/slice syntax (AMP, SAO, WPP, tiles, dependent slices, long-term refs, ST-RPS, scaling lists, PCM, transquant bypass, VUI/HRD, decoded picture hash SEI), CU/PU/TU truth vs HM, MD5 |
| `aom` | AOM AV1 conformance / test vectors (`av1-1-b8-*`, `av1-1-b10-*`, `av1c-vs-*`, `av1-1-b8-22-svc-*`, `av1-1-b8-23-film_grain-*`, `av1-1-b8-24-*`) | https://storage.googleapis.com/aom-test-data/ (file list in libaom `test/test_vectors.cc`; MD5 at `<name>.md5`) | AOM test data: BSD-2-Clause + AOM Patent License 1.0 (same as libaom) — redistributable | AV1 Main 8/10-bit, 4:2:0, IVF (`.ivf`), some `.obu`/Annex B (`av1-1-b8-01-size-*-annexb`), MP4 (`av1c-vs-*`) | ≈ 1.2 GiB; `fast` subset 40 small `size-NNxMM` vectors ≈ 5 MiB | OBU parsing, sequence/frame header syntax, hidden frames, `show_existing_frame`, intra-only frames, S-frames, SVC operating points, film grain, odd sizes, tiles, superres, `av1C` in MP4, decoded MD5 vs libaom/dav1d |
| `argon` | Argon AV1 conformance streams (AOM, originally Argon Design) | https://aomedia.org/argon/ (access terms to be verified at manifest time: the suite was announced as freely downloadable in 2023 but redistribution stays restricted; the manifest records `redistributable=false` and `manual_download=true` until WI-0.552 confirms a direct URL) | AOM membership terms; free for conformance testing | AV1 (all profiles/levels), Annex B `.obu` with per-stream `.md5` | ≈ 90 GiB full; OVT uses the `profile0_core` subset ≈ 4 GiB, nightly only | exhaustive syntax coverage (every syntax element/branch), decoder parity (libaom vs dav1d vs OVT index), fuzz seed source |
| `mpegff` | MPEG FileFormatConformance (ISO/IEC 14496-32) files | https://github.com/MPEGGroup/FileFormatConformance (files under `data/file_features/` via **Git LFS**; `git lfs pull --include "data/file_features/**"`) | BSD-3-Clause-Clear (repository); the media files are MPEG conformance material — treat as not redistributable outside the repo (`redistributable=false`) | ISOBMFF/MP4 (14496-12/-14/-15/-30, 23008-12 HEIF), CMAF | ≈ 3 GiB LFS; `fast` subset 40 files ≈ 100 MiB | box coverage (every registered box in the JSON gets ≥ 1 sample), FullBox versions/flags, `stsd` sample entries, HEIF item boxes, conformance-check diagnostics |
| `gpac` | GPAC test suite media | https://github.com/gpac/testsuite (`media/` folder, ≈ 1 GiB; use `git clone --depth 1`) | LGPL-2.1+ repository; media files carry their own notices in `media/README`; OVT records each used file's notice; `redistributable=false` unless `media/README` says otherwise | MP4/fMP4/DASH segments, HEVC/AVC/AV1 in MP4, timed text, `emsg`, `prft`, CENC | `fast` subset 30 files ≈ 60 MiB | fragment timelines (`tfdt`/`trun`/`sidx`), edit lists, `elst` ↔ `tfdt` sanity, CENC boxes, `MP4Box -diso/-dts/-dnal` parity |
| `bento4` | Bento4 test files | https://github.com/axiomatic-systems/Bento4/tree/master/Test/Data (and `Test/Data/*.mp4`) | GPL-2.0 repository; test media freely downloadable, treated as non-redistributable | MP4, fMP4, PIFF/CENC (`cenc`, `cbcs`), HLS/DASH outputs | ≈ 200 MiB; `fast` 10 files ≈ 20 MiB | CENC (`senc/saiz/saio/tenc/pssh`), `sgpd/sbgp(seig)`, `mp4dump --format json` cross-check |
| `dashif` | DASH-IF test vectors (conformance and live-sim) | https://testassets.dashif.org/ (index: https://dashif.org/guidelines/testvectors/, JSON list `https://testassets.dashif.org/vectors.json`) | DASH-IF test assets: free for conformance/interop testing; not redistributable | fMP4/CMAF segments (`styp`, `sidx`, `moof`), MPDs, AVC/HEVC/AV1 | downloaded on demand: `fast` 5 representations ≈ 150 MiB | DASH virtual file (phase 4), `sidx` ↔ segment byte ranges, multi-period, encrypted (`cenc`) |
| `cta` | CTA WAVE CMAF test content | https://dash.akamaized.net/WAVE/vectors/ (list: https://github.com/cta-wave/Test-Content; MPDs under `.../development/`) | CTA WAVE content for conformance; free use; not redistributable | CMAF (`cmfc`, `cmf2`), AVC/HEVC/AV1, HDR10/HLG variants, fragments with `prft`/`emsg` | ≈ 2 GiB; `fast` 4 tracks ≈ 120 MiB | CMAF brand checks, `clli`/`mdcv` in `colr`-carrying tracks, fragment duration consistency, CMAF chunk mode |
| `apple` | Apple HLS example streams (bipbop, advanced stream) | https://developer.apple.com/streaming/examples/ (e.g. https://devstreaming-cdn.apple.com/videos/streaming/examples/img_bipbop_adv_example_fmp4/master.m3u8) | Apple sample content, for testing; not redistributable | fMP4 (and TS for the legacy example) HLS, AVC/HEVC, HDR10 and Dolby Vision variants in the "advanced" example | on demand; `fast` 2 variants × 3 segments ≈ 30 MiB | HLS virtual file (phase 4), `dvcC`/`dvvC`, `hvcC` vs SPS consistency, TS tree (phase 4) |
| `netflix` | Netflix vmaf test resources | https://github.com/Netflix/vmaf/tree/master/python/test/resource (yuv in `resource/yuv/`, expected scores in `python/test/*_test.py` and `resource/model/`) | BSD-2-Clause-Patent (vmaf repo) — redistributable | raw YUV 8-bit 4:2:0 (`src01_hrc00_576x324.yuv`, `src01_hrc01_576x324.yuv`, …), 10-bit samples | ≈ 60 MiB | bit-exact VMAF/PSNR/SSIM/MS-SSIM parity with the `vmaf` CLI (§14.4.11) |
| `ytugc` | YouTube-UGC subset | https://media.withyoutube.com/ (download links per clip, 20 clips chosen from the `Gaming`, `Sports`, `Vlog`, `HDR` categories at 360p/720p/1080p) | CC BY 4.0 (dataset licence) — redistributable with attribution | raw YUV/MKV (clips are provided as `.mkv` raw 4:2:0 or `.yuv`) | ≈ 1.5 GiB; `fast` 3 clips @360p ≈ 150 MiB | UVQ parity with the reference CLI within 0.01 MOS (§14.4.14), no-reference pipeline, alignment search sanity |
| `hdr` | Public HDR test clips with known static metadata | (a) https://4kmedia.org/ "LG Chess" / "Sony Camp" HDR10 (MaxCLL 1000/4000 in `mdcv`/`clli` SEI); (b) https://www.arib.or.jp/ HLG samples via BBC R&D "HLG test card" https://www.bbc.co.uk/rd/projects/high-dynamic-range; (c) AOM `av1-1-b10-23-film_grain-50` (10-bit **film grain only** — this vector carries no HDR colour config and no HDR metadata OBUs; it is used for the film-grain decode/render paths, never for HDR metadata tests); (d) CTA WAVE HDR10/HLG tracks (see `cta`); AV1 HDR metadata OBUs come from the synthetic `syn_av1_pq_hdr_obu` recipe (§14.3.3) | per-clip terms (demo content; download for testing; not redistributable) — recorded per entry | HEVC Main10 PQ in MP4 with `mdcv`/`clli`, HLG in MP4 with `colr` nclx 9/18/9, AV1 with HDR metadata OBUs | ≈ 1.2 GiB; `fast` 2 clips trimmed ≈ 100 MiB (trimmed with `-c copy` by the fetcher's `post` step, see note) | HDR metadata panel (SEI/OBU vs box values), `ovt_ColorDescription` population, MaxCLL/FALL measurement (§14.4.15), HDR display parity |
| `heif` | HEIF/AVIF conformance images | https://github.com/nokiatech/heif_conformance (`conformance_files/`), https://github.com/AOMediaCodec/av1-avif/tree/master/testFiles | Nokia HEIF conformance: free for testing, not redistributable; av1-avif test files BSD-2-Clause — redistributable | HEIF (`hvc1` items, `grid`, `iden`, `iovl`), AVIF | ≈ 60 MiB | `meta`/`iinf`/`iloc`/`ipco`/`ipma`/`iref` decoding, derived items (chapter 8 §8.4.6, WI-1.214, WI-4.213) |
| `ts` | MPEG-TS captures | https://github.com/tsduck/tsduck-test (`input/*.ts`), https://streams.videolan.org/ (`streams/ts/`) | tsduck-test BSD-2-Clause — redistributable; VideoLAN samples: free for testing, not redistributable | H.264/HEVC/AV1 over TS, DVB SI | ≈ 200 MiB | packet sync, PSI/SI, PES, PCR/PTS, TR 101 290 (chapter 8 WI-4.204–WI-4.207; phase 4) |
| `mkv` | Matroska test suite | https://github.com/ietf-wg-cellar/matroska-test-files (`test_files/test1.mkv` … `test8.mkv`) | public test material of the Matroska project without a licence file — treated as not redistributable (`redistributable=false`) | Matroska/WebM: lacing, `Cues`, `Chapters`, `Tags`, attachments, a damaged file (`test7`) | ≈ 90 MiB; `fast` 3 files ≈ 25 MiB | EBML element tree vs `mkvinfo -v -v -z`, Matroska → `SampleTable` (chapter 8 WI-4.201–WI-4.203; phase 4) |
| `misc` | maintainer-recorded device captures | no URL: files recorded once by the maintainer on the devices of §14.9.2 and stored in the maintainers' private bucket; manifest entries have `manual_download=true` and the recorded SHA-256 | OVT-owned captures, CC0 | Pixel camera MP4 with Motion Photo trailer, iPhone HEVC `.mov` with QuickTime `meta/keys/ilst` | ≈ 60 MiB | real-world container edge cases of chapter 8 (§8.9.3); nightly only, `[needs-corpus]` (skipped when absent) |
| `syn` | OVT synthetic streams | generated locally by `tests/corpora/synthetic/generate.py` (§14.3.3); the fetcher produces them when `--sources syn` is requested and caches them like downloads; the manifest stores the SHA-256 of each generated file for a pinned encoder set | inputs are generated test patterns (`ffmpeg -f lavfi`) → OVT-owned, CC0 | all three codecs, all containers | ≈ 40 MiB | targeted features (tiles, WPP, AMP, SAO, CDEF, LR, film grain, hidden frames, MBAFF/PAFF, weighted pred, long-term refs, multiple slices, cropping, 10-bit, 4:2:2/4:4:4), fuzz seeds, GUI tests |

Note on trimming: for `hdr` entries larger than 100 MiB the manifest entry has `"post": {"ffmpeg_trim_seconds": 10}`; the fetcher runs `ffmpeg -i in -t 10 -c copy -map 0:v:0 out.mp4` with the pinned oracle FFmpeg and hashes the **trimmed** output under `sha256_post`. The original file's hash is still verified first.

**Named public-corpus fixtures.** Acceptance tests in chapters 4, 7, 8, 9, 10, 11, 12 and 16 name individual files by a manifest id; chapter 8 WI-1.237 adds the container entries and WI-0.552 all others to `tests/corpora/manifest.json` (`source`, upstream path, SHA-256, licence tag) from the sources above and of chapter 8 §8.9.3. An acceptance test MUST NOT name a file that is not in this chapter (D26): every name used anywhere in the specification is listed in the fixture index of §14.3.5 with its source and `fast` membership. The named public files and what they are (`fast` column: ✓ = in the `fast` subset):

| Fixture id | Source | File (upstream) | Why it is named |
|---|---|---|---|
| `cta_cmaf_video_cmfv` | `cta` | one CTA WAVE CMAF video track file (`cmfv`/`cmfc` brands, `tfdt`, single-`trun` fragments); ✓ | fragment expansion golden (chapter 8 WI-1.211), CMAF brand conformance |
| `syn_cmaf_bad_elst_cmfv` | `syn` (derived) | `tools/make_broken.py --elst-media-time` over `cta_cmaf_video_cmfv` (media_time ≠ tfdt + cts); written to `tests/corpora/synthetic/isobmff-broken/` | `CNT-3010` (chapter 8 WI-1.220) |
| `dashif_sidx_m4s` | `dashif` | one DASH-IF media segment with `sidx` + `styp` (e.g. from `/dash264/TestCases/1a/`) | `SidxRef.actual_size == referenced_size`; no `CNT-4030`/`CNT-4032` |
| `bento4_cenc_subsample` | `bento4` | Bento4 `Test/Data` fMP4 encrypted with `cenc`, subsample-encrypted AVC (`senc` + `saiz`/`saio`) | subsample ranges, KID/IV extraction (chapter 8 WI-1.213) |
| `bento4_playready` | `bento4` | Bento4 PlayReady sample (`pssh` with WRMHEADER, PIFF `uuid` sample encryption) | `pssh[0].summary` KID (chapter 8 WI-4.212) |
| `hdr_hdr10plus_dv81` | `hdr` | Dolby Vision profile 8.1 + HDR10+ clip (quietvoid `dovi_tool`/`hdr10plus_tool` assets, §8.9.3) | `HdrMetadata.kind == DolbyVision`, `has_hdr10plus`, `nal_62_rpu` + `sei_t35_hdr10plus` signals (chapter 8 WI-1.223) |
| `hdr_hlg_bt2020` | `hdr` | BBC R&D HLG test clip (BT.2020, `transfer 18`) | `HdrMetadata.kind == HLG` |
| `heif_heic_grid` | `heif` | Nokia conformance `grid` image (`C0xx` series: `hvc1` tiles + `grid` derived item) | `ItemInfo.derived_of`, `is_primary` (chapter 8 WI-1.214/WI-4.213) |
| `ts_h264_dvb` | `ts` | tsduck-test DVB capture with one H.264 service (`input/` set) | PES count and PTS/DTS vs `ffprobe -show_packets` (chapter 8 WI-4.206) |
| `gpac_eac3_atmos` | `gpac` | GPAC test media E-AC-3 JOC (`dec3` with `num_ind_sub`, Atmos flag) in MP4 | `dec3.data_rate`, `num_ind_sub` vs MP4Box (chapter 8 WI-4.214) |
| `hdr_dovi_p5` | `hdr` | Dolby Vision **profile 5** (IPTPQc2, single layer, no backward-compatible base) clip: the profile-5 sample of the `dovi_tool` test assets (https://github.com/quietvoid/dovi_tool, `assets/`), muxed with `MP4Box -add <es>.hevc:dvp=5 -new hdr_dovi_p5.mp4`; when the upstream asset carries no picture data, the entry is `manual_download=true` with the maintainer-recorded SHA-256 of a profile-5 clip from the Dolby developer test content (§8.9.3); nightly, `[needs-corpus]` | chapter 4 FR-HDR-010 (DV P5 reshaping, HDR panel label `DV profile 5 → PQ (libplacebo)`), chapter 10 WI-4.456; also the source of `syn_mkv_dv_p5` (§14.3.3 recipe 65) |
| `dashif_testcase_1a` | `dashif` | DASH-IF conformance test case 1a (`/dash264/TestCases/1a/` vector: one period, one AVC adaptation set, `SegmentTemplate`); fetched as `manifest.mpd` + init + the first 10 segments of representation 1, rewritten by the fetcher's `post` step `mpd_localize` so all URLs are relative; ✓ | chapter 4 FR-CNT-017 and chapter 8 WI-4.210 virtual-file tests, served by `tools/tests/fixture_server.py` (WI-0.551; `tests/serve_corpus.py` is its two-line wrapper, default port 8080) |
| `dashif_bbb_30fps` | `dashif` | `https://dash.akamaized.net/akamai/bbb_30fps/bbb_30fps.mpd` (multi-representation live profile, `SegmentTemplate` with `$Number$`), MPD + init + 3 segments of the lowest representation, localised like `dashif_testcase_1a` | chapter 8 WI-4.208 MPD expansion (cited there as `dashif/bbb_30fps.mpd`), §14.11 Phase 4 step 6 |
| `apple_bipbop_adv_hevc` | `apple` | `img_bipbop_adv_example_fmp4/master.m3u8` restricted to one HEVC variant (playlist + init + first 3 segments, localised); ✓ (≈ 12 MiB) | chapter 8 WI-4.209/WI-4.210 HLS virtual file, §14.11 Phase 4 step 6 |
| `mpegff_a1_foreman_qcif` | `mpegff` | `data/file_features/published/isobmff/a1-foreman-QCIF.mp4`; ✓ | minimal real MP4 for `box-tree` smoke tests; WI-0.556 step 4 commits a 2 KiB structural copy (`mdat` payload dropped by `tools/mp4_shrink.py`) |
| `mpegff_hevc_dual_track` | `mpegff` | FileFormatConformance `hevc_dual_track` sample, extended by the manifest `post` step `MP4Box -add syn_av1_gop60_1080p.ivf -add <file> -new out.mp4` reordered so that tracks are 1 = AAC, 2 = HEVC, 3 = AV1 (the `post` result is hashed under `sha256_post`); ✓ | chapter 4 FR-CNT-001 default-video-track rule, chapter 8 WI-1.208 |
| `gpac_enc_cenc` | `gpac` | GPAC test suite `media/encryption/` `cenc` AES-CTR sample with one `pssh` (exact member recorded in the manifest); ✓ | CENC boxes vs `MP4Box -diso`, chapter 8 WI-1.213 |
| `bento4_frag_cenc` | `bento4` | Bento4 `Test/Data/` fragmented `cenc` sample with a `pssh`; ✓ | chapter 8 WI-1.213 |
| `mkv_test1` … `mkv_test8` | `mkv` | the eight Matroska test-suite files (`test1.mkv` … `test8.mkv`; `test1`, `test3`, `test5` are ✓) | chapter 8 WI-4.201–WI-4.203 EBML goldens (cited there as `test1..test8.mkv`) |
| `misc_pixel_motion_photo` | `misc` | Pixel camera MP4 recorded with Motion Photo on (`©xyz` location tag, XMP `uuid` with `MicroVideoOffset`, JPEG + MP4 trailer) | chapter 8 WI-1.215/WI-1.216 udta/uuid tests (cited there as `pixel_camera.mp4` and `motion_photo.mp4`) |
| `misc_iphone_hevc_mov` | `misc` | iPhone HEVC `.mov` with QuickTime `meta/keys/ilst` and a 90° `tkhd.matrix` | chapter 8 WI-1.205/WI-1.206/WI-1.215 QuickTime metadata and rotation tests (cited there as "an iPhone `.mov`" / "a 90°-rotated phone video") |
| `ytugc_clip01`, `ytugc_clip02`, `ytugc_clip03` | `ytugc` | the three `fast` YouTube-UGC clips (360p; one each from `Gaming`, `Sports`, `Vlog`; exact upstream names in the manifest); ✓ | UVQ parity (chapter 4 FR-VQ-009/FR-VQ-010, chapter 9 §9.6, WI-3.593); the 17 nightly clips are `ytugc_clip04` … `ytugc_clip20` |
| `netflix_src01_hrc00_576x324`, `netflix_src01_hrc01_576x324` | `netflix` | `resource/yuv/src01_hrc00_576x324.yuv`, `src01_hrc01_576x324.yuv`, wrapped to Y4M by the fetcher's `post` step `wrap_y4m` (576×324, 8-bit 4:2:0, 48 frames); ✓ | libvmaf parity pair of §14.4.11, chapter 9 §9.11.1, chapter 16 §16.1.4 |

### 14.3.2 Fast subset selection

The `fast` tag (≤ 300 MiB total) is chosen to cover every syntax branch that the golden suites exercise at least once:

- H.264 (`jvt`): `BA1_Sony_D`, `BA_MW_D`, `BAMQ1_JVC_C`, `CABA1_SVA_B`, `CABA3_TOSHIBA_E`, `CAMA1_Sony_C` (MBAFF), `CAPAMA3_Sand_F` (PAFF+MBAFF), `CVWP1_TOSHIBA_E` (weighted pred), `MR1_BT_A` (multiple refs/long-term), `CAPM3_Sony_D` (8×8 transform, High), `FRExt/HPCV_BRCM_A`, `FRExt/Hi422FR1_SONY_A`, `FRExt/Hi444/ALPHACONFORMANCEG`, `sp1_bt_a` (SP slices), `CI1_FT_B` (CAVLC interlace), `cvmp_mot_frm_L31_B` (multiple slices), `src19td.IBP.264`, `NRF_MW_E` (non-ref frames), `Sharp_MP_Field_1_B`, `CVSE2_Sony_B` (SEI), `CVFC1_Sony_C` (frame cropping), `FM1_BT_B` (FMO), `aud_mw_e` (AUD), `MIDR_MW_D`, `LS_SVA_D`, and (added for chapter 4/7 acceptance tests, D26) `CVSE3_Sony_H` (SEI + long-term refs), `CVSEFDFT3_Sony_E` (SEI, field/frame), `MR6_BT_B` and `MR9_BT_B` (multiple refs, interlace, POC type 2/MMCO), `CVPCMNL1_SVA_C` (I_PCM, POC type 0), `CVPA1_TOSHIBA_B` (PAFF field pictures), `CVFI1_Sony_D` (field pictures), `CAQP1_Sony_B` (QP changes), `CABAST3_Sony_E` (CABAC, slice types), `CVFC1_SVA_C` (frame cropping), `FRExt1_Panasonic_D` (FRExt 8×8), `HCAFF1_HHI_B` (High, PAFF CABAC), `HPCAFL_BRCM_C` (High, CABAC field). Nightly-only H.264 entries with their own tags: `MVC/MVCNV-1` … (`mvc`), `SVC/SVCBST-1` … (`svc`) from the `professional_profiles`/`MVC`/`SVC` folders. Spelling rule: the manifest uses the ITU file stem; FFmpeg FATE carries older revisions of some streams (e.g. `FRExt1_Panasonic_C`, `HPCAFL_BCRM_C`), so `tools/check_fast_subset.py` checks every name against the ITU listing at WI-0.552 time and fails on a stem that does not exist upstream — the citing chapter then uses the upstream stem.
- Manifest tags over H.264 streams used by chapter 7 golden tests: `h264-ps-golden` = the 20 streams listed in chapter 7 WI-1.108 (`BA1_Sony_D`, `BA_MW_D`, `BAMQ1_JVC_C`, `CAPAMA3_Sand_F`, `CI1_FT_B`, `CVSE2_Sony_B`, `CVWP1_TOSHIBA_E`, `HPCV_BRCM_A`, `FRExt1_Panasonic_D`, `HCAFF1_HHI_B`, `Hi422FR1_SONY_A`, `HPCAFL_BRCM_C`, `MR1_BT_A`, `MR6_BT_B`, `CAQP1_Sony_B`, `sp1_bt_a`, `CVFC1_SVA_C`, `FM1_BT_B`, `CABAST3_Sony_E`, `SVC/SVCBST-1`); `h264-slice-golden` (chapter 7 WI-1.109, "40 JVT streams") = the 38 `fast` streams of this list plus `MVC/MVCNV-1` and `SVC/SVCBST-1` (40); `h264-analysis-smoke` (chapter 7 WI-2.170, "10 JVT streams") = `BA1_Sony_D`, `BA_MW_D`, `CABA1_SVA_B`, `CABA3_TOSHIBA_E`, `CAMA1_Sony_C`, `CVWP1_TOSHIBA_E`, `MR1_BT_A`, `CAPM3_Sony_D`, `CI1_FT_B`, `NRF_MW_E`.
- HEVC (`jctvc`): `AMP_A_Samsung_7`, `AMP_D_Hisilicon_3`, `CAINIT_A_SHARP_4`, `CONFWIN_A_Sony_1` (conformance window), `DBLK_A_SONY_4`, `DSLICE_A_HHI_5` (dependent slices + WPP), `ENTP_A_QUALCOMM_1` (entry points), `EXT_A_ericsson_4` (extension flags), `FILLER_A_Sony_1`, `HRD_A_Fujitsu_3`, `INITQP_B_Sony_1`, `LTRPSPS_A_FJ_1` (long-term refs in SPS), `MAXBINS_A_TI_5`, `MERGE_B_TI_3`, `NUT_A_ericsson_5` (NAL types), `OPFLAG_A_Qualcomm_1`, `PICSIZE_A_Bossen_1`, `PMERGE_A_TI_3`, `POC_A_Bossen_3`, `PPS_A_qualcomm_7`, `RAP_A_docomo_6` (CRA/RASL), `RPS_A_docomo_5`, `SAO_A_MediaTek_4`, `SDH_A_Orange_4`, `SLIST_A_Sony_5` (scaling lists), `STRUCT_B_Samsung_7`, `TILES_B_Cisco_1`, `TSCL_A_VIDYO_5` (temporal scalability), `WP_A_Toshiba_3` (weighted pred), `WPP_A_ericsson_MAIN10_2`, `VPSID_A_VIDYO_2`, `RExt/Main_422_10_A_RExt_Sony_2` (HM oracle only), `RExt/GENERAL_444_8bit_RExt_Sony_2`, and (added for chapter 4/7 acceptance tests, D26) `WPP_A_ericsson_MAIN_2` (8-bit WPP; FR-DEC-009 `ovt dump --sw --md5`), `RAP_B_Bossen_2` (CRA/BLA with leading pictures), `TSKIP_A_MS_3` (transform skip), `MVCLIP_A_qualcomm_3` and `MVEDGE_A_qualcomm_3` (MV clipping at picture edges), `NoOutPrior_A_Qualcomm_1` (`no_output_of_prior_pics_flag`), `ipcm_A_NEC_3` (PCM), the ST-RPS family `RPS_B_qualcomm_5`, `RPS_C_ericsson_5`, `RPS_D_ericsson_6`, `RPS_E_qualcomm_5`, `RPS_F_docomo_2` (inter-RPS prediction; chapter 7's "`RPS_A_docomo` … `RPS_F_docomo`" means `RPS_A_docomo_5` plus these five upstream names), `RExt/Bitdepth_A_RExt_Sony_1` (12-bit; HM oracle only). Tags: `rext` marks every `RExt/*` entry (chapter 4's `corpus/jctvc_rext_*` = the `rext`-tagged entries), `scc` marks the SCC folder (chapter 7's "an SCC stream" = the first `scc`-tagged entry in manifest order; nightly), `h265-cu-golden` as below; "all HEVC_v1 streams" / "20 HEVC_v1 streams" in chapter 7 are the `jctvc` `fast` entries outside `RExt/` (the first 20 in manifest order for the latter).
- AV1 (`aom`): `av1-1-b8-01-size-16x16`, `-16x18`, `-66x66`, `-196x196`, `-226x226`, `av1-1-b8-02-allintra`, `av1-1-b8-03-sizeup`, `av1-1-b8-03-sizedown` (reference scaling), `av1-1-b8-04-cdfupdate`, `av1-1-b8-05-mv`, `av1-1-b8-06-mfmv`, `av1-1-b8-22-svc-L1T2`, `av1-1-b8-22-svc-L2T1`, `av1-1-b8-23-film_grain-50`, `av1-1-b10-23-film_grain-50`, `av1-1-b8-24-*` (intra-only / S-frames; names as in `test_vectors.cc`), `av1-1-b8-16-intra_only-intrabc-extreme-dv`, `av1-1-b8-16-intra-only`, `av1-1-b10-00-quantizer-{00,10,30,63}`, `av1-1-b8-01-size-16x16-annexb` family (Annex B), `av1c-vs-00`…`av1c-vs-03` (MP4), and `av1-1-b8-22-svc-L2T2` (2 spatial × 2 temporal layers; operating-point tests of chapter 7 WI-1.116). Chapter 4's `corpus/aom_*` = the `aom` `fast` entries; `corpus/argon_*` = the nightly `argon` `profile0_core` subset; chapter 7's "30 Argon streams" / "60 Argon streams" = the first 30 / 60 entries of the `argon-header-golden` tag (manifest order; nightly, `manual_download=true`, skipped when absent).
- ISOBMFF: `mpegff` (`a1-foreman-QCIF.mp4` = `mpegff_a1_foreman_qcif`, `avc-sample-entries`, `hevc_dual_track` = `mpegff_hevc_dual_track`, fragmented/`sidx` files, HEIF `C001.heic`), `gpac` (`counter-10s-1280x720_1s_frag.mp4`, `dash_init.mp4` + 3 segments, `enc_cenc.mp4` = `gpac_enc_cenc`), `bento4` (`frag-cenc.mp4` = `bento4_frag_cenc`, `pssh.mp4`), `cta` (one AVC CMAF track = `cta_cmaf_video_cmfv`, one HEVC HDR10 track), `apple` (two fMP4 variants incl. `apple_bipbop_adv_hevc`), `dashif` (`dashif_sidx_m4s`, `dashif_testcase_1a`), `mkv` (`mkv_test1`, `mkv_test3`, `mkv_test5`).
- Metrics and HDR: `netflix_src01_hrc00_576x324`, `netflix_src01_hrc01_576x324`, `ytugc_clip01`–`ytugc_clip03`, `hdr_hdr10plus_dv81` and `hdr_hlg_bt2020` (both trimmed to 10 s), `ts_h264_dvb`.

Exact file names are fixed in `manifest.json`; the lists above are the selection rationale and MUST be kept in sync by the `corpora-health` job (it fails if a name listed here has no manifest entry).

Chapter 7 golden tests select streams by manifest `tags`: `h264-ps-golden` (parameter-set parity, WI-1.108), `h264-slice-golden` (WI-1.109), `h264-analysis-smoke` (WI-2.170), `h265-cu-golden` (CU/PU/TU grid parity vs `hm_cu_dump`, WI-2.164), `av1-grid-golden` (WI-2.157), `argon-header-golden` (WI-1.117/WI-1.118). The `fast` subset carries these tags on the streams named above; `jctvc_MVCLIP_A_qualcomm_3` and the Argon partition streams carry them in the nightly tier only. HM grid goldens live at `tests/golden/h265/<id>.hm-grid.json` (§14.4.4), the only location.

### 14.3.3 OVT synthetic generator

`tests/corpora/synthetic/generate.py` runs the recipes in `recipes.json` with the pinned oracle-image encoders: `ffmpeg` (n7.1.1 build with libx264, libx265, libaom), `x264` (r3191+), `x265` (4.1), `aomenc` (libaom v3.12.1). Every recipe starts from the same two 60-frame sources produced by lavfi so the corpus is reproducible and CC0:

```bash
# Source A: 8-bit 4:2:0 1280x720, 60 frames, moving test pattern with text (gives inter motion + sharp edges)
ffmpeg -y -f lavfi -i "testsrc2=size=1280x720:rate=30,format=yuv420p" -frames:v 60 src_720p_8bit.y4m
# Source B: 10-bit 4:2:0 1920x1080 PQ-tagged gradient + noise (for HDR and 10-bit recipes)
ffmpeg -y -f lavfi -i "mandelbrot=size=1920x1080:rate=30,format=yuv420p10le" -frames:v 60 \
       -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc src_1080p_10bit.y4m
# Source C: 4:4:4 8-bit 640x360 for chroma-format recipes
ffmpeg -y -f lavfi -i "testsrc2=size=640x360:rate=30,format=yuv444p" -frames:v 30 src_360p_444.y4m
```

Recipes (the id is the corpus id; features are the manifest `features` vocabulary). Each command writes to `$OUT/<id>.<ext>`; `generate.py` then re-wraps Annex B / IVF into MP4 where the recipe says `mp4` (`ffmpeg -i in -c copy -movflags +faststart out.mp4`).

| # | Corpus id | Features | Exact command |
|---|---|---|---|
| 1 | `syn_h264_mbaff` | `mbaff`, `interlaced`, `cabac` | `x264 --preset medium --profile high --interlaced --tff --bframes 3 --ref 4 --keyint 30 --min-keyint 30 --no-scenecut --output syn_h264_mbaff.264 src_720p_8bit.y4m` |
| 2 | `syn_h264_paff` | `paff`, `interlaced`, `field_pictures` | `x264 --preset medium --profile high --interlaced --tff --fake-interlaced --bframes 2 --keyint 30 --no-scenecut --no-mbtree --output syn_h264_paff.264 src_720p_8bit.y4m` (x264 emits MBAFF; for true PAFF field pictures use: `ffmpeg -i src_720p_8bit.y4m -c:v libx264 -x264-params "interlaced=1:tff=1:keyint=30:bframes=2" -flags +ildct+ilme -f h264 syn_h264_paff.264` and then JM `lencod -p PicInterlace=1 -p MbInterlace=0 -p InputFile=src_720p_8bit.yuv -p OutputFile=syn_h264_paff.264` as the canonical PAFF generator; the manifest records which encoder produced the pinned file) |
| 3 | `syn_h264_weightedpred_ltr` | `weighted_pred`, `long_term_refs`, `multi_ref` | `x264 --preset slow --profile high --weightp 2 --weightb --ref 8 --bframes 4 --b-pyramid normal --keyint 60 --no-scenecut --output syn_h264_weightedpred_ltr.264 src_720p_8bit.y4m` (long-term reference pictures are **not** produced by this recipe — x264 never emits them and no verified JM `lencod` configuration key marks long-term pictures; the feature tag `long_term_refs` is kept on the id for historical reasons but H.264 LTR coverage comes from the conformance streams `jvt_MR1_BT_A`, `jvt_MR*_TANDBERG_*`, `jvt_LS_SVA_D` and `jvt_CVSE3_Sony_H` of the `jvt` corpus) |
| 4 | `syn_h264_multislice_cavlc` | `multiple_slices`, `cavlc`, `baseline` | `x264 --preset fast --profile baseline --no-cabac --slices 4 --keyint 15 --no-scenecut --output syn_h264_multislice_cavlc.264 src_720p_8bit.y4m` |
| 5 | `syn_h264_crop_8x8dct_10bit` | `cropping`, `transform_8x8`, `high10`, `10bit` | `ffmpeg -i src_1080p_10bit.y4m -vf "crop=1918:1078:1:1" -c:v libx264 -pix_fmt yuv420p10le -profile:v high10 -x264-params "8x8dct=1:keyint=30:bframes=3:ref=3" -f h264 syn_h264_crop_8x8dct_10bit.264` (odd size forces `frame_cropping_flag=1`) |
| 6 | `syn_h264_444_intra` | `444`, `intra_only`, `lossless` | `x264 --preset medium --profile high444 --output-csp i444 --qp 0 --keyint 1 --output syn_h264_444_intra.264 src_360p_444.y4m` |
| 7 | `syn_h265_tiles_wpp` | `tiles`, `wpp`, `main` | WPP (x265): `ffmpeg -i src_720p_8bit.y4m -c:v libx265 -x265-params "wpp=1:keyint=30:no-open-gop=1:bframes=4:ref=3" -f hevc syn_h265_wpp.265`. Tiles (x265 has no tile support, so HM is used): `TAppEncoder -c encoder_randomaccess_main.cfg -i src_720p_8bit.yuv -wdt 1280 -hgt 720 -fr 30 -f 60 --TileUniformSpacing=1 --NumTileColumnsMinus1=1 --NumTileRowsMinus1=1 --WaveFrontSynchro=0 -b syn_h265_tiles_2x2.265` (the `.yuv` is `ffmpeg -i src_720p_8bit.y4m -f rawvideo src_720p_8bit.yuv`) |
| 8 | `syn_h265_amp_sao_on` | `amp`, `sao`, `main` | `x265 --input src_720p_8bit.y4m --preset slower --amp --sao --rect --keyint 30 --no-open-gop --bframes 4 --ref 4 --output syn_h265_amp_sao_on.265` |
| 9 | `syn_h265_sao_off_deblock_off_pcm` | `no_sao`, `no_deblock`, `pcm`, `transquant_bypass` | `TAppEncoder -c encoder_lowdelay_main.cfg -i src_720p_8bit.yuv -wdt 1280 -hgt 720 -fr 30 -f 30 --SAO=0 --LoopFilterDisable=1 --PCMEnabledFlag=1 --PCMLog2MaxSize=5 --PCMLog2MinSize=3 --TransquantBypassEnable=1 --CUTransquantBypassFlagForce=1 -b syn_h265_sao_off_deblock_off_pcm.265` |
| 10 | `syn_h265_main10_pq_hdr` | `10bit`, `pq`, `mdcv_sei`, `clli_sei`, `main10` | `x265 --input src_1080p_10bit.y4m --input-depth 10 --output-depth 10 --profile main10 --preset medium --colorprim bt2020 --transfer smpte2084 --colormatrix bt2020nc --master-display "G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)" --max-cll "1000,400" --hdr10 --hdr10-opt --repeat-headers --keyint 30 --output syn_h265_main10_pq_hdr.265` then `-c copy` into MP4 so `colr`/`mdcv`/`clli` boxes are written (`ffmpeg -i syn_h265_main10_pq_hdr.265 -c copy -tag:v hvc1 syn_h265_main10_pq_hdr.mp4`) |
| 11 | `syn_h265_422_444_rext` | `422`, `444`, `rext`, `12bit` | `x265 --input src_360p_444.y4m --input-csp i444 --output-depth 12 --profile main444-12 --preset medium --keyint 15 --output syn_h265_444_12bit.265` and `ffmpeg -i src_360p_444.y4m -pix_fmt yuv422p10le -c:v libx265 -x265-params "profile=main422-10:keyint=15" -f hevc syn_h265_422_10bit.265` (two files, one id prefix; libde265 plugin skips 4:4:4-12; HM oracle decodes both) Consumed only by `ovt-plugin-hm` (WI-4.105) and the `hm_cu_dump` oracle; libde265 is Main/Main10 only (chapter 7 §7.6.2, OI-03), so no de265 golden exists for this entry. |
| 12 | `syn_h265_multislice_wp` | `multiple_slices`, `dependent_slices`, `weighted_pred` | `TAppEncoder -c encoder_randomaccess_main.cfg -i src_720p_8bit.yuv -wdt 1280 -hgt 720 -fr 30 -f 60 --SliceMode=1 --SliceArgument=300 --DependentSliceMode=1 --DependentSliceArgument=100 --WeightedPredP=1 --WeightedPredB=1 -b syn_h265_multislice_wp.265`. HEVC **long-term reference pictures are not synthesised**: the HM encoder's GOP table expresses short-term RPS entries only and has no configuration path that marks pictures long-term; LTR coverage (`long_term_ref_pics_present_flag=1`, `num_long_term_pics>0`, `lt_idx_sps`) comes from the conformance streams `jctvc_LTRPSPS_A_FJ_1`, `jctvc_POC_A_Bossen_3` and the `jctvc_RPS_*` family, which are in the `fast` subset for that purpose |
| 13 | `syn_av1_tiles_cdef_lr` | `tiles`, `cdef`, `loop_restoration`, `superblock_128` | `aomenc --codec=av1 --cpu-used=4 --end-usage=q --cq-level=30 --tile-columns=2 --tile-rows=1 --enable-cdef=1 --enable-restoration=1 --sb-size=128 --kf-max-dist=30 --lag-in-frames=0 --ivf -o syn_av1_tiles_cdef_lr.ivf src_720p_8bit.y4m` |
| 14 | `syn_av1_hidden_altref_showexisting` | `hidden_frames`, `show_existing_frame`, `altref`, `order_hint` | `aomenc --codec=av1 --cpu-used=3 --end-usage=q --cq-level=35 --lag-in-frames=19 --auto-alt-ref=1 --enable-fwd-kf=1 --kf-max-dist=60 --tile-columns=0 --ivf -o syn_av1_hidden_altref_showexisting.ivf src_720p_8bit.y4m` (ALTREF hidden frames + `show_existing_frame=1` OBU_FRAME_HEADERs) |
| 15 | `syn_av1_filmgrain_10bit` | `film_grain`, `10bit`, `pq` | `aomenc --codec=av1 --cpu-used=4 --bit-depth=10 --input-bit-depth=10 --end-usage=q --cq-level=32 --denoise-noise-level=25 --enable-dnl-denoising=1 --color-primaries=bt2020 --transfer-characteristics=smpte2084 --matrix-coefficients=bt2020ncl --kf-max-dist=30 --ivf -o syn_av1_filmgrain_10bit.ivf src_1080p_10bit.y4m` then `ffmpeg -i syn_av1_filmgrain_10bit.ivf -c copy -movflags +faststart syn_av1_filmgrain_10bit.mp4` (the sequence header's colour config yields the `colr` box; **no** MaxCLL/MDCV: FFmpeg `-metadata` writes neither `clli`/`mdcv` boxes nor metadata OBUs, and `aomenc` has no CLL/MDCV option) |
| 15a | `syn_av1_pq_hdr_obu` | `10bit`, `pq`, `hdr_metadata_obu`, `mdcv_obu`, `clli_obu` | `syn_av1_filmgrain_10bit.ivf` re-written by `tools/av1_insert_hdr_obus.py` (committed, ≈ 150 lines: splits the IVF temporal units, inserts one `OBU_METADATA` of `METADATA_TYPE_HDR_MDCV` (`G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)`) and one of `METADATA_TYPE_HDR_CLL` (`1000,400`) after every sequence header, per AV1 spec §5.8.3–5.8.4) → `syn_av1_pq_hdr_obu.ivf`, then MP4 with `ffmpeg -c copy` **and** `MP4Box -add syn_av1_pq_hdr_obu.ivf:colr=nclc,9,16,9 -new syn_av1_pq_hdr_obu.mp4` so that `mdcv`/`clli` boxes are derived by the muxer where supported; the manifest records which boxes the pinned muxer wrote. Used for AV1 HDR metadata OBU parsing (chapter 7), the HDR panel and `ovt hdr --compare-declared` |
| 16 | `syn_av1_intra_only_sframe_svc` | `intra_only_frame`, `switch_frame`, `svc`, `operating_points` | `aomenc --codec=av1 --cpu-used=5 --end-usage=q --cq-level=40 --kf-max-dist=9999 --enable-order-hint=1 --sframe-dist=15 --sframe-mode=1 --ivf -o syn_av1_sframe.ivf src_720p_8bit.y4m` and `examples/scalable_encoder src_720p_8bit.y4m syn_av1_svc_L2T1.obu` (libaom example, 2 spatial layers, Section 5 OBU stream) |
| 17 | `syn_av1_444_422_profile2` | `444`, `422`, `profile_high`, `profile_professional` | `aomenc --codec=av1 --cpu-used=5 --end-usage=q --cq-level=30 --profile=1 --kf-max-dist=15 --ivf -o syn_av1_444_profile1.ivf src_360p_444.y4m` and `ffmpeg -i src_360p_444.y4m -pix_fmt yuv422p -f yuv4mpegpipe - \| aomenc --codec=av1 --cpu-used=5 --profile=2 --end-usage=q --cq-level=30 --ivf -o syn_av1_422_profile2.ivf -` |
| 18 | `syn_av1_annexb_obu_split` | `annexb`, `temporal_unit`, `obu_size_field` | `aomenc --codec=av1 --cpu-used=6 --end-usage=q --cq-level=40 --kf-max-dist=10 --annexb=1 --obu -o syn_av1_annexb.obu src_720p_8bit.y4m` and `aomenc ... --annexb=0 --obu -o syn_av1_lowoverhead.obu src_720p_8bit.y4m` |
| 19 | `syn_av1_superres_scaled_refs` | `superres`, `reference_scaling`, `render_size` | `aomenc --codec=av1 --cpu-used=4 --end-usage=q --cq-level=35 --superres-mode=1 --superres-denom=12 --resize-mode=1 --resize-denominator=12 --kf-max-dist=30 --ivf -o syn_av1_superres.ivf src_720p_8bit.y4m` |
| 20 | `syn_mp4_edit_list_frag_cenc` (files `syn_mp4_elst.mp4`, `syn_mp4_frag.mpd` + `seg_*.m4s`, `syn_mp4_cenc_cbcs.mp4`) | `elst`, `fragmented`, `sidx`, `cenc_cbcs`, `pssh` | (a) `MP4Box -add syn_h264_multislice_cavlc.264:fps=30:delay=500 -new syn_mp4_elst.mp4` (the 500 ms delay produces an `elst` with an empty edit); (b) `MP4Box -dash 1000 -frag 500 -rap -segment-name seg_ -out syn_mp4_frag.mpd syn_mp4_elst.mp4` (fMP4 init + segments with `sidx`, `styp`, `tfdt`, `trun`); (c) `MP4Box -crypt tests/corpora/synthetic/drm_cbcs.xml syn_mp4_elst.mp4 -out syn_mp4_cenc_cbcs.mp4` (`drm_cbcs.xml` is committed, GPAC CENC description with scheme `cbcs`, one key, one `pssh` with the Common PSSH system id `1077efec-c0b2-4d02-ace3-3c1e52e2fb4b`) |
| 21 | `syn_mp4_frag_cmaf` | `fragmented`, `cmaf`, `cmfc`, `styp`, `tfdt`, `trun` | `MP4Box -dash 2000 -frag 2000 -rap -profile live -segment-name 'cmaf_$Number$' -brand cmfc -out syn_mp4_frag_cmaf.mpd syn_h265_amp_sao_on.265:fps=30` then `cat cmaf_init.mp4 cmaf_1.m4s cmaf_2.m4s cmaf_3.m4s > syn_mp4_frag_cmaf.mp4` (one self-contained CMAF track file with `styp`/`moof` sequence; the GUI Fragments tab test of chapter 11 WI-1.460 and `mp4ff-info` goldens use it) |
| 22 | `syn_h264_epb_dense` | `epb_dense`, `cabac`, `high` | `x264 --preset medium --profile high --qp 0 --keyint 30 --no-scenecut --output syn_h264_epb_dense.264 src_epb.y4m` where `src_epb.y4m` is `ffmpeg -f lavfi -i "color=black:size=1280x720:rate=30,format=yuv420p" -frames:v 30` (constant black at QP 0 produces long zero runs and therefore emulation-prevention bytes inside SPS/PPS/slice-header and slice-data bytes); `generate.py --check` asserts ≥ 20 EPBs in the first 3 units via `ovt syntax --hex`. Golden `tests/golden/epb/syn_h264_epb_dense.json` (field → file byte offset/bit, from `trace_headers`) drives chapter 11 WI-1.452/WI-1.458 |
| 23 | `syn_h265_main10_pq_hdr_mismatch` | `10bit`, `pq`, `hdr_conflict` | `MP4Box -add syn_h265_main10_pq_hdr.265:colr=nclc,1,1,1 -new syn_h265_main10_pq_hdr_mismatch.mp4` (container `colr` says BT.709/BT.1886 while the SPS VUI says BT.2020/PQ → the core raises `DEC-HDR-0001`; used by chapter 11 WI-1.462 and chapter 8 conflict tests) |
| 24 | `syn_align_drop7` | `alignment`, `leading_drop` | distorted = `ffmpeg -i src_720p_8bit.y4m -vf "select='gte(n,7)',setpts=N/30/TB" -c:v libx264 -crf 23 -keyint_min 30 -g 30 -f h264 syn_align_drop7.264` (first 7 source frames removed, then encoded); reference = `src_720p_8bit.y4m`. The alignment search must report `offset_frames == +7` with the chapter 9 §9.3.1 sign convention (distorted frame i ↔ reference frame i+7) |
| 25 | `syn_metrics_1080p_pair` | `metrics_pair`, `1080p` | reference = `ffmpeg -f lavfi -i "testsrc2=size=1920x1080:rate=30,format=yuv420p" -frames:v 300 src_1080p_8bit.y4m`; distorted = `x264 --preset medium --crf 28 --keyint 60 --no-scenecut --output syn_metrics_1080p_dist.264 src_1080p_8bit.y4m` decoded back to Y4M by the fetcher (`ffmpeg -i … -f yuv4mpegpipe`). Used by `bench_metrics_*`, chapter 11 WI-3.450 and §14.4.10 |
| 26 | `syn_h264_gop60_1080p`, `syn_h265_gop60_1080p`, `syn_av1_gop60_1080p` | `gop60`, `1080p`, `perf` | from `src_1080p_8bit.y4m` (recipe 25): `x264 --preset medium --crf 24 --keyint 60 --min-keyint 60 --no-scenecut --bframes 3 --output syn_h264_gop60_1080p.264`; `x265 --preset medium --crf 26 --keyint 60 --min-keyint 60 --no-open-gop --amp --sao --output syn_h265_gop60_1080p.265`; `aomenc --cpu-used=4 --end-usage=q --cq-level=32 --kf-max-dist=60 --kf-min-dist=60 --lag-in-frames=0 --enable-cdef=1 --enable-restoration=1 --ivf -o syn_av1_gop60_1080p.ivf`; each wrapped to MP4 |
| 27 | `syn_h265_10min_1080p`, `syn_av1_10min_1080p`, `syn_h264_1h_1080p` | `long`, `perf` | `src_1080p_8bit.y4m` looped with `-stream_loop` to 18 000 / 18 000 / 108 000 frames and encoded with the recipe-26 settings (`-stream_loop 59`, `-stream_loop 359`); outputs are **generated on the perf runner, never fetched** (`generate.py --only syn_h264_1h_1080p` ≈ 20 min); the manifest stores their hashes with `"local_only": true` |
| 28 | `syn_av1_4k_2min` | `4k`, `perf` | `ffmpeg -f lavfi -i "testsrc2=size=3840x2160:rate=30,format=yuv420p10le" -frames:v 3600 … \| aomenc --cpu-used=6 --end-usage=q --cq-level=35 --kf-max-dist=60 --tile-columns=2 --ivf -o syn_av1_4k_2min.ivf -`; `local_only` like recipe 27 |
| 29 | `syn_h264_multislice_cavlc_mp4` | as recipe 4, `mp4` | `ffmpeg -i syn_h264_multislice_cavlc.264 -c copy -movflags +faststart syn_h264_multislice_cavlc.mp4` (the Android smoke test of §14.10.4 pushes this MP4) |
| 30 | `syn_mp4_frag` (= the `syn_mp4_frag.mpd` + `seg_*.m4s` set of recipe 20, also fetched as one id) | `fragmented`, `sidx`, `dash` | alias row: the files of recipe 20 (b); named by chapter 8 WI-1.219/WI-1.235 acceptance tests |
| 31 | `syn_mp4_frag_3600` | `fragmented`, `perf` | `MP4Box -dash 1000 -frag 1000 -rap -segment-name 'frag3600_' -out syn_mp4_frag_3600.mpd syn_h264_1h_1080p.mp4` then `MP4Box -cat 'frag3600_*.m4s' -new syn_mp4_frag_3600.mp4` (one file, 3 600 `moof`s; chapter 8 WI-1.239 `bench_container_fragments_3600`) |
| 32 | `syn_heif_1000items` | `heif`, `items`, `perf` | `tools/gen_heif_items.py --items 1000 --tile 64 --out syn_heif_1000items.heic` (committed, ≈ 120 lines over `heif-convert`-free pure Python box writer: 1 000 `hvc1` tile items + one `grid`; chapter 8 WI-1.239) |
| 33 | `syn_ts_cc_errors` | `ts`, `cc_errors` | `ffmpeg -i src_720p_8bit.y4m -c:v libx264 -g 30 -f mpegts syn_ts_clean.ts` then `tools/ts_inject_errors.py --cc-errors 7 --pcr-gap 1 syn_ts_clean.ts syn_ts_cc_errors.ts` (committed; drops 7 packets of the video PID and removes PCRs for 1 s; chapter 8 WI-4.207 TR 101 290 counts) |
| 34 | `syn_mp4_broken_stco_beyond_eof` | `broken`, `stco` | `tools/make_broken.py --stco-beyond-eof` over the 20 KiB base file (`tests/corpora/synthetic/isobmff-broken/`, chapter 8 §8.9.3) |
| 35 | `syn_mp4_broken_cmfc_two_tracks` | `broken`, `cmaf` | `tools/make_broken.py --cmfc-two-tracks` (adds a second track to a `cmfc`-branded file) — `CNT-7010` |
| 36 | `syn_mp4_broken_msix_without_sidx` | `broken`, `cmaf` | `tools/make_broken.py --msix-no-sidx` (brand `msix` without `sidx`) — `CNT-7031` |
| 37 | `syn_mp4_broken_hvc1_no_ps` | `broken`, `hvc1` | `tools/make_broken.py --hvc1-strip-ps` (`hvc1` sample entry whose `hvcC` has no VPS/SPS/PPS arrays) — `CNT-7005` |
| 38 | `syn_hlg_1080p` | `10bit`, `hlg`, `main10`, `bt2020` | `x265 --input src_1080p_10bit.y4m --input-depth 10 --output-depth 10 --profile main10 --preset medium --colorprim bt2020 --transfer arib-std-b67 --colormatrix bt2020nc --repeat-headers --keyint 30 --output syn_hlg_1080p.265` then `ffmpeg -i syn_hlg_1080p.265 -c copy -tag:v hvc1 syn_hlg_1080p.mp4` (VUI `transfer_characteristics = 18`; **no** MDCV/CLL SEI and no `mdcv`/`clli` boxes; the pixel content is the Source-B Mandelbrot, interpreted as HLG). Phase-1 HLG demo (§14.11, chapter 16 §16.1 row 16), §14.10.5 step 5, chapter 10 HLG render tests; ✓ |
| 39 | `syn_h265_pq_sei_only` | `10bit`, `pq`, `mdcv_sei`, `clli_sei` | the elementary stream `syn_h265_main10_pq_hdr.265` of recipe 10 registered as its own id (VUI + MDCV/CLL SEI, no container boxes); ✓ |
| 40 | `syn_h265_pq_colr_only` | `10bit`, `pq`, `colr_only` | `x265 --input src_1080p_10bit.y4m --input-depth 10 --output-depth 10 --profile main10 --preset fast --keyint 30 --output pq_novui.265` (no colour options → VUI colour description absent) then `MP4Box -add pq_novui.265:colr=nclx,9,16,9,0 -new syn_h265_pq_colr_only.mp4` (transfer 16 known only from `colr`); ✓ |
| 41 | `syn_h265_pq_4000nit` | `10bit`, `pq`, `mdcv_sei`, `clli_sei` | recipe 10 with `--master-display "G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(40000000,50)" --max-cll "4000,1000"` and the source graded so that the top 10 % of rows decode above 1000 nits: Source B passed through `-vf "lutyuv=y='if(lt(Y,800),Y,Y+60)'"` before encoding; output MP4 |
| 42 | `syn_h265_vui_unspecified` | `no_vui`, `bt709_content` | `TAppEncoder -c encoder_lowdelay_main.cfg -i src_720p_8bit.yuv -wdt 1280 -hgt 720 -fr 30 -f 30 --VuiParametersPresent=0 -b syn_h265_vui_unspecified.265` (no VUI; content is BT.709 limited range) |
| 43 | `syn_h265_bpyramid` | `hierarchical_b`, `temporal_layers`, `refgraph` | `TAppEncoder -c encoder_randomaccess_main.cfg -i src_720p_8bit.yuv -wdt 1280 -hgt 720 -fr 30 -f 33 -b syn_h265_bpyramid.265` (HM random-access GOP: hierarchical B pyramid with several `TemporalId` levels; the reference edges are recorded once in `tests/corpora/bpyramid_edges.json` by `tests/oracles/normalize/hm_trace.py`). This is the "hierarchical-B fixture" of chapter 11; ✓ |
| 44 | `syn_h265_tiles_4x2` | `tiles`, `main` | recipe 7's HM command with `--NumTileColumnsMinus1=3 --NumTileRowsMinus1=1 -b syn_h265_tiles_4x2.265` |
| 45 | `syn_h265_multi_pps` | `multi_pps`, `pps_resend` | `tools/make_multi_pps.py syn_h265_amp_sao_on.265 --pps-ids 0,1,2 --resend-at 20,40 -o syn_h265_multi_pps.265` (clones the PPS with ids 1 and 2 (different `init_qp_minus26`), rewrites slice `slice_pic_parameter_set_id` round-robin and re-sends all PPS before frames 20 and 40); writes `tests/golden/syntax/syn_h265_multi_pps.expected.json` (expected `active_pps_unit` per slice); ✓ |
| 46 | `syn_h265_crop_1080_in_1088` | `cropping`, `conformance_window` | `TAppEncoder -c encoder_lowdelay_main.cfg -i src_1080p_8bit.yuv -wdt 1920 -hgt 1080 -fr 30 -f 10 --ConformanceWindowMode=2 --VerticalPadding=8 -b syn_h265_crop_1080_in_1088.265` (coded 1920×1088, `conf_win_bottom_offset = 4` in chroma units); ✓ |
| 47 | `syn_h265_sei_unknown_200` and the family `syn_sei_<codec>_<payload>` (one file per SEI payload type of chapter 7 §7.3, e.g. `syn_sei_h264_ga94_cc`, `syn_sei_h264_mastering_display_colour_volume`, `syn_sei_h264_content_light_level_info`, `syn_sei_h265_alternative_transfer`) | `sei` | `tools/sei_inject.py --codec h265 --type 200 --size 17 --seed 1 syn_h265_amp_sao_on.265 syn_h265_sei_unknown_200.265`; the family is produced by `tools/sei_inject.py --all --out-dir …` from the payload definitions in `tests/corpora/sei_payloads.json` (hand-encoded payload bytes per type, including an ATSC A/53 `GA94` `cc_data()` payload in an H.264 `user_data_registered_itu_t_t35` SEI) inserted after the first slice-less prefix of `syn_h264_multislice_cavlc.264` / `syn_h265_amp_sao_on.265`; the expected field values are `tests/corpora/sei_expected.json`. `syn_sei_h264_ga94_cc`, `syn_sei_h264_mastering_display_colour_volume`, `syn_sei_h264_content_light_level_info` (chapter 7 WI-1.110 test 1) and `syn_h265_sei_unknown_200` are ✓ |
| 48 | `syn_h265_hdr10plus` | `10bit`, `pq`, `hdr10plus`, `t35` | recipe 10 with `--dhdr10-info tests/corpora/synthetic/hdr10plus_metadata.json` (committed; 60 frames of ST 2094-40 parameters in x265's JSON format; the oracle image builds x265 with `-DENABLE_HDR10_PLUS=ON`); MP4 as recipe 10. Chapter 7 WI-1.120 "x265 stream with HDR10+ metadata"; ✓ |
| 49 | `syn_broken_h265_truncated_sps`, `syn_broken_h265_slice_error_f12`, `syn_broken_h264_missing_pps` and the family `syn_broken_syntax_<code>` (one file per diagnostic code of chapter 7 §7.9 whose condition can be produced by byte edits; the code is in the id, e.g. `syn_broken_syntax_SYN_H264_0004`) | `broken` | `tools/corrupt.py` over a clean recipe output: `--truncate-nal sps --keep-bytes 9 syn_h265_amp_sao_on.265` (→ `SYN-H265-0003`); `--flip-slice-data --frame 12 --byte 40 syn_h265_amp_sao_on.265` (→ decode error at decode index 12); `--drop-nal pps syn_h264_multislice_cavlc.264` (→ `SYN-H264-0010`); `corrupt.py --all-codes` emits the family from the condition table `tests/corpora/corrupt_conditions.json`; ✓ for the three named files |
| 50 | `syn_broken_h265_hash_mismatch` (file `syn_broken_h265_hash_mismatch_f17.265`) | `broken`, `hash_sei` | `TAppEncoder -c encoder_lowdelay_main.cfg -i src_720p_8bit.yuv -wdt 1280 -hgt 720 -fr 30 -f 30 --SEIDecodedPictureHash=1 -b clean_md5.265` then `tools/corrupt.py --flip-hash-sei --frame 17 clean_md5.265 syn_broken_h265_hash_mismatch_f17.265` (one bit of the MD5 of picture 17 inverted; the file name records the frame, as chapter 4 FR-SYN-019 requires); ✓ |
| 51 | `syn_level_violation_h264`, `syn_level_violation_h265`, `syn_level_violation_av1` | `level_violation` | `ffmpeg -i syn_h264_gop60_1080p.264 -c copy -bsf:v h264_metadata=level=3 -f h264 syn_level_violation_h264.264` (1080p at Level 3 exceeds MaxFS); `ffmpeg -i syn_h265_gop60_1080p.265 -c copy -bsf:v hevc_metadata=level=2 -f hevc syn_level_violation_h265.265`; `tools/av1_set_level.py --seq-level-idx 0 syn_av1_gop60_1080p.ivf syn_level_violation_av1.ivf` (rewrites `seq_level_idx[0]` to 2.0). Expected codes and `limit_name` values: `tests/corpora/level_expected.json`; ✓ |
| 52 | `syn_metrics_1080p_bitflip_f37` | `metrics_pair`, `identity` | `tools/y4m_poke.py --frame 37 --plane 0 --x 611 --y 203 --delta +1 src_1080p_8bit.y4m syn_metrics_1080p_bitflip_f37.y4m` (reference of recipe 25 with exactly one luma sample changed by +1); the original and new sample values are written to `tests/corpora/compare_expected.json` (`{frame, plane, x, y, a, b}`, with `b == a + 1`) and acceptance tests read `a`/`b` from that file; ✓ |
| 53 | `syn_metrics_10bit_8bit_pair` | `metrics_pair`, `bitdepth_mismatch` | reference = `src_1080p_10bit.y4m`; distorted = `ffmpeg -i src_1080p_10bit.y4m -vf format=yuv420p -c:v libx264 -crf 23 -f h264 -` decoded to 8-bit Y4M; ✓ |
| 54 | `syn_metrics_444_420_pair` | `metrics_pair`, `chroma_mismatch` | reference = `src_360p_444.y4m`; distorted = `ffmpeg -i src_360p_444.y4m -pix_fmt yuv420p syn_metrics_444_420_dist.y4m`; ✓ |
| 55 | `syn_metrics_fps_mismatch_pair` | `metrics_pair`, `fps_mismatch` | reference = `ffmpeg -f lavfi -i "testsrc2=size=1280x720:rate=60,format=yuv420p" -frames:v 600 ref60.y4m` (10 s at 60 fps); distorted = `ffmpeg -i ref60.y4m -vf fps=30 dist30.y4m` (300 frames) |
| 56 | `syn_metrics_scale_range_pair` | `metrics_pair`, `scale`, `range` | reference = `src_1080p_8bit.y4m`; distorted = `ffmpeg -i src_1080p_8bit.y4m -vf "scale=1280:720:flags=bicubic:out_range=full" -c:v libx264 -x264-params "range=pc" -crf 20 -f h264 -` decoded to Y4M (720p, full range) |
| 57 | `syn_metrics_ladder_ref`, `syn_metrics_ladder_q22`, `syn_metrics_ladder_q27`, `syn_metrics_ladder_q32`, `syn_metrics_ladder_q37` | `ladder`, `bdrate` | reference = first 120 frames of `src_1080p_8bit.y4m`; each rung `x264 --preset medium --qp <22\|27\|32\|37> --keyint 60 --no-scenecut --frames 120 --output r.264` wrapped to MP4 (`ffmpeg -c copy`) |
| 58 | `syn_metrics_pq_1080p_pair` | `metrics_pair`, `pq`, `10bit` | reference = `src_1080p_10bit.y4m`; distorted = `syn_h265_main10_pq_hdr.265` (recipe 10) decoded to 10-bit Y4M; expected values: `tests/corpora/pu21_expected.json`, `tests/vq/hdr_reference.json` (computed by the reference implementations of chapter 9 §9.5) |
| 59 | `syn_scenes_5cuts` | `scenes` | `ffmpeg -f lavfi -i "testsrc2=s=1280x720:r=30:d=0.667" -f lavfi -i "mandelbrot=s=1280x720:r=30" -f lavfi -i "smptehdbars=s=1280x720:r=30" -f lavfi -i "color=c=0x802020:s=1280x720:r=30" -f lavfi -i "life=s=1280x720:r=30:mold=10" -f lavfi -i "cellauto=s=1280x720:r=30:rule=110" -filter_complex "[0]trim=end_frame=20,format=yuv420p[a];[1]trim=end_frame=20,format=yuv420p[b];[2]trim=end_frame=20,format=yuv420p[c];[3]trim=end_frame=20,format=yuv420p[d];[4]trim=end_frame=20,format=yuv420p[e];[5]trim=end_frame=20,format=yuv420p[f];[a][b][c][d][e][f]concat=n=6:v=1" -c:v libx264 -crf 20 -g 300 syn_scenes_5cuts.mp4` (cuts at frames 20, 40, 60, 80, 100, written to `tests/corpora/scenes_expected.json`) |
| 60 | `syn_raw_1080p_p010` and the family `syn_raw_<ctag>` (one file per raw-format `C` tag of chapter 4 FR-PLN-001/FR-PLN-002, file name `syn_raw_<ctag>_<w>x<h>.yuv`) | `raw` | `ffmpeg -i src_1080p_10bit.y4m -frames:v 10 -pix_fmt p010le -f rawvideo syn_raw_1080p_p010.yuv`; the family uses the same command with `-pix_fmt <fmt>` from the `ctag → pix_fmt` table in `tests/corpora/raw_formats.json` (10 frames each, from Source A or B by bit depth); ✓ |
| 61 | `syn_h265_4k_main10` | `4k`, `10bit`, `perf` | `ffmpeg -f lavfi -i "testsrc2=size=3840x2160:rate=30,format=yuv420p10le" -frames:v 600 -c:v libx265 -x265-params "profile=main10:keyint=60:crf=26" -tag:v hvc1 syn_h265_4k_main10.mp4`; `local_only` like recipe 27 |
| 62 | `syn_av1_tools_all`, `syn_av1_palette`, `syn_av1_deltaq` | `palette`, `obmc`, `superres`, `delta_q` | `aomenc --codec=av1 --cpu-used=4 --end-usage=q --cq-level=30 --tune-content=screen --enable-palette=1 --enable-obmc=1 --superres-mode=1 --superres-denom=12 --kf-max-dist=30 --ivf -o syn_av1_tools_all.ivf src_720p_8bit.y4m`; `aomenc … --tune-content=screen --enable-palette=1 --limit=10 --ivf -o syn_av1_palette.ivf src_720p_8bit.y4m`; `aomenc … --deltaq-mode=1 --enable-tpl-model=1 --limit=30 --ivf -o syn_av1_deltaq.ivf src_720p_8bit.y4m`; `generate.py --check` asserts the tool counts with the `inspect` oracle and writes `tests/corpora/toolset_expected.json`; ✓ |
| 63 | `syn_av1_lowoverhead`, `syn_av1_annexb` | `obu`, `annexb` | alias rows: the two files of recipe 18 |
| 64 | `syn_av1_hlg_metadata` | `10bit`, `hlg`, `hdr_metadata_obu` | `aomenc --codec=av1 --cpu-used=5 --bit-depth=10 --input-bit-depth=10 --end-usage=q --cq-level=32 --color-primaries=bt2020 --transfer-characteristics=hlg --matrix-coefficients=bt2020ncl --kf-max-dist=30 --limit=30 --ivf -o hlg.ivf src_1080p_10bit.y4m` then `tools/av1_insert_hdr_obus.py --cll 600,200 --mdcv "G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,50)" hlg.ivf syn_av1_hlg_metadata.ivf`; values recorded in `tests/corpora/hdr_expected.json` together with those of recipes 39 and 40; ✓ |
| 65 | `syn_h264_hrd_cbr`, `syn_h264_hrd_vbr` | `hrd`, `nal_hrd` | `x264 --preset medium --bitrate 2000 --vbv-bufsize 2000 --vbv-maxrate 2000 --nal-hrd cbr --keyint 60 --frames 300 --output syn_h264_hrd_cbr.264 src_1080p_8bit.y4m`; `x264 --preset medium --crf 23 --vbv-bufsize 4000 --vbv-maxrate 4000 --nal-hrd vbr --keyint 60 --frames 300 --output syn_h264_hrd_vbr.264 src_1080p_8bit.y4m`; ✓ |
| 66 | `syn_h265_hrd_cbr`, `syn_h265_hrd_vbr` | `hrd` | `x265 --input src_1080p_8bit.y4m --preset medium --bitrate 3000 --vbv-bufsize 3000 --vbv-maxrate 3000 --strict-cbr --hrd --keyint 60 --frames 300 --output syn_h265_hrd_cbr.265`; VBR: `--crf 26 --vbv-bufsize 6000 --vbv-maxrate 6000 --hrd`; ✓ |
| 67 | `syn_h265_main10_hev1` | `hev1`, `10bit` | `ffmpeg -i syn_h265_main10_pq_hdr.265 -c copy -tag:v hev1 syn_h265_main10_hev1.mp4` (parameter sets in-band and in `hvcC`); ✓ |
| 68 | `syn_mp4_cenc`, `syn_mp4_frag_cbcs`, `syn_mp4_cbcs_keyrot`, `syn_mp4_cenc_widevine` | `cenc`, `cbcs`, `pssh` | `MP4Box -crypt tests/corpora/synthetic/drm_cenc.xml syn_h264_multislice_cavlc.mp4 -out syn_mp4_cenc.mp4` (scheme `cenc`, `encv`/`avc1`); `MP4Box -crypt tests/corpora/synthetic/drm_cbcs_constiv.xml syn_mp4_frag/… -out syn_mp4_frag_cbcs.mp4` (scheme `cbcs`, `constant_IV`, `Per_Sample_IV_Size = 0`, fragmented); `MP4Box -crypt tests/corpora/synthetic/drm_cbcs_keyrot.xml syn_h264_gop60_1080p.mp4 -out syn_mp4_cbcs_keyrot.mp4` (two keys rotated every 60 samples through `sgpd`/`sbgp` `seig`, `crypt_byte_block 1`, `skip_byte_block 9`); `mp4fragment syn_h264_multislice_cavlc.mp4 f.mp4 && mp4encrypt --method MPEG-CENC --key 1:<key>:random --property 1:KID:<kid> --pssh edef8ba9-79d6-4ace-a3c8-27dcd51d21ed:tests/corpora/synthetic/widevine_pssh.bin f.mp4 syn_mp4_cenc_widevine.mp4` (Widevine `pssh` whose protobuf `key_id` equals the KID); keys and KIDs are committed test values in the XML/JSON files; ✓ |
| 69 | `syn_mp4_piff` | `piff`, `smooth` | `mp4fragment syn_h264_multislice_cavlc.mp4 f.mp4 && mp4encrypt --method PIFF-CTR --key 1:<key>:random --property 1:KID:<kid> f.mp4 syn_mp4_piff.ismv` (PIFF `uuid` track-encryption and sample-encryption boxes) |
| 70 | `syn_m4a_itunes`, `syn_mp4_nero_chapters`, `syn_avif_10bit` | `ilst`, `chpl`, `avif` | `ffmpeg -f lavfi -i "sine=f=440:d=5" -c:a aac -metadata title=Title -metadata artist=Artist -metadata location=+48.8584+002.2945/ -f ipod syn_m4a_itunes.m4a`; `MP4Box -add syn_h264_multislice_cavlc.264:fps=30 -chap tests/corpora/synthetic/chapters.txt -new syn_mp4_nero_chapters.mp4` (`chpl` box); `ffmpeg -i src_1080p_10bit.y4m -frames:v 1 -c:v libaom-av1 -pix_fmt yuv420p10le -still-picture 1 -f avif syn_avif_10bit.avif`; ✓ |
| 71 | `syn_mkv_h264`, `syn_mkv_hdr10`, `syn_mkv_dv_p5`, `syn_webm_lacing_xiph` | `mkv`, `webm` | `mkvmerge -o syn_mkv_h264.mkv syn_h264_multislice_cavlc.mp4`; `mkvmerge -o syn_mkv_hdr10.mkv syn_h265_main10_pq_hdr.mp4` (writes `Colour`, `MasteringMetadata`, `MaxCLL`/`MaxFALL`); `mkvmerge -o syn_mkv_dv_p5.mkv hdr_dovi_p5.mp4` (writes the `BlockAdditionMapping` with `dvcC`); `tools/make_mkv_lacing.py --lacing xiph --frames-per-block 4 syn_webm_lacing_xiph.webm` (committed pure-Python EBML writer, one Opus track generated from `sine`, `SimpleBlock`s with Xiph lacing — mkvmerge and FFmpeg choose lacing themselves, so a generator is used); ✓ |
| 72 | `syn_mp4_broken_stsc_descending`, `syn_mp4_broken_ctts_v0_negative`, `syn_mp4_broken_size7_in_moov` | `broken` | `tools/make_broken.py --stsc-descending`, `--ctts-v0-negative`, `--box-size 7 --in moov` over the 20 KiB base file (written to `tests/corpora/synthetic/isobmff-broken/` under their chapter 8 file names); ✓ |
| 73 | `syn_pat_pq_patches_1000`, `syn_pat_hlg_patches`, `syn_pat_sdr709_ramp`, `syn_pat_p3_saturation`, `syn_pat_siting_checker` | `patterns` | produced by `tools/gen_hdr_patterns.py` of chapter 10 WI-1.413 into `tests/corpora/patterns/<name>.mp4` + `<name>.expected.json` (pattern definitions in chapter 10 §10.9.2); registered in this manifest with tag `patterns` (there is no separate pattern manifest); ✓ |
| 74 | `syn_av1_globalmotion` | `global_motion`, `av1` | `ffmpeg -i src_1080p_8bit.y4m -vf "crop=1280:720:'t*64':0" -frames:v 30 -pix_fmt yuv420p syn_av1_globalmotion_src.y4m && aomenc --codec=av1 --cpu-used=4 --enable-global-motion=1 --end-usage=q --cq-level=30 --kf-max-dist=30 --lag-in-frames=0 --ivf -o syn_av1_globalmotion.ivf syn_av1_globalmotion_src.y4m` (horizontal pan of 64 px/s). Expected: at least one inter frame has `is_global` set for a reference; chapter 7 WI-1.118 test 3 records the `gm_params` values. Replaces the gated Argon global-motion streams. ✓ |
| 75 | `syn_m4a_aac_lc` | `aac`, `audio` | `ffmpeg -f lavfi -i sine=frequency=1000:sample_rate=48000 -ac 2 -t 2 -c:a aac -profile:a aac_low syn_m4a_aac_lc.m4a` (AAC-LC: `audioObjectType` 2, `samplingFrequencyIndex` 3, 2 channels; chapter 8 WI-1.207 test 4). ✓ |
| 76 | `syn_raw_yuv420p10le_1080p` (file name `foo_1920x1080_yuv420p10le.yuv`, chosen so that the raw-format guesser sees geometry and pixel format in the name) | `raw`, `10bit` | `ffmpeg -i src_1080p_10bit.y4m -frames:v 30 -f rawvideo -pix_fmt yuv420p10le foo_1920x1080_yuv420p10le.yuv` (exactly 30 frames; chapter 4 FR-CNT-014, chapter 8 WI-1.229). ✓ |

Recipes 38–76 use the extra sources `src_1080p_8bit.y4m` (recipe 25) and `src_1080p_8bit.yuv` / `src_720p_8bit.yuv` (raw copies for HM, `ffmpeg -i <y4m> -f rawvideo <yuv>`). Every helper named in a recipe (`tools/corrupt.py`, `make_broken.py`, `sei_inject.py`, `y4m_poke.py`, `av1_set_level.py`, `make_multi_pps.py`, `make_mkv_lacing.py`, `av1_insert_hdr_obus.py`, `ts_inject_errors.py`, `gen_heif_items.py`) is a committed, deterministic Python script produced by WI-0.553 except `gen_hdr_patterns.py` (chapter 10 WI-1.413). Every `*_expected.json` sidecar named above is written by `generate.py --check` from the oracle run and committed next to the recipe; acceptance tests in other chapters read expected values from these files, never from numbers typed into the test. `✓` marks `fast`-subset membership.

**Non-stream fixtures (FlatBuffers and binary inputs).** These are generated by committed scripts, hashed in the manifest under `source: "syn"` with `kind: "fixture"`, and regenerated by `generate.py --fixtures`; chapters 6, 9 and 10 reference them by name:

| Fixture | Generator | Used by |
|---|---|---|
| `tests/perf/data/fa_4k_dense.bin` | `tools/gen_dense_fa.py` — a `FrameAnalysis` of a 3840×2160 AV1 frame, every 4×4 cell inter with an MV, QP ramp | `bench_overlay_4k_mv_arrows`, `bench_overlay_build` (§14.9.3) |
| `render/tests/fixtures/synthetic_4k_grid.fb`, `synthetic_4k_mv.fb`, `synthetic_8k_mv.fb` | `tools/gen_fixture_fb.py --kind grid\|mv --size 4k\|8k` — `FrameAnalysis` with a regular partition grid / dense MVs | chapter 10 §10.9.2 overlay goldens |
| `render/tests/fixtures/real_1080p_hevc_frame.fb`, `real_4k_hevc_frame.fb` | `ovt analyze syn_h265_gop60_1080p.mp4 --frame 59 --plugin de265 --format fb -o …` (and `syn_av1_4k_2min` frame 59 via `aom` for the 4K file) — real plugin output, committed | chapter 10 §10.9.3 `bench_render_video_4k`, `bench_render_first_frame` |
| `render/tests/fixtures/pat_*.fb` (`pat_pq_ramp`, `pat_hlg_ramp`, `pat_bt709_bars`, `pat_siting_checker`, …) | `tools/gen_fixture_fb.py --kind pattern --name <id>` from the pattern definitions of chapter 10 WI-1.413 (`Picture` planes with the §10.9.2 code values) | chapter 10 §10.9.2 render goldens, §14.10.5 checklist |
| `core/tests/golden/tiny16_hevc.fa`, `tiny32_h264.fa`, `tiny64_av1.fa` (+ `.sha256`) | generated by `core/tests/golden/gen_fixtures.cpp` (deterministic builder code of chapter 6 WI-0.093; hand-built, not cropped from an analysed stream; regenerated, never hand-edited); `fast` | chapter 6 WI-0.084–WI-0.093 schema, validator, exporter and round-trip tests (`test_model_roundtrip`) |
| `core/tests/golden/tiny16.yuv` | generated by `gen_fixtures.cpp` (16×16 4:2:0 synthetic gradient); `fast` | YUView export checks of chapter 6 WI-0.089 |
| `core/tests/golden/tiny16_hevc.json`, `tiny16_hevc.csv`, `tiny64_av1.inspect.json`, `aom_inspect_keys.txt` | goldens written by chapter 6 WI-0.088 / WI-0.089 / WI-0.090 (the inspect golden by `gen_fixtures.cpp`); `fast` | chapter 6 JSON, YUView CSV and libaom-inspect exporters |
| `tests/golden/epb/syn_h264_epb_dense.json` | `tests/oracles/normalize/trace_headers.py --epb-offsets` over recipe 22 | chapter 11 WI-1.452, WI-1.458 |
| `tests/corpora/hdr10plus/f000.bin`, `f030.bin`, `f059.bin` | `tools/sei_inject.py --extract t35 --frames 0,30,59 syn_h265_hdr10plus.265` (raw ST 2094-40 payloads of recipe 48) with reference JSON from `ffprobe -show_frames -show_entries frame_side_data_list` | chapter 10 `test_hdr10plus_map` |
| `tests/corpora/render-ref/<id>.png` | `tests/oracles/mpv_capture.py` (WI-1.575) over the `patterns`-tagged ids; hashes in `manifest.json` entries with `kind: "fixture"` and tag `render-ref` | §14.4.15; chapter 10 §10.9.2 |

Rules for `generate.py`: every command above is stored verbatim in `recipes.json` (`"cmd": [...]` as an argv list, no shell); the script refuses to run if the encoder's `--version` string differs from `recipes.json`'s pin; outputs are hashed and compared with `manifest.json`; a mismatch prints a diff of the encoder versions and exits 5 (the maintainer then regenerates goldens with `OVT_UPDATE_GOLDEN=1`). Entries 2, 15a and 20 have more than one producer; the manifest records the one whose output is pinned. Every corpus id or fixture path that another chapter names MUST have a row in this section, in §14.3.1, or in the alias index of §14.3.5 (`tools/check_fast_subset.py --all-chapters` greps `docs/spec/` for `syn_*`, `corpus/*`, `*.fb`, `*.fa`, `*.bin` and container-file names and fails on an unknown one).

**Reproducibility adjustments (#119).** `recipes.json` is the executable form of the table above; where a cell cannot run verbatim without a shell or would not give the same bytes on every runner, `recipes.json` applies these rules and records a `note` on the recipe, and `generate.py` rejects a recipe that breaks rules 1–2 (exit 4):

1. Thread counts are pinned, because x264, x265 and libaom output depends on them: `x264 --threads 1`, `x265 --frame-threads 1 --pools 1`, `aomenc --threads=1`, FFmpeg `-threads 1` after `-c:v libx264|libaom-av1` and `frame-threads=1:pools=1` in `-x265-params`. The `local_only` perf recipes (27, 28, 31, 61) use the fixed counts `8` (x264, aomenc) and `--frame-threads 4 --pools 8` (x265).
2. Muxers write no wall-clock dates or random ids: `MP4Box -for-test`, `mkvmerge --no-date --deterministic 1`; lavfi `life` and `cellauto` (recipe 59) get `seed=1`; Bento4 `mp4encrypt` (recipes 68, 69) uses a fixed test key, KID and IV instead of `random`.
3. FFmpeg writes 10-bit Y4M only with `-strict -1` (Source B, recipes 28, 41, 58); raw Annex B input to the MP4 wrapping step gets `-framerate 30`.
4. Tool paths and arguments: HM and JM encoder configs are `{hm_cfg}/…cfg` and `{jm_cfg}/encoder_main.cfg` (installed by the oracle image); JM `lencod` is the pinned producer of recipe 2; libaom `scalable_encoder` takes `av1 1280 720 <i420> <i420> <out.obu> 60` (recipe 16); recipe 31 concatenates `frag3600_init.mp4` and the segments with `cat`, because `MP4Box -cat … -new` writes a de-fragmented file; recipe 68 `syn_mp4_frag_cbcs` encrypts `MP4Box -frag 500 syn_mp4_elst.mp4`; recipe 22 uses `--qp 1`, because x264 refuses `--qp 0` with `--profile high`; recipe 41 grades with `lutyuv=y='if(lt(val,800),val,min(val+60,maxval))'`, because `lutyuv` names the sample `val`, not `Y`; recipe 56 sets full range with `-color_range pc`, because libx264 rejects `-x264-params range=pc`; recipe 28 passes `--bit-depth=10 --input-bit-depth=10`; recipe 27 loops with `ffmpeg -stream_loop 59|359 … -f yuv4mpegpipe -` into the recipe-26 encoder; the decoded-Y4M halves of recipes 53, 56 and 58 are produced by the generator.
5. Path aliases of §14.3.5 (`corpus/<dir>/<file>`) are resolved by `check_fast_subset.py`; only bare file names are manifest `aliases`.

### 14.3.4 Corpora licence check

`tools/license_gate.py --corpora` (also run as part of the dependency gate) enforces:

1. every manifest entry has `licence`, `licence_spdx`, `redistributable`;
2. `redistributable=true` requires `licence_spdx` ∈ {`CC0-1.0`, `CC-BY-4.0`, `BSD-2-Clause`, `BSD-3-Clause`, `BSD-2-Clause-Patent`, `MIT`, `Apache-2.0`} (extend by PR, never by CI override);
3. entries with `redistributable=false` are never copied into release artefacts, Docker images or caches that are publicly readable (the GitHub Actions cache is private to the repo; that is allowed);
4. no entry has `licence_spdx` containing `AGPL`, `SSPL`, `BUSL` or `NonCommercial`;
5. `tests/corpora/LICENSES.md` is regenerated and must match the committed file (fails if stale).

### 14.3.5 Fixture alias index (D26)

Some chapters cite a fixture by a descriptive file name or a legacy `corpus/<dir>/<file>` path. Those names are aliases: the fetcher materialises each alias as a symlink to the registry entry (`fetch_corpora.py` writes `<cache>/alias/<name>` from the `aliases` array of the manifest entry), and `check_fast_subset.py --all-chapters` accepts a name only if it is a registry id or appears in this table. New tests MUST use registry ids.

| Name as cited | Registry id | Source | Fast | Cited by |
|---|---|---|---|---|
| `isom_basic.mp4` | `mpegff_a1_foreman_qcif` | `mpegff` | ✓ | 08 WI-1.205 |
| `hev1_main10.mp4` | `syn_h265_main10_hev1` | `syn` (recipe 67) | ✓ | 08 WI-1.207, WI-1.233 |
| `av01_10bit.mp4` | `syn_av1_filmgrain_10bit` (MP4 of recipe 15) | `syn` | ✓ | 08 WI-1.207 |
| `dv_p81.mp4` | `hdr_hdr10plus_dv81` | `hdr` | ✓ | 08 WI-1.208, WI-1.217 |
| `encv_cenc.mp4` | `syn_mp4_cenc` | `syn` (recipe 68) | ✓ | 08 WI-1.217 |
| `cenc_cbcs_keyrotation.mp4` | `syn_mp4_cbcs_keyrot` | `syn` (recipe 68) | ✓ | 08 WI-1.210 |
| `bento4_widevine.mp4` | `syn_mp4_cenc_widevine` | `syn` (recipe 68) | ✓ | 08 WI-1.213 |
| `gpac_cbcs.mp4` | `syn_mp4_frag_cbcs` | `syn` (recipe 68) | ✓ | 08 WI-1.213 |
| `heif_heic_grid.heic` | `heif_heic_grid` | `heif` | ✓ | 08 WI-1.214 |
| `avif_10bit.avif` | `syn_avif_10bit` | `syn` (recipe 70) | ✓ | 08 WI-1.214 |
| `itunes_tagged.m4a` | `syn_m4a_itunes` | `syn` (recipe 70) | ✓ | 08 WI-1.215 |
| `aac_lc.m4a` | `syn_m4a_aac_lc` | `syn` (recipe 75) | ✓ | 08 WI-1.207 |
| `foo_1920x1080_yuv420p10le.yuv` | `syn_raw_yuv420p10le_1080p` | `syn` (recipe 76) | ✓ | 04 FR-CNT-014, 08 WI-1.229 |
| `mini.mp4` | `syn_h264_multislice_cavlc_mp4` | `syn` (recipe 29) | ✓ | 13 WI-0.051 |
| `nero_chapters.mp4` | `syn_mp4_nero_chapters` | `syn` (recipe 70) | ✓ | 08 WI-1.215 |
| `smooth_piff.ismv` | `syn_mp4_piff` | `syn` (recipe 69) | — | 08 WI-1.216, WI-4.212 |
| `dv_p5.mkv` | `syn_mkv_dv_p5` | `syn` (recipe 71) | ✓ | 08 WI-4.202 |
| `hdr10.mkv` | `syn_mkv_hdr10` | `syn` (recipe 71) | ✓ | 08 WI-4.202 |
| `lacing_xiph.webm` | `syn_webm_lacing_xiph` | `syn` (recipe 71) | ✓ | 08 WI-4.202, WI-4.203 |
| `test1.mkv` … `test8.mkv` | `mkv_test1` … `mkv_test8` | `mkv` | `test1/3/5` | 08 WI-4.201–WI-4.203 |
| `tests/corpora/synthetic/isobmff-broken/stsc_descending.mp4`, `ctts_v0_negative.mp4`, `size7_in_moov.mp4` | `syn_mp4_broken_stsc_descending`, `syn_mp4_broken_ctts_v0_negative`, `syn_mp4_broken_size7_in_moov` | `syn` (recipe 72) | ✓ | 08 WI-1.201, WI-1.209 |
| `pixel_camera.mp4`, `motion_photo.mp4` | `misc_pixel_motion_photo` | `misc` | — | 08 WI-1.215, WI-1.216 |
| `corpus/h264/gop.mp4` | `syn_h264_gop60_1080p` | `syn` (recipe 26) | ✓ | 12, 15 |
| `corpus/h265/gop.hevc` | `syn_h265_gop60_1080p` | `syn` (recipe 26) | ✓ | 12 |
| `corpus/h265/basic.hevc` | `syn_h265_amp_sao_on` | `syn` (recipe 8) | ✓ | 12 |
| `corpus/av1/gop.ivf` | `syn_av1_gop60_1080p` | `syn` (recipe 26) | ✓ | 12, 15 |
| `corpus/h265/jct-vc/WPP_A_ericsson_MAIN_2.bit` | `jctvc_WPP_A_ericsson_MAIN_2` | `jctvc` | ✓ | 12 |
| `corpus/broken/h265_hash_mismatch.hevc` | `syn_broken_h265_hash_mismatch` | `syn` (recipe 50) | ✓ | 12 |
| `corpus/broken/h265_truncated_sps.hevc` | `syn_broken_h265_truncated_sps` | `syn` (recipe 49) | ✓ | 12 |
| `corpus/mp4/hdr10_hevc.mp4`, `hevc_main10.mp4` | `syn_h265_main10_pq_hdr` | `syn` (recipe 10) | ✓ | 12 |
| `corpus/perf/1h_1080p_h264.mp4`, `syn_h264_1h_1080p.mp4` | `syn_h264_1h_1080p` | `syn` (recipe 27, `local_only`) | — | 08, 12 |
| `corpus/vq/ref.y4m`, `corpus/vq/dist.mp4` | `syn_metrics_1080p_pair` (reference / distorted) | `syn` (recipe 25) | ✓ | 11, 12 |
| `corpus/vq/ref_bitflip_f37.y4m` | `syn_metrics_1080p_bitflip_f37` | `syn` (recipe 52) | ✓ | 12 |
| `pq_patches_1000.mp4`, `hlg_patches.mp4`, `sdr709_ramp.mp4`, `p3_saturation.mp4`, `siting_checker.mp4` (also under `tests/corpora/patterns/`) | `syn_pat_pq_patches_1000`, `syn_pat_hlg_patches`, `syn_pat_sdr709_ramp`, `syn_pat_p3_saturation`, `syn_pat_siting_checker` | `syn` (recipe 73) | ✓ | 09, 10 |
| "hierarchical-B fixture" | `syn_h265_bpyramid` | `syn` (recipe 43) | ✓ | 11 |
| `corpus/jctvc_rext_*`, `corpus/aom_*`, `corpus/argon_*` | the `rext`-tagged `jctvc` entries / the `aom` `fast` entries / the nightly `argon` `profile0_core` subset (§14.3.2) | — | — | 04 |
| "an x265 stream with HDR10+ metadata" | `syn_h265_hdr10plus` | `syn` (recipe 48) | ✓ | 07 WI-1.120 |
| "a broadcast stream with GA94 captions" | `syn_sei_h264_ga94_cc` | `syn` (recipe 47) | ✓ | 07 WI-1.120 |

Fixtures that cannot be produced as described by the citing chapter: none remain open after the aliases above; a citing chapter that needs a property a recipe cannot produce (for example an H.264 long-term-reference stream from x264, recipe 3) MUST use the conformance stream named in the recipe's note.


---
## 14.4 Oracle-based golden tests

### 14.4.0 Common mechanics

Every oracle test follows the same shape, implemented once in `core/tests/test_support/golden.cpp` and `tests/oracles/normalize/*.py`:

1. **Oracle run** (nightly, inside the oracle image): run the oracle command on a corpus file, convert its native output to the **canonical JSON** of that test family with the normaliser script, and commit the result as `tests/golden/<codec-or-area>/<corpus-id>.<tool>.json` (plus the raw oracle text as `.txt`/`.xml` when it is small, ≤ 1 MiB, for debugging).
2. **OVT run** (every CI run): produce OVT's output via the C API or the `ovt` CLI, convert to the same canonical JSON with a C++ normaliser (`fb_normalize.cpp`) and compare.
3. **Comparison**: structural JSON equality after sorting object keys; numeric fields compared exactly unless the test family defines a tolerance; missing-in-oracle fields are ignored (the oracle is the *lower bound* of what OVT must report), missing-in-OVT fields fail.
4. **Update**: `OVT_UPDATE_GOLDEN=1 ctest -L golden` rewrites OVT-side goldens (used only when the oracle also changed; the PR must say why).

Canonical JSON families used below:

- **`syntax-elements`**: `[{"unit": <unit index>, "name": "<syntax element>", "bit_offset": <int>, "bit_length": <int>, "value": <int|string>}]` sorted by `(unit, bit_offset)`. `bit_offset` is relative to the RBSP (H.26x) or OBU payload (AV1) start, matching `BitSpan` in §0.5.1.
- **`header-struct`**: a nested object `{"sps": {...}, "pps": {...}, "slice_header": {...}}` with spec field names as keys and integers/arrays as values (no offsets).
- **`block-grid`**: `{"w4": <cells_w>, "h4": <cells_h>, "fields": {"<field>": [<int per cell>]}}` — the `CellGrid` of §0.5.4 with a selected set of fields.
- **`box-tree`**: `[{"path": "moov/trak[1]/mdia/minf/stbl/stsz", "offset": <int>, "size": <int>, "fields": {"<name>": <value>}}]`.
- **`sample-table`**: `[{"i": n, "dts": .., "cts": .., "size": .., "offset": .., "sync": 0|1}]`.
- **`scores`**: `{"frames": [{"i": n, "<metric>": <double>}], "pooled": {"<metric>": {"mean": .., "harmonic_mean": .., "min": ..}}}`.

### 14.4.1 FFmpeg `trace_headers` — header syntax values and bit positions

| Item | Value |
|---|---|
| Validates | `core/src/syntax/` H.264, HEVC, AV1 header parsers: every syntax element's **name, value and bit position** (`SyntaxNode.name`, `uvalue/ivalue`, `span`). |
| Obtain | FFmpeg ≥ 7.1 built with `--enable-gpl` is not required for `trace_headers`; the oracle image builds `n7.1.1` from https://github.com/FFmpeg/FFmpeg at that tag. |
| Command | `ffmpeg -hide_banner -loglevel trace -i <in> -map 0:v:0 -c:v copy -bsf:v trace_headers -f null - 2> <id>.trace_headers.txt` For MP4 input add `-bsf:v h264_mp4toannexb,trace_headers` (HEVC: `hevc_mp4toannexb`). |
| Oracle output | Lines like `[trace_headers @ 0x...] pic_order_cnt_lsb                              0000000000 = 0` with, in trace level, the bit position column: CBS prints `<position>  <name> <bits> = <value>` where position is the bit offset within the current unit (see `libavcodec/cbs.c` `cbs_trace_syntax_element`). The normaliser `normalize/trace_headers.py` parses `^\s*(\d+)\s+(\S+)\s+([01]+)\s*=\s*(-?\d+)` plus the `Packet: N bytes` / `OBU`/`NAL unit` delimiters to assign unit indices. |
| OVT side | `ovt syntax <in> --format json --fields name,span,value --all-units` (chapter 12) → `syntax-elements`. OVT reports positions relative to the RBSP; CBS also reports after emulation-prevention removal, so no EPB correction is needed. For AV1, CBS positions are relative to the OBU start **including** the OBU header, whereas OVT's `BitSpan` is relative to the OBU payload; the normaliser subtracts `8 × (1 + has_extension) + leb128 size bytes × 8`. |
| Tolerance | exact for `name`, `bit_offset`, `bit_length`, `value`. Elements CBS does not print (e.g. `rbsp_trailing_bits`, `byte_alignment`) are excluded via `normalize/trace_headers_ignore.txt`. Names differing between CBS and the spec are mapped by `normalize/cbs_name_map.json` (e.g. CBS `nal_unit_header` vs spec `forbidden_zero_bit` children); any unmapped CBS name fails the test. |
| Corpora | all `fast` `jvt`, `jctvc`, `aom` streams and all `syn_*`; nightly whole corpora. |
| Golden file | `tests/golden/<codec>/<id>.trace_headers.json` |

### 14.4.2 GStreamer codecparsers — a tiny C oracle program

| Item | Value |
|---|---|
| Validates | parsed **values** of SPS/PPS/VPS/slice headers/sequence & frame headers (`header-struct` family), independently of FFmpeg; catches cases where OVT and CBS share a misreading. |
| Obtain | `libgstreamer-plugins-bad1.0-dev` ≥ 1.24 (Ubuntu 24.04) in the oracle image. |
| Build | `tests/oracles/gst_parse_oracle.c`, built by `tests/oracles/CMakeLists.txt` with `pkg-config --cflags --libs gstreamer-codecparsers-1.0`. ≈ 400 lines: reads Annex B (`gst_h264_parser_identify_nalu` / `gst_h265_parser_identify_nalu`) or an OBU stream (`gst_av1_parser_identify_one_obu`), calls `gst_h264_parser_parse_sps/pps/slice_hdr`, `gst_h265_parser_parse_vps/sps/pps/slice_hdr`, `gst_av1_parser_parse_sequence_header_obu/frame_header_obu/tile_group_obu`, and prints one JSON object per unit using a generated field list (`tests/oracles/gst_fields.h` lists the struct members to print with their spec names; generated once from the GStreamer headers by `tools/gen_gst_fields.py` and committed). |
| Command | `gst_parse_oracle --codec h264\|h265\|av1 [--annexb\|--obu\|--ivf] <in> > <id>.gstparse.json` |
| OVT side | `ovt syntax <in> --format json --all-units --fields name,value,enum_name,children` → `header-struct` (values only, no offsets; chapter 12 §12.2.3). |
| Tolerance | exact integers; arrays compared element-wise; fields GStreamer derives rather than parses (`fps_num`, `width/height` from cropping) are in `normalize/gst_ignore.txt`. |
| Corpora | same as 14.4.1. |
| Golden file | `<id>.gstparse.json` |

### 14.4.3 JM `ldecod` with TRACE — H.264 macroblock-level syntax

| Item | Value |
|---|---|
| Validates | `ovt-plugin-ffmpeg` (phase 2: MVs, QP per MB; phase 4: `mb_type`, `sub_mb_type`, `ref_idx`, intra modes, `cbp`) and the H.264 slice-data symbols listed in the `AvcMb` extension table (chapter 06). Also validates `FrameInfo.poc` and `declared_refs`. |
| Obtain/build | `tests/oracles/build_jm.sh`: download JM 19.0 from https://iphome.hhi.de/suehring/tml/download/jm19.0.zip, verify SHA-256 pinned in the script, `cd JM/ldecod && sed -i 's/^#define TRACE .*/#define TRACE 1/' inc/defines.h && make -j`. (`-DTRACE` is a compile-time macro; see `ldecod` docs https://iphome.hhi.de/suehring/tml/doc/ldec/html/index.html.) Licence: ITU/ISO reference software terms (permissive; not AGPL). |
| Command | `ldecod -p InputFile=<in>.264 -p OutputFile=<id>.ldecod.yuv -p RefOffset=0 -p POCScale=2 -p WriteUV=1 -p SilentDecode=1` ; the trace is written to `trace_dec.txt` in the working directory (fixed name). MP4 inputs are first converted: `ffmpeg -i in.mp4 -c:v copy -bsf:v h264_mp4toannexb -f h264 in.264`. |
| Oracle output | `trace_dec.txt` lines `@<bitpos>  <element name>   <bits>   (<value>)` with MB context lines `*********** POC: N (I/P) MB: M Slice: S Type K **********`. `normalize/jm_trace.py` emits per MB `{poc, mb_addr, mb_type, sub_mb_type[4], ref_idx_l0[4], ref_idx_l1[4], mvd_l0[16][2], mvd_l1[16][2], intra4x4_pred_mode[16], intra_chroma_pred_mode, cbp, mb_qp_delta, transform_size_8x8_flag}` and per-slice `{slice_type, slice_qp_delta, num_ref_idx_active[2]}`. |
| OVT side | `ovt analyze <in> --plugin ffmpeg --all --order decode --format json` (blocks and `ext` are included by default, chapter 12 §12.2.8) → for each `Block` with `ext_type == "AvcMb"` the normaliser emits the same keys from the `AvcMb` table (chapter 06 field names are the spec names, so the map is identity). Phase 2 (public-API FFmpeg) only fills `qp` and MVs: the phase-2 test compares `qp` (final QP_Y; the normaliser accumulates `mb_qp_delta` from the JM trace) and skips the other keys; derived MVs are not in the JM trace, so phase-2 MVs are checked against FFmpeg's own side-data export (§14.4.3a). |
| Tolerance | exact integers. Known JM quirk: `mb_skip_flag` MBs produce no trace lines → the normaliser synthesises `mb_type="P_Skip"/"B_Skip"`. |
| Corpora | `jvt` `fast` subset; nightly all `jvt` streams that JM decodes (list in `manifest.json` `tests` field). |
| Golden file | `<id>.jm-trace.json` |

**14.4.3a FFmpeg motion-vector cross-check (phase 2).** `ffprobe` prints only the side-data *type* and size for `AV_FRAME_DATA_MOTION_VECTORS`, never the vectors, so the oracle is a small libavcodec program: `tests/oracles/ffmpeg_mvs.c` (≈ 120 lines, derived from FFmpeg's `doc/examples/extract_mvs.c`, LGPL) opens the input with libavformat, sets `AVCodecContext.flags2 |= AV_CODEC_FLAG2_EXPORT_MVS`, and for every decoded frame writes one CSV row per `AVMotionVector` of the `AV_FRAME_DATA_MOTION_VECTORS` side data: `framenum,source,blockw,blockh,srcx,srcy,dstx,dsty,flags,motion_x,motion_y,motion_scale` (`ffmpeg_mvs <in> > <id>.ffmpeg-mvs.csv`). The normaliser `normalize/ffmpeg_mvs.py` maps each row to `MotionVector{rect, mv_x, mv_y, list}` in 1/16-pel: FFmpeg's `motion_x/motion_y` are in `1/motion_scale` luma-sample units (`motion_scale = 4` for H.264), so `mv_x_16 = motion_x × 16 / motion_scale`; `rect = {dst_x - w/2, dst_y - h/2, w, h}` (FFmpeg reports the block centre); `list = 0` for `source < 0`, `1` for `source > 0`. Compared exactly against the plugin's `FrameAnalysis.mvs` for 16×16/16×8/8×16/8×8 partitions (FFmpeg collapses sub-8×8; OVT's normaliser collapses to match by taking the top-left 8×8 sub-partition vector). With `--qp` the program additionally sets `AVCodecContext.export_side_data |= AV_CODEC_EXPORT_DATA_VIDEO_ENC_PARAMS` and writes a second CSV `<id>.ffmpeg-qp.csv` with one row per `AVVideoBlockParams` of `AV_FRAME_DATA_VIDEO_ENC_PARAMS`: `framenum,src_x,src_y,w,h,qp,delta_qp` (`qp` = `AVVideoEncParams.qp`). FFmpeg's `qp + delta_qp` **includes** `QpBdOffsetY`; `normalize/ffmpeg_mvs.py --qp` subtracts `6·(bit_depth_luma − 8)` before comparing with `FrameAnalysis` `grid.qp` (chapter 7 WI-2.170 test 2).

### 14.4.4 HM with `ENC_DEC_TRACE` and `TComDataCU` getters — HEVC CU/PU/TU truth

| Item | Value |
|---|---|
| Validates | `ovt-plugin-de265` block fields (`HevcCu`, `HevcPu`, `HevcTu` extension tables, `CellGrid.pred/qp/intra_mode_*/mv*/ref*/tx_*/cbf`), `FrameAnalysis.refs`, deblocking `bS` where available, and (phase 4) the `ovt-plugin-hm` plugin itself (which must equal the oracle by construction). |
| Obtain/build | `tests/oracles/build_hm.sh`: `git clone --branch HM-18.0 https://vcgit.hhi.fraunhofer.de/jvet/HM` (tag pinned), build **two** decoders: (a) stock `TAppDecoder`; (b) `TAppDecoder_trace` with `-DENC_DEC_TRACE=1` added to `source/Lib/TLibCommon/TypeDef.h` (the macro writes `TraceDec.txt` through `g_hTrace`). Also build the **getter dumper** `tests/oracles/hm_cu_dump.cpp`: a ≈ 300-line program linked against HM's `TLibDecoder`/`TLibCommon` that runs `TDecTop` on the stream and, after each picture, walks every CTU's `TComDataCU` printing per 4×4 unit: `getPredictionMode, getPartitionSize, getDepth, getSkipFlag, getMergeFlag, getMergeIndex, getIntraDir(CHANNEL_TYPE_LUMA/CHROMA), getQP, getCbf(COMPONENT_Y/Cb/Cr), getTransformIdx, getTransformSkip, getCUMvField(REF_PIC_LIST_0/1)->getMv/getRefIdx/getMvd, getInterDir, getMVPIdx, getIPCMFlag, getCUTransquantBypass` into the `block-grid` family. HM is BSD-3-Clause. |
| Commands | `TAppDecoder_trace -b <in>.bit -o /dev/null --SEIDecodedPictureHash=1` → `TraceDec.txt`; `hm_cu_dump -b <in>.bit > <id>.hm-grid.json` |
| OVT side | `ovt analyze <in> --plugin de265 --all --format json --fields pred,qp,intra_mode_y,intra_mode_c,mv0_x,mv0_y,mv1_x,mv1_y,ref0,ref1,tx_log2_w,cbf` (blocks and `ext` included by default, chapter 12 §12.2.8) → `block-grid`. Mapping: HM `getIntraDir` 0..34 ↔ `intra_mode_y` (identity); HM MVs are in 1/4-pel → ×4 to OVT's 1/16-pel; HM `refIdx` ↔ `ref0/ref1` through `FrameAnalysis.refs[i].name == "L0[refIdx]"`; `getPartitionSize` ↔ `Block.partition_type` (`SIZE_2Nx2N=0 … SIZE_nRx2N=7`); `getDepth` ↔ `Block.level`. Fields libde265 cannot provide (`merge_idx`, `mvp_idx`, `mvd` — transient in libde265 unless the patch stores them, see chapter 07) are compared only when `ovt_AnalysisCaps.grid_fields` declares them. |
| Tolerance | exact. |
| Corpora | `jctvc` `fast` subset (Main/Main10) for de265; `RExt` streams for the HM plugin only. |
| Golden file | `<id>.hm-grid.json`, `<id>.hm-trace.txt` (raw trace kept for the first 2 frames only). |

### 14.4.5 libaom `inspect` JSON — AV1 blocks and bit accounting

| Item | Value |
|---|---|
| Validates | `ovt-plugin-aom`: all `Av1Block`/`Av1Txb` extension fields, `CellGrid` fields, `SymbolCost` accounting, `qp_hist`, `frame.type/shown/show_existing_of`, tile layout in `partitions`. Also the AV1 `index` (frame counts incl. hidden frames). |
| Obtain/build | `tests/oracles/build_libaom_inspect.sh`: `git clone https://aomedia.googlesource.com/aom -b v3.12.1` (pin matches `third_party/VERSIONS.md`), `cmake -B build -DCONFIG_INSPECTION=1 -DCONFIG_ACCOUNTING=1 -DENABLE_TESTS=0 -DENABLE_DOCS=0 && cmake --build build --target inspect aomdec`. Produces `build/examples/inspect`. |
| Command | `inspect -A -a <in>.ivf > <id>.aom-inspect.json` where `-A/--all` dumps **every** block-level layer (block size, transform size/type, modes, MVs, skip, filters, CDEF, reference frames, delta-q, segment id, compound type, motion mode, CfL, intrabc, palette, UV palette, dual filter) and `-a/--accounting` adds the per-symbol bit accounting; `-x` RLE compression is **not** used so the JSON stays plain. The flags `-bs`, `-mv`, `-ts`, `-tt`, `-m`, `-mm`, `-ct`, `-uvm`, `-s`, `-f`, `-c`, `-cfl`, `-df`, `-r`, `-dq`, `-si`, `-ibc`, `-plt`, `-uvp` select single layers (they are command-line flags, not JSON keys). The flag table can differ between libaom versions, so `build_libaom_inspect.sh` extracts the arg table from the pinned `examples/inspect.c` into `tests/oracles/ORACLE_VERSIONS.md` and the normaliser refuses an output whose key set differs from the pinned one. For `.obu` Annex B input the oracle first wraps with `aomdec --annexb ... --rawvideo` — not needed: `inspect` accepts IVF and OBU (Section 5) input; Annex B streams are converted with `ffmpeg -i in.obu -c copy -f ivf in.ivf`. |
| Oracle output | one JSON array per stream: `[`, then one JSON object per decoded frame each followed by `,`, terminated by `null` and `]` (libaom v3.12.1 `examples/inspect.c`; the framing of chapter 6 §6.8.3; not JSON Lines; D30; frames output with `show_existing_frame = 1` produce **no** object). The key set is exactly the one of chapter 6 §6.8.3 and `normalize/aom_inspect_names.json` lists exactly these keys: frame keys `frame`, `showFrame`, `frameType`, `baseQIndex`, `tileCols`, `tileRows` (tile size in MI units), `deltaQPresentFlag`, `deltaQRes`, `config`, `configString`; per-layer 2-D arrays `blockSize`, `transformSize`, `transformType`, `dualFilterType`, `mode`, `uv_mode`, `motion_mode`, `compound_type`, `skip`, `filter`, `cdef_level`, `cdef_strength`, `cfl_alpha_idx`, `cfl_alpha_sign`, `delta_q`, `seg_id`, `motionVectors`, `intrabc`, `palette`, `uv_palette`, `referenceFrame`, each with its `<name>Map` legend, plus `referenceFrameMap`; accounting keys `symbolsMap` and `symbols` (`[id, bits, samples]`). There is no superblock (`sb`) layer and no per-superblock bit count: OVT's AV1 `Block.bits_x8` is derived from the accounting symbols only (chapter 6 §6.8.3). `normalize/aom_inspect.py` converts to `block-grid` (MI unit = 4×4 = OVT cell) plus `{"symbols": [{"x4","y4","name","bits_x8","count"}]}`. The oracle counts bits in 1/8-bit units (`AOM_ACCT_BITRES=3`), the same unit as `SymbolCost.bits_x8`; per-block AV1 bits are compared as accounting totals per `(x, y, symbol_id)`. |
| OVT side | `ovt analyze <in> --plugin aom --all --order decode --format json` (decode order includes hidden frames; grids, blocks, symbols and `ext` are all emitted by default, chapter 12 §12.2.8). OVT's `Av1Block` names are the spec names; `normalize/aom_inspect_names.json` maps inspect's keys (`mode`→`y_mode`, `palette`→`palette_size_y`, `uv_palette`→`palette_size_uv`, `intrabc`→`use_intrabc`, `cdef_level`/`cdef_strength`→ the luma primary/secondary strength selected by the block's `cdef_idx`, …); the normaliser refuses an output with a key not in that map. |
| Tolerance | exact for every grid field and for `symbols` sums per `(x4,y4,name)`; `bits_x8` per **block** must match the sum of the oracle's symbols for that block exactly. |
| Corpora | `aom` `fast` subset and all `syn_av1_*`; nightly all `aom` + the Argon `profile0_core` subset. |
| Golden file | `<id>.aom-inspect.json` (gzip-compressed when > 1 MiB: `.json.gz`). |

### 14.4.6 MP4Box `-diso` / `-dts` / `-dnal` — ISOBMFF tree, sample timing, NAL list

| Item | Value |
|---|---|
| Validates | `core/src/container/isobmff`: box tree completeness (`BoxNode.fourcc`, nesting, `version/flags`, decoded `fields`), derived `SampleTable` (DTS/CTS/size/offset/sync), and the NAL-per-sample unit map. |
| Obtain | GPAC built from tag `v2.4.0` or newer pinned in `tests/oracles/ORACLE_VERSIONS.md` (`git clone https://github.com/gpac/gpac && ./configure --static-bin && make`). LGPL-2.1+. |
| Commands | `MP4Box -diso -std <in>.mp4 > <id>.mp4box-diso.xml`; `MP4Box -dts -std <in>.mp4 > <id>.mp4box-dts.txt`; `MP4Box -dnal 1 -std <in>.mp4 > <id>.mp4box-dnal.xml` (track 1 = first track; the normaliser runs it per video track id). |
| Oracle output | `-diso` XML: one element per box with attributes (`Size`, `Type`, `Version`, `Flags`, decoded fields such as `SampleCount`, `Timescale`); no file offsets. `-dts` text: per sample `DTS`, `CTS`, `RAP`, `size`, `offset` (offsets **are** present here). `-dnal`: per sample a list of NAL units with `size`, `type`, and for parameter sets their content. |
| OVT side | `ovt probe <in> --format json --expand all` → `box-tree` (offsets stripped for the `-diso` comparison, kept for `-dts`); `ovt probe <in> --samples --track <n> --format json` → `sample-table`; `ovt index <in> --with-units --format json` → per-sample NAL types/sizes (chapter 12 §12.2.2/§12.2.4). |
| Normalisation | `normalize/mp4box_diso.py` converts GPAC's attribute names to spec field names via `normalize/gpac_field_map.json` (e.g. `SampleCount`→`sample_count`, `CompositionOffset`→`sample_offset`). Boxes GPAC dumps as `UnknownBox` are compared on `fourcc`+`size` only. GPAC's `-dts` prints raw `ctts`-based composition times (no edit-list adjustment); OVT's `sample-table` uses raw values too (the edit-list–adjusted presentation timeline is a separate `--presentation` view, compared in §14.4.7 against mp4ff's `-l all` segment timing only for fragmented files). |
| Tolerance | exact; box **order** must match; `payload_span` is not compared. |
| Corpora | `mpegff`, `gpac`, `bento4`, `cta`, `apple`, `dashif` `fast` subsets and `syn_mp4_*`, `syn_*.mp4`. |
| Golden file | `<id>.mp4box-diso.json`, `<id>.mp4box-dts.json`, `<id>.mp4box-dnal.json` |

### 14.4.7 `mp4ff-info -l all:1` — second ISOBMFF opinion incl. CMAF/CENC

| Item | Value |
|---|---|
| Validates | box fields for CMAF/fragmented/encrypted files (`tfdt`, `trun` flags and per-sample arrays, `senc` IVs and subsamples, `saiz/saio`, `sgpd/sbgp` `seig`, `pssh`, `emsg`, `prft`, `sidx` references). |
| Obtain | `go install github.com/Eyevinn/mp4ff/cmd/mp4ff-info@v0.48.0` (pin). MIT. |
| Command | `mp4ff-info -l all:1 <in>.mp4 > <id>.mp4ff-info.txt` (`-l` takes `all:<level>` or `<box>:<level>`; bare `all` is not accepted) |
| Oracle output | indented text tree, `[fourcc] size=N` headers and ` - field: value` lines; `normalize/mp4ff_info.py` parses the indentation into `box-tree` with `normalize/mp4ff_field_map.json`. |
| OVT side | as 14.4.6. |
| Tolerance | exact on the intersection of fields both tools decode; mp4ff's per-sample `trun` rows are compared fully. |
| Corpora | `gpac`, `bento4`, `cta`, `dashif`, `syn_mp4_frag*`, `syn_mp4_cenc*`. |
| Golden file | `<id>.mp4ff-info.json` |

### 14.4.8 `mediainfo --Details=1` — box offsets and HDR interpretation

| Item | Value |
|---|---|
| Validates | `BoxNode.file_span.offset/length` for **every** box (MediaInfo's trace prints offsets), `header_length`, and the interpreted HDR summary (`ovt_ColorDescription` from `colr`/`mdcv`/`clli`/`dvcC` and from SEI). |
| Obtain | `mediainfo` CLI ≥ 24.x (`apt install mediainfo` in the oracle image or https://mediaarea.net/en/MediaInfo/Download). BSD-2-Clause. |
| Commands | `mediainfo --Details=1 --Output=XML <in> > <id>.mediainfo-trace.xml` (trace with offsets); `mediainfo --Output=JSON <in> > <id>.mediainfo.json` (interpreted: `MasteringDisplay_ColorPrimaries`, `MasteringDisplay_Luminance`, `MaxCLL`, `MaxFALL`, `HDR_Format`, `HDR_Format_Profile`, `colour_primaries`, `transfer_characteristics`, `matrix_coefficients`, `colour_range`). |
| OVT side | `ovt probe <in> --format json --boxes --expand all` (offsets) and `ovt probe <in> --format json --hdr` (the HDR summary; chapter 12). |
| Normalisation | `normalize/mediainfo_trace.py` extracts `<block offset="..." size="..." name="...">` elements whose `name` is a 4-character box type; maps QuickTime-style names to fourcc. The HDR summary is compared as a flat object: MediaInfo prints luminance as `min: 0.0001 cd/m2, max: 1000 cd/m2`; the normaliser converts to the integer units of `ovt_ColorDescription` (0.0001 cd/m² for MDCV luminance, cd/m² for CLL). |
| Tolerance | offsets/sizes exact; HDR fields exact after unit conversion; primaries compared as integers in 0.00002 units (`mdcv_primaries`). |
| Corpora | all ISOBMFF `fast` files; `hdr` entries; `syn_h265_main10_pq_hdr.mp4`, `syn_av1_filmgrain_10bit.mp4`. |
| Golden file | `<id>.mediainfo-trace.json`, `<id>.mediainfo-hdr.json` |

### 14.4.9 mp4box.js — Node script oracle (per-box start/size/hdr_size)

| Item | Value |
|---|---|
| Validates | box `file_span`, `header_length` and the set of recognised boxes (mp4box.js's `BoxParser` tables are a third independent registry); `uuid` boxes; HEIF item boxes. |
| Obtain | Node ≥ 20, `npm install mp4box@1.x` pinned in `tests/oracles/package.json` (+ `package-lock.json`). BSD-3-Clause. |
| Script | `tests/oracles/mp4boxjs_dump.mjs` (≈ 80 lines): reads the file into an `ArrayBuffer`, `const mp4 = MP4Box.createFile(); buf.fileStart = 0; mp4.appendBuffer(buf); mp4.flush();` then walks `mp4.boxes` recursively emitting `{path, start, size, hdr_size, type, uuid?}` and, for boxes with simple scalar members, `fields` taken from the parsed object's own enumerable numeric/string properties (excluding `start,size,hdr_size,type,boxes,data`). Output is `box-tree` JSON directly. |
| Command | `node tests/oracles/mp4boxjs_dump.mjs <in>.mp4 > <id>.mp4boxjs.json` |
| OVT side | `ovt probe <in> --format json --boxes --expand all`. |
| Tolerance | `start`/`size`/`hdr_size` exact for every box both tools list; field values exact on the intersection after `normalize/mp4boxjs_field_map.json`. mp4box.js stops at the first unknown box in some containers (it stores them as raw); such boxes are compared on fourcc/size only. |
| Golden file | `<id>.mp4boxjs.json` |

### 14.4.10 FFmpeg `psnr` / `ssim` / `libvmaf` filters — metric kernels (SDR)

| Item | Value |
|---|---|
| Validates | `core/src/metrics/kernels`: PSNR per plane and FFmpeg-style average (`psnr_avg` = PSNR of the mean MSE over Y, U, V weighted by sample count); the libvmaf host through FFmpeg as a *second* path (primary parity is the `vmaf` CLI, §14.4.11). FFmpeg's `ssim` filter is **not** an oracle for any OVT metric: chapter 9 §9.4.3 documents it as a different kernel with no parity test, and there is no `ssim_ffmpeg` id. |
| Command (PSNR) | `ffmpeg -hide_banner -i <dist>.y4m -i <ref>.y4m -lavfi "[0:v][1:v]psnr=stats_file=<id>.ffpsnr.txt" -f null -` |
| Command (VMAF) | `ffmpeg -hide_banner -i <dist>.y4m -i <ref>.y4m -lavfi "[0:v][1:v]libvmaf=model=version=vmaf_v0.6.1:feature=name=psnr\|name=float_ssim:log_path=<id>.ffvmaf.json:log_fmt=json:n_threads=4" -f null -` |
| Oracle output | `psnr` stats file: `n:1 mse_avg:… mse_y:… psnr_avg:… psnr_y:… psnr_u:… psnr_v:…` per frame (values printed with `%0.2f`; `inf` for identical frames); libvmaf JSON (same schema as the CLI). `normalize/ffmetrics.py` → `scores`. |
| OVT side | PSNR: `ovt metrics -r <ref>.y4m -d <dist>.y4m -m psnr --align none --threads 1 --format json -o <id>.ovt.json`; VMAF cross-check: `ovt metrics -r <ref>.y4m -d <dist>.y4m -m vmaf --model version=vmaf_v0.6.1 --opt vmaf.features='["psnr","float_ssim"]' --align none --threads 4 --format vmaf-json -o <id>.ovt.vmaf.json` (the libvmaf JSON layout, so both sides are compared by the same normaliser; chapter 12 §12.2.9 grammar). The normalisation stage MUST be a no-op for these tests (inputs already identical in size/format); the test asserts that `normalization.applied.ref` equals `normalization.applied.dist` and that both equal the source description (no `scale`, `color` or `bit_depth` step recorded; chapter 9 §9.2.4). |
| Tolerance | PSNR: FFmpeg prints `%0.2f`, so the normaliser rounds OVT's double to 2 decimals and requires equality, and additionally checks OVT against the normaliser's own float64 recomputation from the Y4M planes to 1e-6 dB; infinite PSNR (`inf`) must match. VMAF via FFmpeg: \|Δ\| ≤ 1e-6 on `vmaf` per frame (both call the same libvmaf; the filter must be pinned to `model=version=vmaf_v0.6.1:feature=…:n_threads=4` with no `n_subsample`, and inputs must have equal frame rate and count because the filter pairs frames by pts). This is a cross-check only; §14.4.11 is the authoritative VMAF parity. |
| Corpora | `netflix` yuv pairs (wrapped as Y4M by the fetcher's `post` step with `ffmpeg -s 576x324 -pix_fmt yuv420p -i x.yuv x.y4m`), `syn_*` decoded pairs (`src_*.y4m` vs decoded recon). |
| Golden file | `metrics/<id>.ffpsnr.json`, `.ffvmaf.json` |

### 14.4.11 `vmaf` CLI JSON — bit-exact VMAF parity (phase 3 exit criterion)

| Item | Value |
|---|---|
| Validates | the libvmaf host (`core/src/metrics/vmaf/vmaf_host.cpp`, chapter 9 §9.5): `vmaf_v0.6.1`, `vmaf_v0.6.1neg`, `vmaf_4k_v0.6.1`, `vmaf_b_v0.6.3` (bootstrap CI) and every further model bundled by the pinned libvmaf (ids generated at build time, chapter 9 §9.5.3), features `psnr`, `float_ssim`, `float_ms_ssim`, `psnr_hvs`, `ciede`, `cambi`; the four pooled values (`min`, `max`, `mean`, `harmonic_mean`); `n_subsample`. |
| Obtain | libvmaf **≥ 3.0.0**, exact tag pinned in `third_party/VERSIONS.md` (D5): `git clone https://github.com/Netflix/vmaf -b <pinned tag> && cd libvmaf && meson setup build --buildtype release -Denable_float=true -Denable_cuda=false -Denable_avx512=true && ninja -C build` → `build/tools/vmaf`. `-Denable_float=true` is required for the `float_ssim`/`float_ms_ssim` features; the fixed-point VIF/ADM/motion paths remain the default regardless (chapter 9 §9.5.1). BSD-2-Clause-Patent. OVT links the **same** source tree with the same meson options, so parity is a build-consistency test as much as a correctness test. |
| Command | `vmaf --reference <ref>.y4m --distorted <dist>.y4m --model version=vmaf_v0.6.1 --feature psnr --feature float_ssim --feature float_ms_ssim --feature psnr_hvs --feature ciede --feature cambi --threads 4 --json --output <id>.vmaf.json` ; variants: `--model version=vmaf_v0.6.1neg`, `--model version=vmaf_4k_v0.6.1`, `--model version=vmaf_b_v0.6.3`, every further `version=` the pinned release lists in `resource/doc/models.md`, and `--subsample 5`. The CLI has no pooling option: `pooled_metrics` always carries `min/max/mean/harmonic_mean` and all four are compared. Y4M input carries width/height/pix-fmt/bit depth, so no `-w/-h/-p/-b` flags are given (they are only for raw `.yuv` input, where all four are required). |
| Oracle output | libvmaf JSON: `{"version":..., "fps":..., "frames":[{"frameNum":0,"metrics":{"integer_adm2":..,"vmaf":..,"psnr_y":..}}], "pooled_metrics":{"vmaf":{"min":..,"max":..,"mean":..,"harmonic_mean":..}}}`. The normaliser drops `fps` and `version`-independent keys. |
| OVT side | `ovt metrics -r <ref>.y4m -d <dist>.y4m -m vmaf --model version=vmaf_v0.6.1 --opt vmaf.features='["psnr","float_ssim","float_ms_ssim","psnr_hvs","ciede","cambi"]' --threads 4 --align none --format vmaf-json -o <id>.ovt.vmaf.json` (chapter 12 §12.2.9 grammar; `--format vmaf-json` is the libvmaf-compatible exporter of chapter 9 §9.9.2; `--subsample 5` for the subsample variant). |
| Tolerance | **bit-exact**: the JSON documents must be identical after the normaliser removes `fps` and reformats numbers with `%.6f` as libvmaf does (libvmaf writes 6 decimals; OVT's exporter MUST write the same precision). Every per-frame feature value and every pooled value must match. |
| Corpora | `netflix` (`src01_hrc00_576x324` vs `src01_hrc01_576x324` and the other pairs used in `python/test/`), `syn_metrics_1080p_pair`, and two 10-bit pairs decoded to Y4M from `syn_h265_main10_pq_hdr` (the Y4M header carries `C420p10`, so neither tool needs a bit-depth flag). |
| Golden file | `metrics/<id>.<model>.vmaf.json` |

### 14.4.12 `x264_ssim` — vendored x264 SSIM kernel oracle

| Item | Value |
|---|---|
| Validates | metric id `ssim_x264`, defined in chapter 9 §9.4.4 as **exactly** x264's `x264_pixel_ssim_wxh` (4×4 sums, 8×8 windows at stride 4, `ssim_end1/ssim_end4`, integer constants). `tiny_ssim` (libvpx) is a different kernel and is not an oracle (D10). |
| Obtain | `tests/oracles/x264_ssim/` — the unmodified C functions `ssim_4x4x2_core`, `ssim_end1`, `ssim_end4`, `x264_pixel_ssim_wxh` copied from `common/pixel.c` of the pinned x264 tag (recorded in `tests/oracles/ORACLE_VERSIONS.md`; GPL-2.0-or-later, built only in the oracle image, never linked into OVT) plus a 60-line `main.c` that reads two Y4M files (8-bit and 10-bit 4:2:0; `BIT_DEPTH` compile-time variants as in x264) and prints per frame `n:<i> Y:<ssim> U:<ssim> V:<ssim>` with `%.9f`. Built by `tests/oracles/x264_ssim/CMakeLists.txt`. |
| Command | `x264_ssim <ref>.y4m <dist>.y4m > <id>.x264ssim.txt`; `normalize/x264_ssim.py` → `scores`. Secondary sanity check: `x264 --ssim --psnr --qp 0 … 2>&1 \| grep 'SSIM Mean Y'` on the same pair must equal the per-plane mean of the kernel oracle to 1e-4 (x264 prints 7 decimals of a float). |
| OVT side | `ovt metrics -r <ref>.y4m -d <dist>.y4m -m ssim_x264 --align none --threads 1 --format json -o <id>.ovt.json` |
| Tolerance | \|Δ\| ≤ 1e-6 per plane per frame (same integer sums; the only float operations are the final divisions, chapter 9 §9.4.4). |
| Corpora | `netflix` pairs (8-bit), `syn_metrics_1080p_pair`, the 10-bit `syn_h265_main10_pq_hdr` pair. |
| Golden file | `metrics/<id>.x264ssim.json` |

### 14.4.13 Daala `dump_psnrhvs` / `dump_ciede2000` and FAU-LMS `bjontegaard`

| Item | Value |
|---|---|
| Validates | `psnr_hvs` and `ciede` via libvmaf (second independent reference to the one in §14.4.11 — Daala is the origin of both algorithms) and `core/src/metrics/bd/bjontegaard.cpp` (cubic, PCHIP, Akima; chapter 9 §9.8.4). |
| Obtain | `git clone https://gitlab.xiph.org/xiph/daala && cd daala && ./autogen.sh && ./configure --disable-player && make tools` → `tools/dump_psnrhvs`, `tools/dump_ciede2000` (BSD-2). `pip install -r tests/oracles/requirements.txt` which pins the FAU-LMS `bjontegaard` package to the version recorded at pin time with `pip index versions bjontegaard` (BSD-3; no version number in this chapter is authoritative). |
| Commands | `dump_psnrhvs -y <ref>.y4m <dist>.y4m > <id>.psnrhvs.txt` (per-frame `PSNR-HVS-Y`), `dump_ciede2000 <ref>.y4m <dist>.y4m > <id>.ciede.txt`; `python -m tests.oracles.normalize.bd_reference rd_points.json > <id>.bd.json` where the script calls `bjontegaard.bd_rate(r_a, q_a, r_b, q_b, method='cubic'\|'pchip'\|'akima')` and `bd_psnr` for the committed RD point sets in `tests/golden/metrics/rd_points/*.json` (4-point and 6-point sets, overlapping and partially overlapping). |
| OVT side | `ovt metrics -r … -d … -m psnr_hvs,ciede --align none --format json` and `ovt bdrate --anchor a.csv --test b.csv --interpolator cubic\|pchip\|akima --metric <id> --range overlap --format json` (chapter 12 §12.2.10). |
| Tolerance | PSNR-HVS \|Δ\| ≤ 1e-3 dB (Daala's tool uses float; libvmaf's port differs in accumulation); CIEDE2000 \|Δ\| ≤ 1e-3; BD-rate \|Δ\| ≤ 1e-6 % and BD-PSNR \|Δ\| ≤ 1e-6 dB versus the Python package (PCHIP mode is documented to reproduce the JVET sheet to ≥ 10 decimals; OVT must match the package, not the sheet). `cubic` is the Bjøntegaard third-order polynomial fit and is defined only for **exactly 4 points per curve**; the 6-point sets are tested with PCHIP and Akima only and `ovt bdrate --interpolator cubic` on them must fail with `VQ-BD-0003` exactly as the package raises. |
| Golden file | `metrics/<id>.psnrhvs.json`, `metrics/<id>.ciede.json`, `metrics/bd/<set>.bd.json` |

### 14.4.14 google/uvq reference CLI — UVQ parity

| Item | Value |
|---|---|
| Validates | the UVQ host (ONNX Runtime, chapter 9 §9.6) end to end: frame sampling, resize, normalisation, the sub-networks, aggregation. |
| Obtain | `git clone https://github.com/google/uvq` at the commit pinned in `tests/oracles/ORACLE_VERSIONS.md`; `pip install -r uvq/requirements.txt` (**TensorFlow** CPU — the published UVQ models are TF SavedModels, D11); download the model directories per its README into `uvq/models/`. Apache-2.0. The oracle runs on CPU only, deterministic (`TF_DETERMINISTIC_OPS=1`, `tf.config.threading.set_inter_op_parallelism_threads(1)`, `OMP_NUM_THREADS=1`). The same SavedModels are the input of the tf2onnx export whose SHA-256 are pinned for OVT (chapter 9 §9.6). |
| Command | `python uvq_main.py --input_files="<clip>,<length_s>,<clip>.mp4" --output_dir=out/ --model_dir=uvq/models --transcode=yes` (the reference CLI of the pinned commit; its argument names are re-checked at pin time) → `out/<clip>/<clip>_uvq.csv` (per-second chunk scores) and the aggregated `compression_content_distortion` MOS; the normaliser `normalize/uvq.py` → `scores`. Repeated for every model version the pinned repository ships. |
| OVT side | `ovt metrics -d <clip>.mp4 -m uvq --opt uvq.version="<ver>" --opt uvq.fps=1 --format json -o <id>.ovt.json` (no-reference: no `-r`; chapter 12 §12.2.9). The ONNX export procedure is chapter 9 §9.6; the exported model files' SHA-256 are pinned in `third_party/VERSIONS.md`. |
| Tolerance | per-chunk MOS \|Δ\| ≤ 0.01; sequence MOS \|Δ\| ≤ 0.01 (phase 3 exit criterion, §0.8); per-chunk compression-level / distortion-type argmax labels must match for ≥ 95 % of chunks. |
| Corpora | `ytugc` `fast` subset (3 clips) on PRs; 20 clips nightly. |
| Golden file | `metrics/<id>.uvq-<ver>.json` |

### 14.4.15 mpv / libplacebo rendered reference frames — HDR display parity

HDR output cannot be verified by a human in CI, but the **pixels OVT hands to the display** can be compared with what mpv (which uses the same libplacebo) produces for the same frame and the same tone-mapping parameters.

| Item | Value |
|---|---|
| Validates | `libovt-render` `VideoSurface` colour pipeline: YUV→RGB, PQ/HLG EOTF, tone mapping (`BT2390`, `Spline`, `Clip`), gamut mapping, dithering off, both for an SDR target (tone-mapped) and an HDR target (PQ pass-through). Also `VideoSurface::measure()` MaxCLL/MaxFALL of a frame against `ffmpeg`'s `signalstats`-free reference: the per-frame max/avg luminance computed by `tests/oracles/normalize/nits_reference.py` from the decoded 10-bit planes with the ST 2084 EOTF in float64. |
| Obtain | mpv ≥ 0.39 with libplacebo ≥ 7 (same major as the `third_party/VERSIONS.md` pin, D5), built or `apt install mpv` in the oracle image (versions printed with `mpv --version`). Vulkan via `lavapipe` in CI (software; `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`). |
| Capture (SDR target) | `tests/oracles/mpv_capture.py --mode sdr --frame <n> <in> out/<id>_f<n>_sdr.png` runs `mpv --no-config --vo=gpu-next --gpu-api=vulkan --hwdec=no --target-trc=bt.1886 --target-prim=bt.709 --target-peak=203 --tone-mapping=bt.2390 --gamut-mapping-mode=perceptual --dither=no --scale=bilinear --cscale=bilinear --dscale=bilinear --vd-lavc-film-grain=cpu --pause --start=<seconds> --hr-seek=yes --hr-seek-framedrop=no --input-ipc-server=/tmp/mpv-<pid>.sock <in>` where `<seconds>` = `n / fps` computed in Python with 6 decimals (`--start` accepts `[[hh:]mm:]ss[.ms]`, not a fraction; `--hr-seek=yes` makes the seek frame-exact), then verifies `estimated-frame-number == n` over IPC (falling back to `frame-step` commands until it matches), waits for `playback-restart`, sends the IPC command `{"command":["screenshot-to-file","out/<id>_f<n>_sdr.png","video"]}` (mode `video` = rendered frame without OSD, at source resolution) then `{"command":["quit"]}`. |
| Capture (HDR target) | same script with `--mode hdr`, which replaces the target flags by `--target-trc=pq --target-prim=bt.2020 --target-peak=1000 --tone-mapping=clip --screenshot-high-bit-depth=yes --screenshot-png-compression=0` → 16-bit PNG of PQ-encoded RGB. |
| OVT side | `ovt render <in> --frame <n> --target sdr-bt1886-203 --tone-map bt2390 --gamut perceptual --dither off --scale bilinear --out out/<id>_f<n>_sdr.png` and `--target pq-bt2020-1000 --tone-map clip --out ..._hdr.png` (an offscreen `VideoSurface` with `DisplayTarget::OffscreenTexture`; chapter 12 §12.2.18 defines the subcommand, GPL `cli/`, phase 4). In phase 1 (WI-1.575) the suite does not use the CLI: `render/tests/test_render_parity.cpp` renders offscreen and exports PNG through WI-1.412's `export.h`; the `ovt render` form is the phase-4 convenience path. |
| Compare | `tools/imgdiff.py --mode hdr-parity a.png b.png`: converts both to linear light (BT.1886 with 203 nits for SDR; ST 2084 EOTF for the 16-bit PNG), computes per-pixel ΔE ITP (BT.2124) and reports `p99` and `max`. Pass: `p99 ≤ 1.0 ΔE_ITP` and `mean ≤ 0.3` for SDR; `p99 ≤ 0.5` for the HDR pass-through (there the only legitimate difference is rounding). Tone-mapping curves have version-dependent constants, so the test pins libplacebo and fails if `mpv --version` reports a different libplacebo major. |
| `measure()` parity | `nits_reference.py <in> --frame n` prints `{"max_cll_nits": .., "fall_nits": ..}` from float64 EOTF of every pixel (MaxCLL = max over pixels of max(R,G,B) in nits after YUV→RGB in BT.2020; FALL = mean of max(R,G,B)). OVT's `Measurements` must be within ±1 % of MaxCLL and ±1 % of FALL. |
| Corpora | `hdr` entries, `syn_h265_main10_pq_hdr`, `syn_av1_filmgrain_10bit` (both tools decode with FFmpeg/libdav1d and let the decoder apply film grain on the CPU: mpv `--vd-lavc-film-grain=cpu`, OVT `--film-grain cpu`; the HW path is excluded from parity tests), `cta` HLG track. |
| Golden file | `render/<id>_f<n>_sdr.png`, `render/<id>_f<n>_hdr.png` (PNG, ≤ 8 MiB each, stored via Git LFS in the repo; regenerated only when libplacebo is bumped). |

---
## 14.5 Decoded-picture correctness

### 14.5.1 Rule

Every **software** decode path OVT exposes (playback software fallback via libavcodec, and every analysis plugin's reconstruction plane `PlaneStage::Reconstruction`) MUST be **bit-exact** with the codec's reference decoder on the conformance corpora. Hardware paths (VideoToolbox, MediaCodec, VA-API) are **not** required to be bit-exact (10-bit HEVC hardware decoders usually are, and the test records bit-exactness when it occurs); they are checked with a PSNR floor (≥ 50 dB luma vs the software decode for 8-bit, ≥ 56 dB for 10-bit) and a "HW vs SW diff" view exists for users (chapter 10).

### 14.5.2 Reference per codec

| Codec | Reference decoder(s) and command | Reference hash source |
|---|---|---|
| H.264 | JM `ldecod -p InputFile=<in> -p OutputFile=ref.yuv` (oracle); FFmpeg `ffmpeg -i <in> -f md5 -` as fast second reference | `jvt` zips ship `.yuv` or `*_yuv.md5`; the fetcher's `post` step computes `sha256`/`md5` of the shipped YUV once |
| HEVC | HM `TAppDecoder -b <in> -o ref.yuv --SEIDecodedPictureHash=1` (oracle); libde265 `dec265 -c -q <in>` (`-c` checks the SEI hash); FFmpeg `-f md5` | `jctvc` zips ship `*_yuv.md5` (MD5 of the whole 8/16-bit little-endian YUV file) |
| AV1 | libaom `aomdec --rawvideo --md5 <in>.ivf` (whole-stream MD5, identical to the shipped `.md5`); dav1d `dav1d -i <in>.ivf -o frames.md5 --muxer md5` (per-frame MD5 list, used to localise the first bad frame); both references must agree with each other, otherwise the test reports a corpus/toolchain problem rather than an OVT failure | `aom` test data ships `<name>.md5` (libaom convention, see `test/test_vector_test.cc`) |

### 14.5.3 OVT's `--md5` and `ovt verify`

Chapter 12 §12.2.6 defines `ovt dump <in> --md5 [--stage recon --plugin <id> | --sw] [--bitdepth 8|16]` (`ovt decode` is an accepted alias). In `text` format the output is one `<md5>  <decode_index>` line per frame (`dav1d --muxer md5` layout) followed by the sequence MD5 as one 32-hex line (`aomdec --md5` layout); the test normalises both to a JSON list. Rules for the hashed bytes: planar Y then U then V, 8-bit samples as 1 byte, > 8-bit as 2-byte little-endian, **cropped** output (conformance window / render size applied) for playback, **coded size** for `PlaneStage::Reconstruction` of analysis plugins (a `--coded-size` flag selects the latter for the hash too). AV1 film grain: reference hashes are computed with grain **applied** (libaom's test vectors), so `ovt dump --md5` applies grain by default and `--no-film-grain` is available (chapter 12 §12.2.6).

Test `test_decode_md5_corpus` (`[slow][needs-corpus][golden]`): for every manifest entry with `"tests": [... "md5" ...]` run OVT's playback software path and each applicable analysis plugin, compare the whole-stream MD5 to the reference; on mismatch print the first differing frame (per-frame mode) and the plane/position of the first differing sample.

### 14.5.4 HEVC decoded picture hash SEI

`core/src/syntax/h265_sei.cpp` parses `decoded_picture_hash` (payloadType 132, H.265 D.2.20 / D.3.20: `hash_type` 0 = MD5, 1 = CRC, 2 = checksum, per colour component). `ovt verify <in> --hash-sei` (alias `--picture-hash`; phase 4; chapter 12 §12.2.12) recomputes the hash for every decoded picture with the method of H.265 D.3.20 (MD5 over the picture's samples in raster order with the specified byte layout; CRC-16 CCITT polynomial 0x1021 per the spec's bit order; checksum per D.3.20 eq. D-6) and reports per picture `match/mismatch/absent`. Tests: all `jctvc` streams with hash SEI → 100 % `match`; the same with one byte flipped in a slice payload (`tests/golden/h265/corrupt/*.bit`, generated by `tools/corrupt.py --byte-xor <offset>`) → mismatch on exactly the affected picture and all pictures referencing it, and a `Diagnostic` with code `DEC-H265-0001`.

### 14.5.5 Cross-decoder equality tests

- `test_decode_sw_equals_plugin_recon`: FFmpeg software decode == `PlaneStage::Reconstruction` of the analysis plugin for the same frame (libde265, libaom, ffmpeg plugin), per sample, for the `fast` corpus. Bit-exact.
- `test_decode_threads_invariance`: plugin output with `threads=1` == `threads=4` (planes and FlatBuffer bytes).
- `test_decode_hw_vs_sw` (`[needs-display]` on macOS; `[needs-android]`): PSNR floor as in §14.5.1; also asserts identical `coded_size`, `crop`, `chroma_format`, `bit_depth` metadata.

---

## 14.6 FrameAnalysis validator and cross-plugin consistency

### 14.6.1 Validator

Chapter 06 §6.9 is the **only** place where the validation rules `V-001`…`V-032` are stated (D13); this chapter does not restate them. `core/src/model/validator.cpp` (chapter 06 WI-0.086; header `core/include/ovt/model/validator.h`) implements them as `ovt::model::validate_frame_analysis(const fb::FrameAnalysis&, const fb::StreamIndex*, ValidationReport&)` and `validate_stream_index(...)`; the functions are part of the LGPL core (used by the `--validate` flag of `ovt analyze`/`ovt index`, chapter 12) and are called by **every** test that produces a `FrameAnalysis` through `ovt::test::analysis_frame(...)`, which fails the test on any `Error`-severity violation. The minimum rule set the validator MUST implement for this chapter's tests (ids are chapter 06's; the text here is a summary):

The rules exercised by every `FrameAnalysis`/`StreamIndex` test are `V-001` … `V-032` exactly as chapter 06 §6.9 lists them (ids only here, per D1/D13; `V-030` concerns `MetricsResult`, `V-031` `StreamIndex`, `V-032` canonical re-serialisation).

Any rule chapter 06 adds is picked up automatically because the tests iterate the validator's own rule table (`ovt::model::Validator`, chapter 06 WI-0.086) rather than a list of their own.

`core/tests/model/test_model_validator.cpp` (chapter 06 WI-0.086) contains one negative test per rule (a hand-built buffer violating exactly that rule → exactly that rule id reported); `core/tests/analysis/test_analysis_validator.cpp` only checks that every plugin's golden output passes with zero violations.

### 14.6.2 Cross-plugin consistency

| Test | Plugins / oracle | What must agree | Tolerance |
|---|---|---|---|
| `test_xplugin_hevc_de265_vs_hm` | `ovt-plugin-de265` vs `ovt-plugin-hm` (phase 4) and vs `hm_cu_dump` JSON (phase 2) | `CellGrid`: `pred, qp, intra_mode_y, intra_mode_c, mv0/1, ref0/1, tx_log2_w/h, cbf, partition_id`; `Block` tree shape (kinds, rects, `partition_type`, `level`); `refs` (POC of each slot); `filter_params` (SAO per CTU, deblocking offsets) | exact |
| `test_xplugin_av1_aom_vs_inspect` | `ovt-plugin-aom` vs libaom `inspect` JSON (§14.4.5) | every grid field, `symbols`, `bits_x8` per block, `frame.type/shown/show_existing_of`, tile `partitions` | exact |
| `test_xplugin_h264_ffmpeg_vs_jm` | `ovt-plugin-ffmpeg` vs JM trace (§14.4.3) | phase 2: `qp`, MVs (≥ 8×8), `pred` (intra/inter/skip from `mb_type` classes); phase 4: all `AvcMb` fields | exact |
| `test_xplugin_recon_equal` | every plugin vs FFmpeg software decode | `PlaneStage::Reconstruction` | bit-exact (§14.5.5) |
| `test_xplugin_index_consistency` | `StreamIndex` vs plugin output | `FrameInfo` in `FrameAnalysis.frame` equals the index's `FrameInfo` for that `decode_index` (type, poc, shown, refs declared) | exact, except `dpb_after` which the plugin may fill when the index could not |
| `test_xplugin_yuview_csv` | `ovt analyze --format yuview` (chapter 12 §12.1.4; `yuview-csv` is an accepted alias) vs YUView's own CSV for the same stream (committed from YUView 2.x `File → Export statistics` on `syn_h265_amp_sao_on` using its libde265 decoder) | block rectangles and values for the statistic types both tools have (`PredMode`, `IntraDir`, `MV0/1`, `TU depth`); the OVT file also passes `tools/yuview_csv_lint.py` (WI-2.584) | exact (YUView CSV values are integers) |

---

## 14.7 Fuzzing

### 14.7.1 Targets

All targets live in `core/tests/fuzz/fuzz_<name>.cpp` (backbone §0.4) and build the executables `ovt_fuzz_<name>` with `ovt_add_fuzz_target` (chapter 13 §13.2.1; placed in `<build>/fuzz/`, e.g. `build/linux-x64-fuzz/fuzz/ovt_fuzz_bitreader`); they are built when `OVT_ENABLE_FUZZ=ON` (clang only, `-fsanitize=fuzzer,address,undefined`), and link `libovt-core` statically. Each target is ≤ 60 lines: it wraps the input in a `BitReader`/`RandomAccessFile` memory view, calls the module entry point, and discards the result. A target MUST NOT crash, hang (> 10 s per input with `-timeout=10`), leak, or exceed 2 GiB RSS (`-rss_limit_mb=2048`).

| Target (source stem `fuzz_<name>`; executable `ovt_fuzz_<name>`) | Entry point exercised | Seeds (`core/tests/fuzz/corpus/<name>/`, e.g. `corpus/bitreader/`) | Dictionary |
|---|---|---|---|
| `fuzz_bitreader` | `ovt::BitReader` all read functions driven by the first bytes as an opcode tape (`u(n)`, `ue`, `se`, `leb128`, `ns`, `su`, `skip`, `byte_align`, `more_rbsp_data`) | 8 random tapes | — |
| `fuzz_syntax_h264_nal` | `ovt::syntax::h264::parse_nal(bytes)` (dispatches on `nal_unit_type` to SPS/PPS/SEI/slice header/AUD/SPS-ext/prefix/subset-SPS) | every distinct NAL type from the `fast` `jvt` corpus, one unit each (extracted by `tools/extract_units.py`) | `h264.dict` (start codes, NAL header bytes) |
| `fuzz_syntax_h264_sps`, `fuzz_syntax_h264_pps`, `fuzz_syntax_h264_slice_header`, `fuzz_syntax_h264_sei` | one parser each with a fixed, valid SPS/PPS context where needed (the slice-header target prepends a canned SPS+PPS; the fuzzer mutates only the slice bytes) | per type | — |
| `fuzz_syntax_h265_nal`, `fuzz_syntax_h265_vps`, `fuzz_syntax_h265_sps`, `fuzz_syntax_h265_pps`, `fuzz_syntax_h265_slice_header`, `fuzz_syntax_h265_sei` | HEVC equivalents (SEI includes decoded picture hash, MDCV, CLL, HDR10+ T.35, alternative transfer characteristics) | `jctvc` units | `h265.dict` |
| `fuzz_syntax_av1_obu`, `fuzz_syntax_av1_sequence_header`, `fuzz_syntax_av1_frame_header`, `fuzz_syntax_av1_tile_group_header`, `fuzz_syntax_av1_metadata` | AV1 parsers; the frame-header target prepends a canned sequence header and keeps a reference-frame state | `aom` units | `av1.dict` |
| `fuzz_container_isobmff` | `ovt::container::isobmff::Reader::open(memory)` + full tree expansion + all derived views (sample table, edit list, fragments, CENC) | 60 small MP4s (≤ 64 KiB) from `mpegff`/`gpac`/`bento4` truncated by `tools/mp4_shrink.py` (drops `mdat` payload, keeps structure) | `isobmff.dict` (all MP4RA fourccs) |
| `fuzz_container_ivf` | IVF reader | 4 tiny IVFs | — |
| `fuzz_container_annexb` | Annex B start-code splitter + EPB removal | 4 | — |
| `fuzz_container_obu` | OBU splitter (Section 5 and Annex B modes, both tried) | 4 | — |
| `fuzz_container_y4m` | Y4M header + frame parser | 4 | `y4m.dict` (`YUV4MPEG2`, `C420`, `FRAME`) |
| `fuzz_index_build` | `StreamIndex` builder over an in-memory Annex B / OBU stream (end-to-end: splitter → syntax → index) | 12 `syn_*` streams truncated to ≤ 256 KiB | — |
| `fuzz_import_yuview_csv` | YUView statistics CSV importer (chapter 06 export format read back) | 6 CSVs | `yuview.dict` |
| `fuzz_metrics_options_json` | `ovt::metrics::parse_options(json)` and `NormalizationConfig::from_json` | 10 JSON docs | `json.dict` |
| `fuzz_cache_blob` | on-disk cache record reader (zstd frame + FlatBuffer verify) | 6 blobs | — |
| `fuzz_plugin_push_<aom\|de265\|ffmpeg>` (`plugins/*/tests/fuzz/`) | `push()` of each analysis plugin with a canned valid parameter-set prefix; asserts `OVT_E_CORRUPT` or `OVT_OK`, never a crash — third-party decoder crashes are reported upstream and the input is kept as a regression with the test marked `[xfail-upstream]` | 10 `syn_*` streams | — |

### 14.7.2 Corpus seeding

`tools/fuzz_seed.py --target <t>` regenerates seeds from the local corpus cache: extracts units per type (`--max-per-type 3`, `--max-bytes 65536`), minimises with `build/linux-x64-fuzz/fuzz/ovt_fuzz_<t> -merge=1 corpus_new core/tests/fuzz/corpus/<t>`, and commits only the merged set (≤ 2 MiB per target) under `core/tests/fuzz/corpus/<t>/`. Argon streams (nightly) are the richest seed source for AV1 and are merged nightly into a **non-committed** corpus stored in the CI cache (`fuzz-corpus-<target>`), which the smoke job restores.

### 14.7.3 CI smoke

The `fuzz-smoke` job of chapter 13 §13.7.1 on every PR: build the `linux-x64-fuzz` preset, then for each target `build/linux-x64-fuzz/fuzz/ovt_fuzz_<t> -max_total_time=60 -max_len=1048576 -timeout=10 -rss_limit_mb=2048 -print_final_stats=1 core/tests/fuzz/corpus/<t> corpus_cache/<t>`; any crash/timeout/OOM uploads the artefact and fails the job. Total wall time is bounded by running targets in 4 parallel shards. Regression inputs from fixed bugs are committed as `core/tests/fuzz/regressions/<target>/<sha1>.bin` and replayed by `test_fuzz_regressions` (`[fast][fuzz-regress]`) on every platform, including non-clang builds.

### 14.7.4 OSS-Fuzz onboarding

1. Phase 1 exit: open a PR to https://github.com/google/oss-fuzz adding `projects/ovt/` with `project.yaml` (language c++, sanitizers address/undefined/memory, fuzzing engines libfuzzer/afl/honggfuzz, `main_repo` URL, primary contact), `Dockerfile` (clones OVT, installs CMake ≥ 3.25, Ninja, no Qt), and `build.sh` (`cmake -S . -B build-oss-fuzz -G Ninja -DOVT_ENABLE_FUZZ=ON -DOVT_FUZZ_ENGINE="$LIB_FUZZING_ENGINE" -DOVT_BUILD_GUI=OFF -DOVT_BUILD_CLI=OFF -DOVT_BUILD_RENDER=OFF && cmake --build build-oss-fuzz && cp build-oss-fuzz/fuzz/ovt_fuzz_* $OUT/ && cp core/tests/fuzz/dictionaries/*.dict $OUT/ && for t in core/tests/fuzz/corpus/*; do zip -j $OUT/ovt_fuzz_$(basename $t)_seed_corpus.zip $t/*; done`).
2. There is **no** `oss-fuzz` CMake preset (chapter 13 §13.2.4 owns presets, D23): `build.sh` configures directly and takes `CC/CXX/CFLAGS/CXXFLAGS` from the OSS-Fuzz environment; `OVT_FUZZ_ENGINE` (default `-fsanitize=fuzzer`, the cache variable read by `ovt_add_fuzz_target`, chapter 13 §13.2.1) is set to `$LIB_FUZZING_ENGINE` so AFL and honggfuzz builds link their own driver.
3. Bug reports go to the two maintainers listed in `project.yaml`; the 90-day disclosure deadline is tracked in the issue tracker label `oss-fuzz`.

### 14.7.5 Crash triage procedure

1. Reproduce: `build/linux-x64-fuzz/fuzz/ovt_fuzz_<t> <crash-file>` under the same sanitizer; record the stack in the issue.
2. Minimise: `build/linux-x64-fuzz/fuzz/ovt_fuzz_<t> -minimize_crash=1 -runs=100000 <crash-file>`; commit the minimised input to `core/tests/fuzz/regressions/<target>/`.
3. Classify: (a) OVT bug → fix, add regression, verify the `BitReader` invariant that should have caught it (§0.7.1); (b) third-party decoder bug → file upstream with the minimised input, pin the finding in `docs/security/upstream-findings.md`, mark `[xfail-upstream]`; (c) resource exhaustion by design (e.g. a 2^32-entry `stsz`) → add a limit with a `Diagnostic` (code `CNT-ISOBMFF-00xx`) and document the limit in chapter 08.
4. Severity: memory-safety findings in `core/` are **release blockers**; a release branch may not be tagged with an open ASan/UBSan finding.

---

## 14.8 Sanitizers and static analysis

### 14.8.1 Sanitizer CI jobs

| Preset | Flags | Runs | Scope |
|---|---|---|---|
| `linux-x64-asan-ubsan` | compile flags of chapter 13 §13.2.5 (`OVT_SANITIZER=asan-ubsan`); runtime `ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1:detect_stack_use_after_return=1`, `UBSAN_OPTIONS=print_stacktrace=1` | every PR | `ctest --preset sanitizers` (`fast` + `golden` labels), all fuzz regressions |
| `linux-x64-tsan` | chapter 13 §13.2.5 `OVT_SANITIZER=tsan`; runtime `TSAN_OPTIONS=halt_on_error=1 suppressions=tests/tsan.supp` | every PR touching `core/src/jobs`, `core/src/analysis`, `core/src/cache`, `render/`, `app/src`; nightly for all | `jobs`, `analysis` host (plugin on worker thread + cancellation), cache concurrent read/write tests, `VideoSurface` offscreen tests |
| `macos-arm64-asan` | clang (Xcode), chapter 13 §13.2.5 `OVT_SANITIZER=asan-ubsan`; test preset `sanitizers-macos` | nightly | same as asan-ubsan |
| (MSan) | not a project preset: MemorySanitizer runs only inside OSS-Fuzz (requires instrumented libc++; §14.7.4) | — | fuzz targets |

Third-party code (FFmpeg, libaom, libde265) is built **with** ASan in these presets (`third_party/*.cmake` propagate `CMAKE_C_FLAGS`); known upstream findings are suppressed in `tests/asan.supp`/`tests/ubsan.supp` with a link to the upstream issue, never silently.

### 14.8.2 clang-tidy

`.clang-tidy` at repo root; CI runs `run-clang-tidy -p build -quiet` on changed files (PR) and the whole tree (nightly, warnings-as-errors). Enabled check groups: `bugprone-*`, `cert-*` (except `cert-err58-cpp`), `clang-analyzer-*`, `concurrency-*`, `cppcoreguidelines-pro-bounds-array-to-pointer-decay`, `cppcoreguidelines-pro-bounds-pointer-arithmetic` (**error** in `core/src/syntax`, `core/src/container`, `core/src/bitreader` — enforces §0.7.1 "no raw pointer arithmetic on input data"), `cppcoreguidelines-narrowing-conversions`, `cppcoreguidelines-init-variables`, `misc-*` (except `misc-non-private-member-variables-in-classes`), `modernize-*` (except `modernize-use-trailing-return-type`), `performance-*`, `portability-*`, `readability-*` (except `readability-magic-numbers`, `readability-identifier-length`). Disabled globally: `bugprone-easily-swappable-parameters`. Header filter: `^(core|render|plugins|cli|app)/`.

### 14.8.3 `-Werror` warning set

The warning set, the `-Werror` switch (`OVT_WERROR`) and the suppression rule are defined once, in chapter 13 §13.2.5 (the `ovt::flags` interface target that every OVT target links; D24). This chapter adds no flags. For testing purposes: every CI preset builds with `OVT_WERROR=ON`; a PR that adds a warning fails `build-linux-x64`/`build-macos-arm64`; WI-0.559 only wires clang-tidy and cppcheck and adds the CI check that no target bypasses `ovt::flags`.

### 14.8.4 cppcheck (optional)

`cppcheck --enable=warning,performance,portability --std=c++20 --inline-suppr --project=build/compile_commands.json --suppressions-list=tests/cppcheck.supp` runs nightly, non-blocking, with results posted as a workflow artefact. New findings are triaged weekly; it becomes blocking only if the owner decides so (open question, chapter 17).

---
## 14.9 Performance tests

### 14.9.1 Harness

Google Benchmark binaries in `tests/perf/` (one per area), each registering benchmarks with `->Unit(benchmark::kMillisecond)->MinTime(2.0)->Repetitions(5)->ReportAggregatesOnly(true)`. They are driven by `tools/perf_run.py`, which: detects the hardware id (§14.9.2), runs each binary with `--benchmark_format=json --benchmark_out=results/<hardware-id>/<bench>.json`, and calls `tools/perf_compare.py` to compare the `median` of each benchmark against `tests/perf/baselines/<hardware-id>.json` and against the absolute budgets table (§14.9.3). Inputs come from the corpus cache (`[needs-corpus]`); the 1-hour 1080p H.264 file is `syn_h264_1h_1080p` — §14.3.3 recipe 27 (108 000 frames, recipe-26 encoder settings), generated once per machine by `generate.py --only syn_h264_1h_1080p`, never downloaded, cached locally and hashed (`local_only`).

### 14.9.2 Reference hardware

| Hardware id | Machine | Role | Runner |
|---|---|---|---|
| `m1-air-8gb` | Apple MacBook Air (M1, 2020), 8 GB, macOS 15, built-in display (SDR) | **normative** reference for §0.7.4 budgets | self-hosted, nightly |
| `linux-x64-ref` | Linux x86_64: 8-core AMD Ryzen 7 5800X or Intel i7-12700 class, 32 GB, Ubuntu 24.04, Mesa + lavapipe (CPU benchmarks) or a discrete GPU for overlay tests | secondary; same budgets ×1.0 for CPU, overlay budget ×1.0 on a discrete GPU | self-hosted, nightly |
| `pixel-tablet` | Google Pixel Tablet (Tensor G2, 8 GB, **SDR panel**), Android 14+ | **normative Android device** (D12): Android budgets are the explicit `pixel-tablet` values of §14.9.3 (analysis ≤ 5 s per 60-frame 1080p GOP, D19; overlay p95 ≤ 33 ms at 1080p instead of 4K, D5); SDR control device for HDR fallback behaviour; no HDR acceptance | device farm / lab device, nightly |
| `android-hdr` | an HDR-capable Android tablet with `Display.getMode().getSupportedHdrTypes()` ∋ HDR10 and `getHdrSdrRatio() > 1` (model recorded in `docs/hardware.md`; the `chromebook-hdr` row may stand in when none is available) | Android HDR acceptance tests of chapters 10/11 (`[needs-android-hdr]`), §14.10.4 step 5, §14.10.5 checklist | lab device, weekly |
| `chromebook-hdr` | HDR-capable Chromebook (e.g. Acer Chromebook Spin 714 (Intel Core i5-1335U) or any ChromeOS device with an HDR-capable panel listed in `docs/hardware.md`), Android runtime (ARCVM) | Android-x86_64 budgets: the `pixel-tablet` values of §14.9.3 apply unchanged; HDR manual checklist (§14.10.5); ADR-014 ChromeOS HDR spike | lab device, weekly |

This table is the single source for reference hardware (chapter 10 §10.9.4 and chapter 11 §11.9 refer to it; D12). The Pixel Tablet is SDR: every HDR-on-Android acceptance test names `android-hdr` (or `chromebook-hdr`), never `pixel-tablet`.

A result file records `hardware_id`, OS version, compiler, commit, and `sysctl`/`/proc/cpuinfo` summary; comparisons are only made between identical `hardware_id`s.

### 14.9.3 Measured quantities and budgets (pass/fail)

Budgets are §0.7.4 values on `m1-air-8gb`; `linux-x64-ref` uses the same numbers; the Android devices use the explicit `pixel-tablet` column of the Android rows below (no multipliers). A benchmark **fails** if its median exceeds the budget, or regresses by more than 20 % against the stored baseline median (§14.9.4). **This table (and its machine form `tests/perf/budgets.json`) is the single source of every performance budget and benchmark name in the specification (D5/D14):** chapters 9, 10 and 11 list their benchmarks by these names and must not state other numbers. Benchmarks live in `tests/perf/<area>/` and are named `bench_<area>_<what>`.

| Benchmark | Measures | Input | Budget (M1) |
|---|---|---|---|
| `bench_index_h264_1h_1080p` | `ovt_stream_build_index` wall time, header-only pass | `syn_h264_1h_1080p.mp4` | ≤ 2 000 ms |
| `bench_index_h265_10min_1080p`, `bench_index_av1_10min_1080p` | same | `syn_h265_10min_1080p.mp4`, `syn_av1_10min_1080p.mp4` (generated) | ≤ 400 ms each |
| `bench_first_frame_<codec>_<container>` | time from `ovt_file_open` to the first `ovt_Picture` returned by the playback path (HW preferred, SW fallback measured separately) | `fast` corpus MP4s | ≤ 500 ms (HW), ≤ 800 ms (SW) |
| `bench_analysis_de265_gop60_1080p` | `ovt_analysis_frame` for the last frame of a 60-frame GOP from the preceding RAP (cold: no cache) | `syn_h265_amp_sao_on` re-encoded at 1080p/GOP 60 (`syn_h265_gop60_1080p`) | ≤ 1 500 ms |
| `bench_analysis_aom_gop60_1080p` | same with `ovt-plugin-aom` (inspection + accounting) | `syn_av1_gop60_1080p` | ≤ 3 000 ms |
| `bench_analysis_ffmpeg_gop60_1080p` | same with `ovt-plugin-ffmpeg` | `syn_h264_gop60_1080p` | ≤ 1 000 ms |
| `bench_analysis_cached_frame` | `ovt_analysis_frame` for a frame present in the disk cache | any | ≤ 30 ms |
| `bench_overlay_4k_mv_arrows` | `VideoSurface::render()` GPU+CPU frame time with grid + 4×4 MV arrows + QP heatmap on a 3840×2160 frame, offscreen target, 100 frames, reports p95 | `FrameAnalysis` of a 4K AV1 frame with all cells inter (synthetic, `tests/perf/data/fa_4k_dense.bin`) | p95 ≤ 16 ms |
| `bench_overlay_build` | `OverlaySet` construction from a 4K `FrameAnalysis` (CPU) | same | ≤ 8 ms |
| `bench_render_video_4k` | `VideoSurface::render()` of a 4K 10-bit PQ frame to an SDR offscreen target (tone map BT.2390), no overlays, p95 of 100 frames | `real_4k_hevc_frame.fb` planes (chapter 10 §10.9.3) | p95 ≤ 8 ms (M1), Android ≤ 16 ms at 1080p |
| `bench_render_first_frame` | `VideoSurface` creation + first `render()` (shader compile from the cache, LUT build) | `real_1080p_hevc_frame.fb` | ≤ 150 ms warm cache, ≤ 1 500 ms cold |
| `bench_metrics_psnr_ssim_1080p` | frames per second of PSNR+SSIM (Gaussian 11×11) combined on 1080p 8-bit 4:2:0, 1 thread and 4 threads | `syn_metrics_1080p_pair` (300 frames) | ≥ 200 fps (4 threads) |
| `bench_metrics_psnr_1080p` | PSNR alone, 1 thread (sanity check of the SIMD kernel) | same | ≥ 600 fps |
| `bench_metrics_vmaf_1080p` | VMAF v0.6.1 fps, 4 threads, fixed-point | same | ≥ 60 fps |
| `bench_metrics_normalize_1080p_pq` | normalisation stage fps (zimg PQ→linear, 10-bit → float) | `syn_h265_main10_pq_hdr` decoded | ≥ 120 fps |
| `bench_metrics_align` | alignment search (chapter 9 §9.3.1) over ±60 frames of 1080p signatures | `syn_align_drop7` vs its source | ≤ 500 ms |
| `bench_metrics_maps` | per-block map generation + store for PSNR/SSIM maps at 1080p | `syn_metrics_1080p_pair` | ≤ 1.5 ms/frame overhead |
| `bench_metrics_uvq_chunk` | UVQ inference of one 1-second chunk (ONNX Runtime, CPU EP, 4 threads) | `ytugc_clip01` | ≤ 1 500 ms (M1); ≤ 4 500 ms `pixel-tablet` |
| `bench_metrics_memory_4k` | peak RSS of a PSNR+SSIM+VMAF job on a 4K 10-bit pair with maps enabled | `syn_av1_4k_2min` first 60 frames vs source | ≤ 2 GiB desktop, ≤ 1 GiB Android |
| `bench_gui_open_first_frame` | `ovt-studio` open → first picture on screen (test IPC `wait-idle` timing) | `fast` corpus MP4s | ≤ 500 ms (NFR-002) |
| `bench_gui_cold_start` | process start → interactive empty window | — | ≤ 1 500 ms M1, ≤ 3 000 ms `pixel-tablet` (NFR-008) |
| `bench_gui_scrub` | 300 timeline scrub steps with F3 overlays, UI frame time p95 (`OVT_GUI_TRACE=1`) | `syn_h265_gop60_1080p` | p95 ≤ 16 ms desktop, ≤ 33 ms `pixel-tablet` (chapter 11 §11.9) |
| `bench_container_open_moov_1h` | `ovt_container_open` + tree of a 1-hour MP4 (≈ 108 000 samples) | `syn_h264_1h_1080p.mp4` | ≤ 150 ms |
| `bench_container_sample_table_1h` | `SampleTable` expansion (`ovt_container_sample_table`) of the 1-hour MP4 | `syn_h264_1h_1080p.mp4` | ≤ 200 ms |
| `bench_container_fragments_3600` | `FragmentTimeline` build over 3 600 `moof`s | `syn_mp4_frag_3600` | ≤ 500 ms |
| `bench_demux_packets_1h` | packets per second delivered by the libavformat `Demuxer` (chapter 8 WI-1.230, `av_read_frame` over the custom `AVIOContext`) in file order, no decode | `syn_h264_1h_1080p.mp4` | ≥ 50 000 packets/s |
| `bench_memory_4k_stream` | peak RSS (via `getrusage`/`mach_task_basic_info`) after opening a 4K stream, indexing, analysing 64 frames and viewing 4 planes | `syn_av1_4k_2min` (generated) | ≤ 2 GiB desktop, ≤ 1 GiB Android (§0.7.3) |
| `bench_validator` | `validate_frame_analysis` with all V-rules on a 4K synthetic `FrameAnalysis` (960×540 cells, ≈ 130 k blocks; `tools/gen_dense_fa.py --size 4k`) | `tests/perf/data/fa_4k_dense.bin` | ≤ 200 ms on `m1-air-8gb` (chapter 6 WI-0.086) |
| `bench_cache_write` | write of 100 × 1080p `FrameAnalysis` into the analysis cache with zstd level 3, uncompressed throughput | `tests/perf/data/fa_1080p_dense.bin` × 100 | ≥ 200 MB/s on `m1-air-8gb` (chapter 6 WI-0.091) |

Android rows (`pixel-tablet`; `chromebook-hdr` uses the same values):

| Benchmark | Budget (`pixel-tablet`) |
|---|---|
| `bench_analysis_de265_gop60_1080p`, `bench_analysis_aom_gop60_1080p`, `bench_analysis_ffmpeg_gop60_1080p` | ≤ 5 000 ms per 60-frame 1080p GOP (D19) |
| `bench_first_frame_<codec>_<container>` | ≤ 1 500 ms (HW), ≤ 2 400 ms (SW) |
| `bench_index_h265_10min_1080p`, `bench_index_av1_10min_1080p` | ≤ 1 200 ms each |
| `bench_overlay_4k_mv_arrows` (run with the 1080p input `tests/perf/data/fa_1080p_dense.bin`, `tools/gen_dense_fa.py --size 1080p`) | p95 ≤ 33 ms |
| `bench_render_video_4k` (1080p input) | p95 ≤ 16 ms |
| `bench_metrics_psnr_ssim_1080p` | ≥ 60 fps (4 threads) |
| `bench_metrics_vmaf_1080p` | ≥ 15 fps |
| `bench_metrics_uvq_chunk` | ≤ 4 500 ms |
| `bench_metrics_memory_4k`, `bench_memory_4k_stream` | ≤ 1 GiB |
| `bench_gui_cold_start` | ≤ 3 000 ms |
| `bench_gui_scrub` | p95 ≤ 33 ms |

Benchmarks not listed in the Android table are not run on Android. Cache cap on Android: 2 GiB (chapter 11 §11.5.6). `tests/perf/budgets.json` carries one object per benchmark with the keys `m1-air-8gb` and `pixel-tablet`.

### 14.9.4 Regression policy

- Baselines live in `tests/perf/baselines/<hardware-id>.json` and are updated by a maintainer with `tools/perf_compare.py --accept results/<hardware-id>` in a dedicated PR titled `perf: update baseline <hardware-id>` that explains the change.
- Nightly: `perf_compare.py` fails if any benchmark median is > 20 % slower than baseline **or** exceeds its absolute budget; a budget failure opens an issue labelled `perf-budget`, a regression failure labelled `perf-regression` with the commit range since the last green run.
- PR-level: `bench_index_*`, `bench_overlay_build` and `bench_metrics_psnr_ssim_1080p` run on the Linux GitHub runner as a smoke (`--benchmark_min_time=0.5s`) with a **50 %** threshold against a runner-specific baseline (`gh-ubuntu-22.04.json`; chapter 13 pins `ubuntu-22.04` runners), because hosted runners are noisy; only nightly results are authoritative.
- Noise control: 5 repetitions, median; machines are idle (no other jobs), on mains power, display asleep (except overlay tests), CPU frequency scaling left default (documented in the result file).

---

## 14.10 GUI tests

### 14.10.1 Qt Quick Test unit tests for models

`app/tests/qml/tst_*.qml`, run by `ovt_studio_qmltests` (`QUICK_TEST_MAIN_WITH_SETUP(ovt_studio, Setup)`; `Setup` registers the C++ models with fixture data from `tests/golden`). `QT_QPA_PLATFORM=offscreen`, every PR, Linux and macOS. Minimum coverage:

| Test file | Model / component | Assertions |
|---|---|---|
| `tst_ContainerTreeModel.qml` | `ContainerTreeModel` (QAbstractItemModel over `ContainerTree`) | row counts per level, `fourcc`/`offset`/`size` roles, lazy expansion emits `rowsInserted`, `selectedSpan` property updates on `setCurrentIndex` |
| `tst_SyntaxTreeModel.qml` | `SyntaxTreeModel` | names/descriptors/values, `bitSpan` role, `findByBitOffset(off)` returns the deepest node containing `off`, diagnostics role |
| `tst_HexViewModel.qml` | `HexViewModel` | windowed byte access for a 2 GiB mock file, `highlight(ByteSpan, BitSpan)` → row/column ranges, EPB positions rendered as inserted bytes |
| `tst_TimelineModel.qml` | `TimelineModel` | frame sizes, types, RAP flags, display↔decode order mapping, `visibleRange` with 100 000 frames stays O(visible) |
| `tst_FrameAnalysisModel.qml` | `SelectionInfoModel`, `OverlayToggleModel` | `blockAt(x,y)` returns the leaf block and its `ext` fields as a key/value list; overlay toggles emit `overlaySetChanged` once per change |
| `tst_MetricsModel.qml` | `MetricsSeriesModel`, `PooledModel` | per-frame series, pooled rows, CSV export string equals golden |
| `tst_HdrMetadataModel.qml` | `HdrMetadataModel` | box-level vs SEI-level values side by side, mismatch flag |
| `tst_Shortcuts.qml` | keyboard map (chapter 11) | every documented shortcut triggers its action exactly once |

### 14.10.2 Screenshot tests

`ovt_studio_screenshots` (C++ test using `QQuickView::grabWindow()` offscreen): opens fixture files, applies a scripted state, grabs PNGs at **96 DPI** (`QT_SCALE_FACTOR=1`, `QT_FONT_DPI=96`), fixed window 1600×1000, both `light` and `dark` themes, with the bundled font (`app/resources/fonts/Inter-Variable.ttf`; system fonts are never used in tests). Reference images in `app/tests/screenshots/<platform>/` (Git LFS). Comparison with `tools/imgdiff.py --mode ui`: pass if SSIM (grey, 7×7 window) ≥ 0.995 **and** no 8×8 tile has mean |Δ| > 12/255 (ignores sub-pixel anti-aliasing noise, catches layout breaks). Failures upload a side-by-side diff image. Platforms: Linux (lavapipe) on PRs; macOS nightly. The picture area is rendered by libplacebo and is included (it is deterministic with dithering off).

Scenes: `container_tree_selected`, `syntax_tree_hex_selected`, `timeline_1000_frames`, `picture_overlay_partitions_h265`, `picture_overlay_mv_av1`, `planes_y_u_v`, `metrics_panel_vmaf`, `hdr_metadata_panel`, `selection_info_block`, each × 2 themes.

### 14.10.3 Scripted interaction tests

`ovt-studio` exposes a **test IPC** (enabled only with `--test-ipc <socket>`, GPL app code): JSON-lines over a local socket with commands `open <path>`, `click <objectName> [row]`, `select-syntax-node <path>`, `key <sequence>`, `get <objectName>.<property>`, `screenshot <path>`, `wait-idle`. `app/tests/scripts/*.py` (pytest, `[needs-display]` on macOS or `offscreen` on Linux) drive it. Required scripts:

1. `test_open_click_field_hex.py`: open `jctvc_AMP_A_Samsung_7`; wait-idle; `select-syntax-node "units[1]/seq_parameter_set_rbsp/pic_width_in_luma_samples"`; `get hexView.highlightedBitSpan` == the `BitSpan` from the golden `trace_headers.json`; `get hexView.firstVisibleOffset` ≤ span start ≤ last visible.
2. `test_timeline_scrub.py`: open `syn_av1_hidden_altref_showexisting.ivf`; `click timeline 37`; `get picture.displayIndex == 37`; `get picture.decodeIndex` equals the index golden; `get timeline.hiddenFrameCount > 0`.
3. `test_block_click_selection_info.py`: open `syn_h265_amp_sao_on.265`; enable partitions overlay; `click picture 200 120`; `get selectionInfo.fields` contains every `HevcCu` field name of chapter 06 with values equal to `ovt analyze --format json` for that block.
4. `test_metrics_run.py`: open ref and dist; run PSNR+SSIM; `get metricsPanel.pooled.psnr.mean` equals CLI output within 1e-9.
5. `test_hdr_panel.py`: open `syn_h265_main10_pq_hdr.mp4`; `get hdrPanel.maxCll == 1000`, `get hdrPanel.mdcvFromBox == hdrPanel.mdcvFromSei`.

### 14.10.4 Android instrumentation smoke

`android/app/src/androidTest/java/.../SmokeTest.kt` (UI Automator), run nightly on the x86_64 emulator (API 36 `google_apis` image, GPU swiftshader; the image of chapter 13 §13.7.1 `android-emulator-smoke`) and on the `pixel-tablet` device with `./gradlew -p android :app:connectedFossDebugAndroidTest`:

1. install the debug APK (`./gradlew -p android :app:installFossDebug`), launch `MainActivity` with an intent extra `OVT_OPEN_URI` pointing at `syn_h264_multislice_cavlc.mp4` pushed to the app's cache dir (`adb push`), 
2. wait ≤ 10 s for `objectName == "pictureSurface"` to report `frameReady == true` (exposed through the accessibility tree as a content description),
3. take a screenshot (`UiDevice.takeScreenshot`) and assert the picture region is not uniform (≥ 100 distinct colours) and the container tree lists `moov`,
4. open the syntax tree, tap the first SPS node, assert the hex view shows a highlight (content description `hexHighlight=<offset>`),
5. on the `android-hdr` device only (§14.9.2; skipped on `pixel-tablet`, whose panel is SDR and must instead report the tone-mapped fallback diagnosis), open `syn_h265_main10_pq_hdr.mp4` and assert that `adb logcat -d -s OVT_HDR` contains the line `dataspace=BT2020_PQ` (tag `OVT_HDR`, message `dataspace=<BT2020_PQ|BT2020_HLG|SRGB>`, emitted by `ovt_jni_surface_attach` of chapter 13 WI-0.052 whenever the dataspace is set); on `pixel-tablet` and the emulator the same step asserts `dataspace=SRGB` (the SDR fallback).

### 14.10.5 Manual HDR checklist (cannot run in CI)

Executed per release candidate and per phase exit on each HDR platform; results recorded in `docs/acceptance/hdr-<platform>-<date>.md` with photos of the display taken with fixed camera exposure next to a reference device if available.

| # | Step | Expected | macOS (XDR / external HDR) | Android (`android-hdr` device of §14.9.2; the Pixel Tablet is SDR and is not used here) | ChromeOS |
|---|---|---|---|---|---|
| 1 | Open `syn_h265_main10_pq_hdr.mp4`; HDR output on | OS reports HDR mode (macOS: "HDR" badge in Display settings / `ovt` log `EDR headroom > 1`; Android: logcat `OVT_HDR dataspace=BT2020_PQ`; ChromeOS: same via ARCVM) | ☐ | ☐ | ☐ |
| 2 | Show the 1000-nit white patch region of the PQ test pattern (`ovt` built-in pattern, chapter 10) | visibly brighter than the UI white (203 nits); no clipping banding in the 100–1000 nit ramp | ☐ | ☐ | ☐ |
| 3 | Toggle `ToneMap::Clip` vs `BT2390` | with Clip, > peak regions flatten; with BT2390, highlights roll off smoothly | ☐ | ☐ | ☐ |
| 4 | Switch output to SDR (menu) | tone-mapped image matches the `mpv` reference screenshot (§14.4.15) by eye; no hue shift in skin tones | ☐ | ☐ | ☐ |
| 5 | Open `syn_hlg_1080p` (§14.3.3 recipe 38) | HLG rendered with system gamma for the display's peak; the "nits" readout under the cursor changes with the display peak setting | ☐ | ☐ | ☐ |
| 6 | Measurements panel: MaxCLL/MaxFALL of frame 0 | equals `nits_reference.py` within 1 % | ☐ | ☐ | ☐ |
| 7 | False colour overlay | thresholds at 100/203/1000 nits line up with the pattern's labelled steps | ☐ | ☐ | ☐ |
| 8 | Window moved between SDR and HDR displays (macOS), or window resized/docked (ChromeOS) | output mode follows the display; no black frame > 1 s; no crash | ☐ | n/a | ☐ |
| 9 | Compare A/B PQ vs SDR-graded versions side by side | both halves correctly mapped; the split line has no colour discontinuity in the shared SDR range | ☐ | ☐ | ☐ |
| 10 | Film grain on/off on `syn_av1_filmgrain_10bit` | grain visible in HDR without clipping; `--no-film-grain` removes it | ☐ | ☐ | ☐ |

---

## 14.11 Acceptance test procedure per phase

Each procedure is executed by a human (or an agent with device access) at phase end on the reference machines (§14.9.2) and recorded in `docs/acceptance/phase-<n>.md` with command outputs. All steps must pass; a failed step reopens the responsible WI.

### Phase 0 — Foundations

1. `git clone <repo> && cd ovt && cmake --preset macos-arm64-release && cmake --build --preset macos-arm64-release && ctest --preset pr` → all green, 0 skipped except `needs-corpus` when offline.
2. Same with `linux-x64-release` and `linux-x64-asan-ubsan`.
3. `python3 tools/fetch_corpora.py --tags fast` → exit 0; `--verify-only` → exit 0; re-run with one manifest hash altered → exit 2 and the file is re-downloaded.
4. `tools/license_gate.py` → exit 0; add a fake `AGPL-3.0` entry to `vcpkg.json` → exit 1 naming it.
5. `cmake --preset android-arm64-release && cmake --build --preset android-arm64-release && ./gradlew -p android :app:assembleFossDebug` → APKs; install on the Pixel Tablet and the HDR Chromebook; the app shows the PQ test pattern through `SurfaceView`; `adb logcat -d | grep OVT_HDR` prints `OVT_HDR dataspace=BT2020_PQ` on the Chromebook (format of chapter 13 WI-0.052), or `dataspace=SRGB` plus the documented fallback diagnosis in `docs/platforms/chromeos-hdr.md` if ChromeOS pass-through is unavailable — this is the spike result the owner needs.
6. `ctest --preset pr -L fuzz-regress` green; `build/linux-x64-fuzz/fuzz/ovt_fuzz_bitreader -max_total_time=60` green (after `cmake --preset linux-x64-fuzz && cmake --build --preset linux-x64-fuzz`).
7. FlatBuffers schemas compile (`flatc --cpp --python --kotlin core/schema/*.fbs`) and `ctest -R test_model_roundtrip` (chapter 6 WI-0.093) passes.

### Phase 1 — See the stream

1. `ovt probe jctvc_… .mp4`-style: for every `fast` ISOBMFF corpus file, `ovt probe <f> --format json --boxes --expand all` matches the MP4Box/mp4ff/mediainfo/mp4box.js goldens (`ctest -L golden -R container`).
2. `ovt syntax <f> --format json` matches `trace_headers` and `gstparse` goldens for all three codecs (`ctest -R syntax_golden`).
3. `ovt index syn_av1_hidden_altref_showexisting.ivf --format json` lists hidden frames with `shown=false` and ≥ 1 frame with `show_existing_of` set; the number of index frames without `show_existing_of` equals the number of frame objects in the `inspect` golden (inspect emits no object for `show_existing_frame = 1`, §14.4.5), and the number of shown frames equals the frame count of `aomdec --rawvideo -o /dev/null --summary`.
4. GUI: open `cta` HEVC HDR10 CMAF track; click `moov/trak/mdia/minf/stbl/stsd/hvc1/hvcC` → hex view highlights the box bytes; expand `hvcC` → click `general_profile_idc` → hex highlights the exact bits (compare with `trace_headers` offsets); click an SPS node in the syntax tree → hex highlights; scrub the timeline → picture updates within 500 ms (measured by the `--test-ipc` `wait-idle` timing).
5. Y/U/V plane views show the three planes of a 4:2:0 10-bit frame with correct dimensions (U/V half size).
6. HDR: on an XDR MacBook, PQ content is displayed in HDR (checklist §14.10.5 steps 1–4); on an SDR display the tone-mapped output matches the mpv reference (§14.4.15 passes in CI). HLG: open `syn_hlg_1080p` on the XDR MacBook and with `--force-sdr`; the HDR panel shows `transfer = 18 (HLG)` with no MDCV/CLLI rows and checklist step 5 passes (chapter 16 §16.1 row 16).
7. First frame ≤ 500 ms and index ≤ 2 s budgets pass on `m1-air-8gb` (`tools/perf_run.py --only bench_index_h264_1h_1080p,bench_first_frame_*,bench_gui_open_first_frame,bench_render_first_frame`).
8. Android: steps 4–5 on the Pixel Tablet via `content://` from the Files app; the Chromebook runs the same with mouse hover showing tooltips.

### Phase 2 — Look inside blocks

1. `ctest -L golden -R "analysis|xplugin"` green: libaom vs `inspect`, libde265 vs HM grid, FFmpeg plugin vs JM trace and `ffmpeg_mvs`.
2. GUI, for each codec sample (`syn_h264_weightedpred_ltr`, `syn_h265_amp_sao_on`, `syn_av1_tiles_cdef_lr`): enable partitions, prediction modes, MVs, QP map, bits heatmap, ref-index map in turn; each toggle redraws within one frame (`bench_overlay_4k_mv_arrows` p95 ≤ 16 ms at 4K on M1; visually smooth at 60 fps while panning).
3. Click a block → Selection Info lists every extension-table field with the spec name; the values equal `ovt analyze --format json` for that block id.
4. Per-stage planes: switch to Prediction / Residual / Pre-deblock for the selected frame; `Reconstruction` equals the playback frame bit-exactly (`ovt decode --md5` of both agree).
5. GOP/reference graph shows `declared_refs` edges; for the AV1 sample, hidden ALTREF appears as an unshown node.
6. `ovt analyze <f> --format yuview` passes `tools/yuview_csv_lint.py` and loads in YUView 2.x and overlays identically (manual, screenshot recorded).
7. Analysis budgets pass (`bench_analysis_*`).
8. Android: steps 2–3 on the Pixel Tablet at 1080p; analysis of a 60-frame 1080p GOP completes in ≤ 5 s (`bench_analysis_*_gop60_1080p`, §14.9.3 Android rows, D19).

### Phase 3 — Measure quality

1. `ctest -L golden -R metrics` green, including `vmaf_cli_parity` **bit-exact** on all `netflix` vectors and `uvq_parity` within 0.01 MOS.
2. `ovt metrics -r a.y4m -d b.y4m -m vmaf --align none --format vmaf-json -o ovt.json` output `diff`s empty (after `fps` removal) against `vmaf --json` for `src01_hrc00` vs `src01_hrc01` (recorded verbatim).
3. GUI: load ref/dist; the metrics panel shows per-frame VMAF/PSNR/SSIM curves, pooled table, heatmap overlay of per-block PSNR; A/B and diff views switch with one key; CSV/JSON export files equal the CLI's.
4. Misaligned inputs (`syn_align_drop7`: dist with 7 leading frames dropped): alignment reports `offset_frames == +7` (chapter 9 §9.3.1 sign: distorted frame i ↔ reference frame i+7) with `confidence ≥ 0.5` and `status == "ok"`; scores after alignment equal the run with `--align 7`.
5. 10-bit PQ pair: normalisation reports the conversion; PSNR uses peak 1023; VMAF output equals the recorded `vmaf` CLI run on the same 10-bit Y4M files bit-exactly (libvmaf reads the depth from the Y4M header).
6. `ovt bdrate` on the committed RD sets equals the `bjontegaard` package (all three interpolators).
7. Budgets: `bench_metrics_psnr_ssim_1080p ≥ 200 fps`, `bench_metrics_vmaf_1080p ≥ 60 fps` on M1.
8. Android: PSNR/SSIM/VMAF run on the Pixel Tablet on a 720p pair; integer/fixed-point results (PSNR MSE sums, `ssim_x264`, libvmaf fixed-point features and VMAF score) equal desktop results bit-exactly; float metrics (`ssim`, `ms_ssim`, float libvmaf features) agree within 1e-4 relative (chapter 9 §9.4.1 cross-architecture rule; WI-3.590).

### Phase 4 — Deepen & extend

1. H.264 full `AvcMb` fields match JM trace on the `jvt` fast subset (`test_xplugin_h264_ffmpeg_vs_jm` full mode).
2. `ovt-plugin-hm` equals `hm_cu_dump` on `jctvc` incl. RExt; `ovt verify --hash-sei` reports 100 % match on `jctvc` and localises the corrupted picture of `syn_broken_h265_hash_mismatch` (frame 17).
3. Residual/coefficient views: coefficients of a selected TU equal HM's `TraceDec.txt` values for that TU (manual spot check of 3 TUs, recorded).
4. HRD/buffer graph: for `jctvc_HRD_A_Fujitsu_3`, no underflow is reported (the stream is conformant); with an artificially halved `bit_rate_value_minus1` (patched copy) an underflow is flagged.
5. Matroska and TS trees: open an MKV and a TS built from `syn_*` with `mkvmerge`/`ffmpeg -f mpegts`; trees match `mediainfo --Details=1` element/packet offsets (the oracle both formats share) and, for MKV, `mkvinfo -v -v -z` element ids and sizes.
6. DASH/HLS virtual file: open `dashif` MPD and `apple` master playlist; init + first 3 segments appear as one stream; segment provenance shown per sample.
7. Python: `pip install ./python && python -c "import ovt; print(ovt.probe('x.mp4').tracks)"`; `python/tests` green on macOS and Linux.
8. Linux Wayland HDR: on a Wayland compositor that exposes the `wp_color_manager_v1` global of the `color-management-v1` protocol (wayland-protocols ≥ 1.41, staging; compositor and Mesa versions as listed in chapter 10 §10.4.2, the owner of those versions), PQ output verified by the checklist steps 1–4.
9. Sidecar metric: ColorVideoVDP via the sidecar protocol produces a score on a 720p pair; the protocol round-trip test passes.
10. HDR scopes: false colour, nits waveform and vectorscope render for the PQ clip; MaxCLL readout within 1 % of reference.
11. Feature-matrix check: every "must-have" row of chapter 03 for the three codecs is demonstrated and ticked in `docs/acceptance/phase-4.md`.

### Track A — Android/ChromeOS

Each phase's steps marked "Android" above plus: resizable window on ChromeOS keeps layout; keyboard shortcuts of chapter 11 work with an external keyboard; mouse hover shows block tooltips; `content://` open from Files and from a "Share to OVT" intent; F-Droid reproducible build produces an APK with identical `classes.dex`/native libraries hash to the release pipeline (`tools/repro_check.sh`, chapter 13).

---
## 14.12 Work items

All WIs follow the §0.1 template. Sizes: S ≤ 1 day, M ≤ 3 days, L ≤ 1 week. "Test support" paths are those of §14.2.2. Phase-0 WIs (WI-0.550…WI-0.569) build the infrastructure; the per-phase WIs (WI-1.570…WI-4.599) add golden suites as the modules they validate land.

### WI-0.550 — Set up the test framework skeleton
**Goal:** Catch2 v3, pytest and ctest labels are wired so that `ctest --preset pr` runs an empty-but-green suite on all CI platforms.
**Depends on:** WI-0.001 (top-level CMake and presets), WI-0.002 (vcpkg manifest).
**Produces:** `core/tests/CMakeLists.txt`, `core/tests/main.cpp`, `core/tests/test_support/{corpus,fb_normalize,golden}.{h,cpp}`, `tests/CMakeLists.txt`, `tools/requirements-test.txt`, the ctest label mapping used by the test presets `pr`, `nightly`, `sanitizers`, `perf` (the presets themselves are defined in `CMakePresets.json` by chapter 13 WI-0.001, D23), and the CTest test `tools_pytest` (labels `fast;tools`, registered by `tests/CMakeLists.txt` when `pytest` imports) that runs the `tools/` pytest suite inside the `ctest --preset pr` step of chapter 13 WI-0.030's build jobs. WI-0.550 adds no job to `.github/workflows/ci.yml`: chapter 13 §13.7.1 has no separate `tests` or `pytest` job (issue #11).
**Interface:** `ovt::test::corpus_path(std::string_view id) -> std::optional<std::filesystem::path>`; `ovt::test::compare_or_update(const std::filesystem::path& golden, std::span<const uint8_t> actual)`; `ovt::test::normalize_json(std::string_view) -> std::string` (sorted keys, 6-decimal floats).
**Steps:** 1. add `catch2`, `benchmark` to `vcpkg.json`; `find_package(Catch2 3 REQUIRED)`; `catch_discover_tests(... PROPERTIES LABELS ...)` mapping tags→labels. 2. write the golden runner's `main.cpp` (linked against `Catch2::Catch2`) with an `ovt_set_logger` hook that appends log lines to the Catch2 `INFO` context; every other test executable links `Catch2::Catch2WithMain` (§14.2.1 linkage rule). 3. implement `corpus_path` honouring `OVT_CORPORA_DIR` and `OVT_REQUIRE_CORPORA`. 4. implement `compare_or_update` with `OVT_UPDATE_GOLDEN`. 5. map tags to labels for the chapter 13 test presets (the `ctest --preset pr`/`pr-macos` step of the `build-*` jobs is chapter 13 WI-0.030's). 6. register the `tools/` pytest suite as the CTest test `tools_pytest` (`tools/requirements-test.txt` is installed by WI-0.030's setup action).
**Acceptance tests:** 1. Given a clean checkout, When `cmake --preset linux-x64-debug && cmake --build --preset linux-x64-debug && ctest --preset pr`, Then exit 0 and the JUnit file lists ≥ 1 test. 2. Given `OVT_REQUIRE_CORPORA=1` and no cache, When a `[needs-corpus]` test runs, Then it fails with message `corpus file missing: <id>`; without the variable it is reported as skipped. 3. Given `OVT_UPDATE_GOLDEN=1`, When a golden test runs, Then the golden file is rewritten and the test passes. 4. Given the build tree, When `nm -C` lists the test executables, Then exactly one (`ovt_core_tests`) defines its own `main` and every other links `Catch2WithMain`.
**Size:** M.
**Notes / pitfalls:** `catch_discover_tests` needs `POST_BUILD` discovery on macOS when the binary links dylibs from the build tree — set `DISCOVERY_MODE PRE_TEST`. Catch2 linkage (D24): ordinary test executables link `Catch2::Catch2WithMain`; only the golden runner `ovt_core_tests` links `Catch2::Catch2` and has its own `main` (logger hook, golden/corpus environment variables). **Overlap:** primary for the test framework; WI-0.004 is reduced to sanitizer flag plumbing. See chapter 16 §16.3.

### WI-0.551 — Implement `tools/fetch_corpora.py` and the manifest schema
**Goal:** corpus files are fetched, verified and cached per §14.2.4.
**Depends on:** none.
**Produces:** `tools/fetch_corpora.py`, `tests/corpora/manifest.json` (initial `fast` entries for `jvt`, `jctvc`, `aom`, `mpegff`, `gpac`, `bento4`, `netflix`), `tests/corpora/manifest.schema.json`, `tools/tests/test_fetch_corpora.py`, `tools/tests/fixture_server.py` (local HTTP fixture server: serves a directory over `http://127.0.0.1:<port>/`, supports `Range`, configurable corruption/latency, used as a pytest fixture here and by chapter 8/12 DASH/HLS acceptance tests; D27), `tests/serve_corpus.py` (CLI wrapper: `python3 tests/serve_corpus.py --root <dir> --port 0` prints the bound URL), `tools/tests/fixtures/fixture.json` (manifest of the served test archives).
**Interface:** CLI grammar of §14.2.4; exit codes 0/2/3/4; cache layout `<cache>/<id>/<file>` + `.ok`; `fixture_server.serve(root, *, corrupt=None, delay_ms=0) -> ContextManager[str]` (yields the base URL).
**Steps:** 1. JSON Schema and validator (`jsonschema` package). 2. downloader with retries, temp file, SHA-256 streaming, size check. 3. archive member extraction with path-traversal guard (zip, tar.gz, 7z via `py7zr` for some ITU zips that are actually 7z — check magic bytes). 4. `post` steps (`ffmpeg_trim_seconds`, `wrap_y4m`) executed with the pinned oracle `ffmpeg` if present, else skipped with a warning and the entry marked unavailable. 5. `--verify-only`, `--max-total-bytes`, `--jobs`. 6. `tools/tests/fixture_server.py` and pytest serving fixtures of each archive type and a corrupted one. 7. `aliases` support (§14.3.5): write `<cache>/alias/<name>` symlinks.
**Acceptance tests:** 1. Given the fixture server, When `fetch_corpora.py --manifest tools/tests/fixtures/fixture.json --cache-dir tmp`, Then all files are present with `.ok` and exit 0. 2. Given a wrong `sha256`, When run, Then exit 2 and no `.ok` file. 3. Given a zip member named `../evil`, When run, Then exit 4 with `unsafe member name`. 4. Given a complete cache and network disabled, When `--verify-only`, Then exit 0 in < 1 s per GiB (streaming hash).
**Size:** M.
**Notes / pitfalls:** ITU FTP paths are case-sensitive and some zips contain a nested directory; always use `extract.member` as a path within the archive. Record `Content-Length` mismatches as exit 3, not 2.

### WI-0.552 — Populate the full corpora manifest and the `fast` subset
**Goal:** every corpus of §14.3.1 has manifest entries with verified hashes; the `fast` subset ≤ 300 MiB.
**Depends on:** WI-0.551.
**Produces:** complete `tests/corpora/manifest.json`, `tests/corpora/LICENSES.md`, `tools/manifest_add.py` (helper that downloads a URL, fills `sha256`/`size_bytes`).
**Interface:** manifest fields of §14.2.4.
**Steps:** 1. script to enumerate ITU directory listings and AOM `test_vectors.cc` into candidate entries. 2. download each, fill hashes, write `features`/`tests`/`tags` per §14.3.2. 3. `mpegff` via Git LFS: entries use `"vcs": {"git": "...", "lfs_path": "data/file_features/..."}` instead of `url`; fetcher support added here. 4. `argon` entries with `manual_download=true` and the expected hashes (hashes supplied by the maintainer with an AOM account). 5. `ytugc`, `hdr`, `cta`, `dashif`, `apple` entries. 6. generate `LICENSES.md`.
**Acceptance tests:** 1. Given the manifest, When `fetch_corpora.py --tags fast`, Then total downloaded bytes ≤ 314 572 800 and exit 0. 2. Given the manifest, When `tools/license_gate.py --corpora`, Then exit 0. 3. Given §14.3.2's lists, When `tools/check_fast_subset.py`, Then every named stream has an entry (exit 0).
**Size:** L.
**Notes / pitfalls:** some JVT zips contain both `.264` and `.jsv`; pick `.264`/`.jvt` Annex B. HEVC `*_yuv.md5` files hash the entire YUV file, not per frame. AOM `.md5` is of the decoded frames in libaom's "md5 of all frames" mode.

### WI-0.553 — Synthetic stream generator
**Goal:** `tests/corpora/synthetic/generate.py` produces every recipe of §14.3.3 (1–76) reproducibly.
**Depends on:** WI-0.551, WI-0.554 (oracle image for pinned encoders).
**Produces:** `tests/corpora/synthetic/{generate.py,recipes.json,drm_cbcs.xml,drm_cenc.xml,drm_cbcs_constiv.xml,drm_cbcs_keyrot.xml,widevine_pssh.bin,hdr10plus_metadata.json,chapters.txt}`, `tests/corpora/{sei_payloads,corrupt_conditions,raw_formats}.json`, the `*_expected.json` sidecars of §14.3.3, `tools/av1_insert_hdr_obus.py`, `tools/gen_dense_fa.py`, `tools/gen_fixture_fb.py`, `tools/gen_heif_items.py`, `tools/ts_inject_errors.py`, the mutators `tools/{corrupt,make_broken,sei_inject,y4m_poke,av1_set_level,make_multi_pps,make_mkv_lacing}.py`, `tools/check_fast_subset.py`, manifest `syn_*` entries (recipes 1–76, aliases of §14.3.5 and the non-stream fixtures of §14.3.3) with hashes.
**Interface:** `generate.py [--only ID] [--out DIR] [--check] [--fixtures] [--local-only]`; exit 5 on encoder version mismatch; `check_fast_subset.py [--all-chapters]` exit 1 on an unknown fixture name.
**Steps:** 1. source generation (A/B/C and the 1080p/4K sources of recipes 25/28). 2. recipes as argv lists; version pin check. 3. MP4 wrapping step. 4. hashing + manifest update mode (`local_only` entries hashed on the perf runner). 5. non-stream fixtures (`--fixtures`). 6. pytest that runs 3 cheap recipes in the oracle image and checks hashes.
**Acceptance tests:** 1. Given the oracle image, When `generate.py --check`, Then every `syn_*` file hash equals the manifest and exit 0. 2. Given `x265 --version` differing from the pin, When run, Then exit 5 and no file written. 3. Given `syn_av1_hidden_altref_showexisting.ivf`, When `inspect -A` lists frames, Then ≥ 1 object has `showFrame: 0`, and the number of frames `aomdec` outputs exceeds the number of `inspect` objects with `showFrame: 1` (the difference is the `show_existing_frame` count; inspect emits nothing for those). 4. Given `syn_h264_epb_dense.264`, When `ovt syntax --hex` runs, Then it reports ≥ 20 `epb_positions` in the first three units; given `syn_align_drop7`, When `ovt metrics --align auto` runs, Then it reports `offset_frames == +7`. 5. Given `tools/check_fast_subset.py --all-chapters`, When run over `doc/`, Then exit 0 (every cited fixture is a registry id or a §14.3.5 alias).
**Size:** L.
**Notes / pitfalls:** `aomenc` needs `--lag-in-frames ≥ 1` for ALTREF; `--enable-fwd-kf=1` guarantees a `show_existing_frame` of a keyframe. x264 `--interlaced` yields MBAFF, not PAFF; PAFF needs JM `lencod` (`PicInterlace=1`). No synthetic recipe produces long-term reference pictures (H.264 or HEVC): LTR coverage is conformance streams only (§14.3.3 recipes 3 and 12).

### WI-0.554 — Oracle Docker image and build scripts
**Goal:** a reproducible image with every oracle of §14.4 at pinned versions.
**Depends on:** none.
**Produces:** `tests/oracles/Dockerfile` (third-party stage; the in-house oracle programs are added to its `oracles` stage by WI-0.562, WI-0.563, WI-0.565 and WI-1.575), `tests/oracles/ORACLE_VERSIONS.md`, `tests/oracles/build_{jm,hm,libaom_inspect,ffmpeg_oracle}.sh`, `tests/oracles/package.json`, `tests/oracles/requirements.txt`, `tests/oracles/install_tsduck.sh` (TSDuck `.deb` at the pinned release; D27), `.github/workflows/oracle-image.yml` (builds and pushes to GHCR on change).
**Interface:** image tag `ghcr.io/openvideotools/ovt-oracles:<yyyymmdd>`; every tool on `PATH`; `oracle-versions` command prints the versions table.
**Steps:** 1. base image + apt deps (`mediainfo`, `mkvtoolnix`, `libgstreamer-plugins-bad1.0-dev`, Go, Node). 2. FFmpeg source build with x264/x265 (`-DENABLE_HDR10_PLUS=ON`)/libaom/libvmaf; standalone `x264`, `x265`, `aomenc`, `dav1d`. 3. GPAC build; Bento4 (`mp4dump`, `mp4encrypt`, `mp4fragment`); `mp4ff-info`; `npm ci`; TSDuck via `install_tsduck.sh`. 4. JM with `TRACE 1`; HM stock + `ENC_DEC_TRACE` (`TAppEncoder`, `TAppDecoder`). 5. libaom `inspect`; `vmaf` CLI (`-Denable_float=true`); Daala tools; mpv; Python packages from `tests/oracles/requirements.txt` (`bjontegaard`, `siti-tools`, uvq requirements with TensorFlow CPU, `tf2onnx`). 6. `oracle-versions` script and smoke test running each tool on a `syn_*` stream.
**Acceptance tests:** 1. Given the image, When `docker run <img> oracle-versions`, Then every third-party tool of §14.2.6 (including `tsp --version`, `mkvinfo --version`, `mp4dump --version`) prints the pinned version. 2. Given the image and `syn_h265_amp_sao_on.265`, When each oracle command of §14.4 runs, Then exit 0 and non-empty output. 3. Given the Dockerfile unchanged, When rebuilt a week later, Then `oracle-versions` output is identical (all pins are tags/commits, no `latest`).
**Size:** L.
**Notes / pitfalls:** JM 19.0 needs `-fcommon` with modern GCC; HM needs `-DCMAKE_CXX_STANDARD=11` on some tags; the libaom inspect example is only built when `CONFIG_INSPECTION=1` **and** `ENABLE_EXAMPLES=1`.

### WI-0.555 — Oracle normalisers and canonical JSON families
**Goal:** every oracle's native output converts to the canonical JSON families of §14.4.0.
**Depends on:** WI-0.554.
**Produces:** `tests/oracles/normalize/{trace_headers,jm_trace,ffmpeg_mvs,hm_grid,aom_inspect,mp4box_diso,mp4box_dts,mp4ff_info,mediainfo_trace,mp4boxjs,ffmetrics,vmaf_json,x264_ssim,daala,bd_reference,uvq,nits_reference}.py`, `*_map.json` name maps, `tools/tests/test_normalizers.py` with captured oracle snippets as fixtures.
**Interface:** each script: `python -I normalize/<x>.py <oracle-output> [--codec ...] > canonical.json`; JSON Schemas for the six families in `tests/oracles/normalize/schema/`.
**Steps:** one afternoon per normaliser (6 steps, two normalisers each), each with fixture-based tests; 7. schema validation of every output.
**Acceptance tests:** 1. Given the fixture `trace_headers` snippet with an AV1 OBU, When normalised, Then `bit_offset` of `seq_profile` is 0 (header bytes subtracted). 2. Given a JM trace with skipped MBs, When normalised, Then `mb_type == "P_Skip"` entries exist for every MB address gap. 3. Given each normaliser output, When validated against its family schema, Then valid.
**Size:** L.
**Notes / pitfalls:** CBS prints `u(1)` flags as `0`/`1` and `ue(v)` with their value only; `bit_length` is the length of the bit string column. MediaInfo trace uses `offset` in bytes as decimal in XML but hex in text mode; always use `--Output=XML`.

### WI-0.556 — Golden test runner (C++ side) and `fb_normalize`
**Goal:** C++ code that turns OVT FlatBuffers/CLI JSON into the canonical families and compares with goldens.
**Depends on:** WI-0.550, WI-0.083 (ch. 6 schemas and generated headers).
**Produces:** `core/tests/test_support/fb_normalize.cpp` (`SyntaxTree → syntax-elements/header-struct`, `ContainerTree → box-tree`, `SampleTable → sample-table`, `FrameAnalysis → block-grid`, `MetricsResult → scores`), `tests/golden/CMakeLists.txt` (`ovt_add_golden_test(NAME ... CORPUS id ORACLE tool FAMILY f CMD ...)` CMake function), `tests/golden/README.md`.
**Interface:** `ovt::test::to_canonical(const fb::SyntaxTree&, Family) -> nlohmann::json` etc.; comparison semantics of §14.4.0 step 3 (oracle is lower bound).
**Steps:** 1. JSON library (`nlohmann_json` vcpkg) and family writers. 2. tolerant comparer with per-field tolerance map. 3. CMake function + label wiring. 4. first golden test (BitReader-independent: `box-tree` of a committed 2 KiB MP4) to prove the plumbing.
**Acceptance tests:** 1. Given a golden file with an extra oracle-only field, When compared, Then pass; Given an OVT-only missing field, Then fail naming the field path. 2. Given two numerically equal floats formatted differently, When compared with tolerance 0, Then pass (compare numbers, not strings).
**Size:** M.
**Notes / pitfalls:** normalise floats through the same `%.6f`/tolerance path as the Python normalisers (§14.4.0), otherwise C++ and Python goldens drift; never compare FlatBuffers byte-wise here (that is `ovt dump --diff`, chapter 12 §12.2.6).

### WI-0.557 — Fuzz harness, first targets and CI smoke
**Goal:** `OVT_ENABLE_FUZZ` builds libFuzzer targets; `fuzz_bitreader` and the splitter targets exist; smoke job runs 60 s per target.
**Overlap:** the splitter targets `fuzz_container_{annexb,obu,ivf,y4m}` move to WI-1.238 (they need the chapter 8 parsers); this WI delivers the harness, `fuzz_bitreader`, seeds tooling, regress mode and `fuzz-smoke.yml`; fuzz sources live under `core/tests/fuzz/` (backbone §0.4). See chapter 16 §16.3.
**Depends on:** WI-0.550, WI-1.102 (`BitReader`), WI-1.100 (`RandomAccessFile`).
**Produces:** `core/tests/fuzz/CMakeLists.txt` (`ovt_add_fuzz_target(<name> <source>)`, linking `${OVT_FUZZ_ENGINE}`, output directory `build/<preset>/fuzz/`; chapter 13 §13.2.1–§13.2.2), `core/tests/fuzz/fuzz_bitreader.cpp` (executable `ovt_fuzz_bitreader`), `core/tests/fuzz/corpus/bitreader/*`, `core/tests/fuzz/regressions/`, `core/tests/fuzz/test_fuzz_regressions.cpp`, `tools/fuzz_seed.py`, the `fuzz-smoke` job steps of chapter 13 §13.7.1. The `linux-x64-fuzz` preset is chapter 13's (WI-0.001); there is no `oss-fuzz` preset (§14.7.4).
**Interface:** `extern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t)`; `ovt_add_fuzz_target` (chapter 13 §13.2.1, WI-0.035 skeleton; final form here) builds `ovt_fuzz_<name>` into `<build>/fuzz/` and adds a ctest entry `fuzz_<name>_regress` (label `fuzz-regress`) that replays `regressions/<name>/*`.
**Steps:** 1. CMake function with `-fsanitize=fuzzer,address,undefined`; non-clang fallback builds a regress-only `main`. 2. `fuzz_bitreader` opcode tape. 3. (splitter targets: WI-1.238.) 4. seeds via `tools/fuzz_seed.py`. 5. workflow with 4 shards and artefact upload.
**Acceptance tests:** 1. Given the `linux-x64-fuzz` build, When `build/linux-x64-fuzz/fuzz/ovt_fuzz_bitreader -runs=100000 core/tests/fuzz/corpus/bitreader`, Then exit 0. 2. Given a planted bug (test branch reading one byte past the end), When the smoke runs, Then ASan reports it within 60 s and the job fails with the artefact attached. 3. Given a file in `regressions/fuzz_bitreader/`, When `ctest -R fuzz_bitreader_regress` on a GCC build, Then it is executed (regress mode compiles without libFuzzer).
**Size:** M.
**Notes / pitfalls:** `-fsanitize=fuzzer` is clang-only — the GCC preset must still compile the regress `main`; keep seeds small (< 4 KiB) so the 60 s smoke reaches the parsers; OSS-Fuzz integration is WI-0.569 (§14.7.4), not this WI.

### WI-0.558 — Sanitizer presets and suppressions
**Goal:** `linux-x64-asan-ubsan`, `linux-x64-tsan`, `macos-arm64-asan` presets exist and CI runs them per §14.8.1.
**Overlap:** this WI delivers only the suppression files, `tools/check_suppressions.py`, `docs/dev/sanitizers.md` and the propagation of sanitizer flags into third_party builds; the presets are WI-0.004 and the CI jobs are WI-0.031. See chapter 16 §16.3.
**Depends on:** WI-0.550, WI-0.004 (sanitizer flag plumbing), WI-0.010 (third_party scripts).
**Produces:** `tests/{asan,ubsan,tsan}.supp`, `tools/check_suppressions.py`, `docs/dev/sanitizers.md`, sanitizer flag propagation in `third_party/*.cmake`.
**Interface:** suppression file format of the respective sanitizer; `check_suppressions.py <file>...` exit 1 on an entry without an upstream URL; the sanitizer preset variables of chapter 13 §13.2.2 (owned by WI-0.004).
**Steps:** 1. suppression files for the flags of §14.8.1 and propagation into third_party builds (presets: WI-0.004; CI jobs: WI-0.031); propagate to third_party builds. 2. workflow; 3. documented procedure for adding a suppression (upstream issue link mandatory, checked by a regex in `tools/check_suppressions.py`).
**Acceptance tests:** 1. Given the asan preset, When `ctest --preset sanitizers`, Then exit 0. 2. Given a suppression line without a URL, When `check_suppressions.py`, Then exit 1. 3. Given a deliberate data race in a scratch test, When run under tsan, Then the job fails.
**Size:** S.
**Notes / pitfalls:** every suppression needs an upstream issue link; third_party builds must receive the same `-fsanitize` flags or ASan reports false positives at the ABI boundary.

### WI-0.559 — clang-tidy, `-Werror` set and cppcheck jobs
**Goal:** static analysis per §14.8.2–14.8.4 is enforced.
**Depends on:** WI-0.550.
**Produces:** `.clang-tidy`, `.github/workflows/static-analysis.yml`, `tests/cppcheck.supp`, `tools/check_ovt_flags.py` (fails if an OVT target does not link `ovt::flags`).
**Interface:** the warning set is chapter 13 §13.2.5's `ovt::flags` interface target (D24; not defined here); `.clang-tidy` check list of §14.8.2.
**Steps:** 1. `tools/check_ovt_flags.py` over the CMake file API reply: every target under `core/`, `render/`, `plugins/`, `cli/`, `app/` links `ovt::flags`. 2. `.clang-tidy` with the check list; per-directory override making pointer-arithmetic checks errors in parser dirs. 3. PR job on changed files (`clang-tidy-diff.py`); nightly whole-tree. 4. nightly cppcheck artefact.
**Acceptance tests:** 1. Given `uint8_t* p = data + 3;` added in `core/src/syntax/`, When the PR job runs, Then it fails with `cppcoreguidelines-pro-bounds-pointer-arithmetic`. 2. Given an implicit narrowing in `core/src/metrics`, When built with a CI preset (`OVT_WERROR=ON`), Then compilation fails with `-Wconversion` as an error. 3. Given a new target that does not link `ovt::flags`, When `tools/check_ovt_flags.py` runs, Then exit 1 naming the target.
**Size:** S.
**Notes / pitfalls:** apply the warning set to OVT targets only, never to third_party; the PR job must run on the changed-lines diff to stay under 10 minutes.

### WI-0.560 — Performance harness and reference-hardware runners
**Goal:** Google Benchmark binaries, `perf_run.py`, `perf_compare.py`, baselines directory, nightly workflow on self-hosted runners.
**Depends on:** WI-0.550, WI-0.552, WI-0.553 (corpora and the perf fixtures).
**Produces:** `tests/perf/CMakeLists.txt` (`ovt_add_benchmark(<area> <name>)` placing binaries under `tests/perf/<area>/`), `tests/perf/budgets.json`, `tests/perf/core/bench_smoke.cpp` (BitReader throughput, used to validate the harness), `tools/perf_run.py`, `tools/perf_compare.py`, `tools/check_budgets.py` (diffs the §14.9.3 tables against `budgets.json`), `tests/perf/baselines/README.md`, `.github/workflows/perf-nightly.yml`, `docs/dev/perf-runners.md` (how to register the M1 Air, Linux box, Pixel Tablet, Chromebook).
**Interface:** result JSON = Google Benchmark format + `ovt_meta{hardware_id, os, compiler, commit}`; `perf_compare.py results/ baselines/<hw>.json --budgets tests/perf/budgets.json --regression-pct 20`.
**Steps:** 1. hardware id detection (`sysctl hw.model`, `/sys/devices/virtual/dmi/id/product_name`, `getprop ro.product.model`). 2. runner + compare scripts with exit codes. 3. `budgets.json` from §14.9.3. 4. workflow with `runs-on: [self-hosted, <hardware-id>]`. 5. Android runner: benchmarks cross-compiled, pushed and executed via `adb shell`, results pulled.
**Acceptance tests:** 1. Given a baseline and a result 25 % slower, When `perf_compare.py`, Then exit 1 listing the benchmark with `+25.0 %`. 2. Given a result over an absolute budget, When compared, Then exit 1 with `budget exceeded`. 3. Given the M1 runner, When the nightly runs, Then a result file with `hardware_id == "m1-air-8gb"` is uploaded as an artefact. 4. Given `budgets.json`, When `tools/check_budgets.py` runs, Then every row of §14.9.3 appears with the same number (`tools/check_budgets.py` diffs the table against the file).
**Size:** M.
**Notes / pitfalls:** Android benchmarks run through `adb shell` with the device on mains and screen on for GPU items; `hardware_id` for the Pixel Tablet is `pixel-tablet` and for the HDR device `android-hdr` (§14.9.2).

### WI-0.561 — Corpora and dependency licence gate
**Goal:** `tools/license_gate.py` checks dependencies (ADR-002) and corpora (§14.3.4) in one CI job.
**Overlap:** this WI delivers only the corpora rules of §14.3.4 as the `--corpora` mode of `tools/license_gate.py` (owned by WI-0.602) and the `LICENSES.md` generator; no separate `tools/licence_gate.py` and no separate allowlist file — rules go into `tools/license_policy.toml`. See chapter 16 §16.3.
**Depends on:** WI-0.551, WI-0.602 (owns `tools/license_gate.py`).
**Produces:** the `--corpora` mode inside `tools/license_gate.py` (file owned by WI-0.602), corpora rules in `tools/license_policy.toml`, `tests/corpora/LICENSES.md` generator, the `--corpora` step of the `license-gate` job in `.github/workflows/ci.yml` (chapter 13 §13.7.1).
**Interface:** `license_gate.py --corpora [--manifest tests/corpora/manifest.json] [--write-licenses-md]`; exit 0 clean, 1 violation (named), 4 manifest invalid.
**Steps:** 1. read `vcpkg.json` + `vcpkg` SPDX output (`vcpkg x-package-info --x-json`) and `third_party/VERSIONS.md` licence column. 2. corpora rules 1–5 of §14.3.4. 3. generate `LICENSES.md`; fail if stale. 4. forbidden list `AGPL`, `SSPL`, `BUSL`, `NonCommercial`, `CC-BY-NC`.
**Acceptance tests:** 1. Given an entry with `licence_spdx: "AGPL-3.0-only"`, When run, Then exit 1 naming the id. 2. Given `redistributable: true` with `LicenseRef-ITU-Conformance`, When run, Then exit 1. 3. Given the committed manifest, When run, Then exit 0 and `LICENSES.md` unchanged.
**Size:** S.
**Notes / pitfalls:** `CC-BY-NC-*` content (e.g. Netflix "Sol Levante") is never redistributable and is not in the manifest (chapter 9 §9.11.1 uses synthetic PQ fixtures instead).

### WI-0.562 — GStreamer codecparsers oracle program
**Goal:** `gst_parse_oracle` of §14.4.2 built in the oracle image.
**Depends on:** WI-0.554.
**Produces:** `tests/oracles/gst_parse_oracle.c`, `tests/oracles/gst_fields.h`, `tools/gen_gst_fields.py`, `tests/oracles/CMakeLists.txt`.
**Interface:** `gst_parse_oracle --codec h264|h265|av1 [--annexb|--obu|--ivf] <in>` → one JSON object per unit on stdout (§14.4.2).
**Steps:** 1. field list generator from GStreamer headers (regex over struct members with a hand-maintained spec-name map). 2. H.264 path; 3. H.265 path; 4. AV1 path (OBU + IVF input); 5. JSON output and tests on `syn_*`.
**Acceptance tests:** 1. Given `syn_h264_multislice_cavlc.264`, When run, Then the JSON has 4 slice headers per frame with `first_mb_in_slice` increasing. 2. Given `syn_av1_tiles_cdef_lr.ivf`, When run, Then `tile_cols == 4` (`--tile-columns=2` is log2) and `enable_cdef == 1`.
**Size:** M.
**Notes / pitfalls:** GStreamer derives some fields (cropped width) rather than parsing them; list them in `gst_ignore.txt` rather than mapping them.

### WI-0.563 — HM getter dumper (`hm_cu_dump`) and JM/HM trace builds
**Goal:** HEVC block-grid oracle and H.264 trace oracle available.
**Depends on:** WI-0.554.
**Produces:** `tests/oracles/hm_cu_dump.cpp`, `tests/oracles/ffmpeg_mvs.c`, updates to `build_hm.sh`, `build_jm.sh`, `build_ffmpeg_oracle.sh`.
**Interface:** `hm_cu_dump -b <in>.bit` → block-grid JSON (§14.4.4); `ffmpeg_mvs <in>` → CSV rows `framenum,source,blockw,blockh,srcx,srcy,dstx,dsty,flags,motion_x,motion_y,motion_scale` (§14.4.3a); `ldecod` TRACE build.
**Steps:** 1. link against HM `TLibDecoder`; drive `TDecTop::decode` over NAL units read with HM's `TAppDecoder` bitstream reader. 2. per-picture walk of CTUs → 4×4 grid JSON of §14.4.4. 3. RExt handling (chroma formats). 4. JM TRACE build flags and `trace_dec.txt` capture. 5. tests on `syn_h265_amp_sao_on`, `syn_h264_mbaff`.
**Acceptance tests:** 1. Given `jctvc_AMP_A_Samsung_7`, When `hm_cu_dump`, Then a grid with `partition_size ∈ {4,5,6,7}` (AMP) appears. 2. Given `syn_h264_mbaff.264`, When `ldecod` (TRACE), Then `trace_dec.txt` contains `mb_field_decoding_flag`. 3. Given `jvt_BA1_Sony_D`, When `ffmpeg_mvs`, Then ≥ 1 row per P frame and every row has `motion_scale == 4`.
**Size:** M.
**Notes / pitfalls:** `ffprobe` cannot dump motion vectors (it prints only the side-data type), hence the dedicated program; `AV_CODEC_FLAG2_EXPORT_MVS` must be set before `avcodec_open2`.

### WI-0.564 — mp4box.js oracle script and ISOBMFF golden set
**Goal:** `mp4boxjs_dump.mjs` and the first ISOBMFF goldens (`-diso`, `-dts`, `mp4ff-info`, `mediainfo`, `mp4boxjs`) for the `fast` ISOBMFF corpus.
**Depends on:** WI-0.554, WI-0.555.
**Produces:** `tests/oracles/mp4boxjs_dump.mjs`, `tests/golden/isobmff/*.json`.
**Interface:** `node mp4boxjs_dump.mjs <in>.mp4` → box-tree JSON (§14.4.9); golden ids per §14.2.3.
**Steps:** 1. script per §14.4.9. 2. generation run in the image for all `fast` ISOBMFF files. 3. commit goldens with header comments of oracle versions.
**Acceptance tests:** 1. Given `gpac_counter...frag.mp4`, When the five oracles run, Then their `box-tree` outputs agree with each other on `fourcc/offset/size` for every box (oracle self-consistency test; disagreements are documented in `tests/golden/isobmff/KNOWN_DIFFS.md` before OVT is compared).
**Size:** S.
**Notes / pitfalls:** mp4box.js reports `hdr_size`; GPAC does not — compare offsets only where both report them.

### WI-0.565 — Metric oracle goldens (vmaf CLI, FFmpeg psnr, x264 SSIM kernel, Daala, bjontegaard, UVQ)
**Goal:** all §14.4.10–14.4.14 goldens generated for the `netflix`, `ytugc` fast subset and `syn_*` pairs.
**Depends on:** WI-0.554, WI-0.555, WI-0.553.
**Produces:** `tests/golden/metrics/**`, `tests/golden/metrics/rd_points/*.json`, `tests/oracles/x264_ssim/{pixel_ssim.c,main.c,CMakeLists.txt}`.
**Interface:** golden file names of §14.4.10–14.4.14 (`<id>.ffpsnr.json`, `<id>.<model>.vmaf.json`, `<id>.x264ssim.json`, `<id>.psnrhvs.json`, `<id>.ciede.json`, `bd/<set>.bd.json`, `<id>.uvq-<ver>.json`); `x264_ssim <ref>.y4m <dist>.y4m`.
**Steps:** 1. decode `syn_*` to Y4M in the image (`ffmpeg -i x -f yuv4mpegpipe`), 2. run each oracle (vmaf CLI with every bundled model, FFmpeg `psnr`, `x264_ssim`, Daala), 3. UVQ on CPU deterministic (TensorFlow), 4. BD sets (JVET-style 4-point, AOM 6-point, partial overlap; `cubic` only on 4-point sets), 5. commit.
**Acceptance tests:** 1. Given `src01_hrc00` vs `src01_hrc01`, When `vmaf --json`, Then pooled mean VMAF equals the value in Netflix `python/test/` (e.g. the `76.68…` reference in `vmaf_test.py` for the pinned model) — asserting the oracle itself is sane. 2. Given the two UVQ runs, When repeated, Then outputs are identical (determinism of the oracle). 3. Given `x264_ssim` on `src01_hrc00` vs `src01_hrc01`, When run, Then its per-plane mean equals `x264 --ssim` log output to 1e-4.
**Size:** M.
**Notes / pitfalls:** `tiny_ssim` must not be used (different kernel, D10); the vmaf CLI has no `--pool` option — all four pooled keys are in the JSON.

### WI-0.566 — libaom `inspect` goldens and AV1 index goldens
**Goal:** `aom-inspect.json` goldens for the `aom` fast subset and `syn_av1_*`.
**Depends on:** WI-0.554, WI-0.555, WI-0.553.
**Produces:** `tests/golden/av1/*.aom-inspect.json(.gz)`.
**Interface:** `inspect -A -a <in>` (§14.4.5) → `normalize/aom_inspect.py` → `block-grid` + `symbols`.
**Steps:** 1. run `inspect` with the flag set of §14.4.5; 2. normalise; 3. gzip > 1 MiB; 4. commit; 5. `test_golden_inspect_selfcheck.py` verifying frame counts vs `aomdec --summary`.
**Acceptance tests:** 1. Given `syn_av1_hidden_altref_showexisting.ivf`, When normalised, Then entries with `shown=false` exist and `symbols` sums per block are non-zero for shown frames.
**Size:** S.
**Notes / pitfalls:** pin the `inspect.c` argument table (it changes between libaom releases); `-x` must stay off.

### WI-0.567 — `trace_headers`, `gstparse`, JM and HM goldens for H.264/HEVC fast corpora
**Goal:** header and block goldens for H.26x.
**Depends on:** WI-0.562, WI-0.563, WI-0.555.
**Produces:** `tests/golden/h264/*.{trace_headers,gstparse,jm-trace,ffmpeg-mvs}.json`, `tests/golden/h265/*.{trace_headers,gstparse,hm-grid}.json`, `tests/golden/epb/syn_h264_epb_dense.json`.
**Interface:** `python -I tools/goldens/gen_h26x.py --manifest tests/corpora/manifest.json --out tests/golden/ [--only <id>]` (runs the §14.4.1–14.4.4 oracles and normalisers for every `fast` H.26x entry).
**Steps:** 1–4 one per oracle family; 5. cross-oracle self-check (`trace_headers` values == `gstparse` values on shared fields; differences recorded in `KNOWN_DIFFS.md`).
**Acceptance tests:** 1. Given the self-check, When run, Then zero undocumented disagreements.
**Size:** M.
**Notes / pitfalls:** CBS and GStreamer name some elements differently; changes go into the name maps, never into OVT's spec names.

### WI-0.568 — Screenshot diff tool and GUI test scaffolding
**Goal:** `tools/imgdiff.py` (modes `ui`, `hdr-parity`) and the Qt Quick Test / screenshot / test-IPC skeletons.
**Overlap:** depends only on the Qt skeleton of WI-0.019 for its trivial IPC test; the full test-IPC server is WI-1.465. See chapter 16 §16.3.
**Depends on:** WI-0.550, WI-0.019 (Qt skeleton with `--self-test`; enough for the trivial IPC test — the full test-IPC server is WI-1.465).
**Produces:** `tools/imgdiff.py`, `app/tests/CMakeLists.txt`, `app/tests/qml/tst_Smoke.qml`, `app/tests/screenshots/README.md`, `app/tests/screenshots/ovt_studio_screenshots.cpp` (the §14.10.2 harness), `app/tests/scripts/conftest.py` (IPC client), `docs/dev/gui-tests.md`.
**Interface:** `imgdiff.py --mode ui|hdr-parity <a.png> <b.png> [--diff out.png]` exit 0/1; §14.10.2 harness CLI `ovt_studio_screenshots --scene <name> --theme light|dark --out <dir>`; pytest fixture `studio(ipc)` yielding the §14.10.3 command set.
**Steps:** 1. `imgdiff.py` SSIM + tile rule; ΔE ITP mode with PNG16 PQ decode. 2. QML test target with offscreen platform. 3. screenshot grabber harness. 4. IPC client + pytest fixture launching `ovt-studio --test-ipc`.
**Acceptance tests:** 1. Given two identical PNGs, When `imgdiff.py --mode ui`, Then exit 0; Given one with a 20×20 red square, Then exit 1 and a diff PNG written. 2. Given a PQ PNG16 and the same scaled by 1.01 in linear light, When `--mode hdr-parity`, Then reported `p99 ΔE_ITP` is between 0.1 and 1.0 (sanity of the metric).
**Size:** M.
**Notes / pitfalls:** §14.10.2 is the only screenshot harness definition (chapter 11 §11.9 refers here, D8); chrome scenes run with `OVT_RENDER_STUB=1`, renderer scenes with lavapipe.

### WI-0.569 — OSS-Fuzz onboarding files and triage docs
**Goal:** `projects/ovt/` files ready to submit; crash triage procedure documented.
**Depends on:** WI-0.557.
**Produces:** `core/tests/fuzz/oss-fuzz/{project.yaml,Dockerfile,build.sh}`, `docs/dev/security/fuzzing.md` (triage §14.7.5), `docs/dev/security/upstream-findings.md`.
**Interface:** OSS-Fuzz `build.sh` contract (`$SRC`, `$OUT`, `$CFLAGS`, `$LIB_FUZZING_ENGINE`); `project.yaml` with `sanitizers: [address, undefined]`, `fuzzing_engines: [libfuzzer]`.
**Steps:** 1. files per §14.7.4; 2. local test with `infra/helper.py build_fuzzers --external` from the oss-fuzz repo; 3. docs.
**Acceptance tests:** 1. Given the oss-fuzz repo checkout, When `python infra/helper.py build_fuzzers --external $PWD` then `check_build`, Then all targets pass `check_build`.
**Size:** S.
**Notes / pitfalls:** OSS-Fuzz builds with its own clang; third_party must build from source under `$CFLAGS` (no prebuilt binaries).

### WI-1.570 — Golden suite: BitReader and splitters
**Goal:** unit + fuzz + golden coverage for `BitReader`, Annex B/OBU/IVF/Y4M splitters.
**Depends on:** WI-0.556, WI-0.557, WI-1.107 (unit scanners), WI-1.226, WI-1.227, WI-1.231 (container splitters).
**Produces:** `core/tests/bitreader/test_bitreader_*.cpp`, `core/tests/container/test_container_{annexb,obu,ivf,y4m}.cpp`, goldens `tests/golden/<codec>/<id>.ovt-units.json` compared against `MP4Box -dnal` (MP4) and `ffprobe -show_packets` (ES) unit counts/sizes.
**Interface:** `ovt syntax <in> --list --format json` → unit list (chapter 12 §12.2.3); `BitReader` API of chapter 7 §7.2.
**Steps:** 1. exhaustive `BitReader` tests (every descriptor, boundary, EPB mapping `epb_positions`). 2. splitter tests on all `fast` ES corpora: unit count/type/size equal the oracle. 3. fuzz seeds refreshed.
**Acceptance tests:** 1. Given `jvt_CABA3_TOSHIBA_E`, When split, Then unit count and per-unit sizes equal `ffprobe -show_packets`-derived NAL list (via `h264_mp4toannexb`-free `-bsf trace_headers` unit delimiters). 2. Given a stream with EPBs, When `ovt_stream_unit_bytes` returns raw and RBSP, Then `raw.size - rbsp.size == epb_positions.size()`.
**Size:** M.
**Notes / pitfalls:** `ffprobe -show_packets` on Annex B merges AUD/SEI into the packet of the following slice; compare per access unit, not per NAL, for the ES case.

### WI-1.571 — Golden suite: ISOBMFF reader
**Goal:** OVT `box-tree`/`sample-table` equals the five ISOBMFF oracles on the `fast` corpus.
**Depends on:** WI-0.564, WI-1.221, WI-1.224 (ISOBMFF reader complete through the validation driver).
**Produces:** `tests/golden/CMakeLists.txt` entries `golden_isobmff_<oracle>_<id>`, `core/tests/container/test_container_isobmff_*.cpp`, `fuzz_container_isobmff` + seeds.
**Interface:** `ovt probe <in> --format json --expand all`, `--samples --track <n>`, `--fragments`, `--edits`, `--enc` (chapter 12 §12.2.2) → `box-tree`/`sample-table`.
**Steps:** 1. wire goldens; 2. fix the field-name maps as discrepancies appear (maps, not OVT names, change); 3. derived views tests (edit list, fragments, CENC) vs `mp4ff-info`; 4. fuzz target.
**Acceptance tests:** 1. Given all `fast` ISOBMFF files, When `ctest -L golden -R isobmff`, Then green. 2. Given `syn_mp4_cenc_cbcs.mp4`, When compared with `mp4ff-info`, Then every `senc` IV and subsample entry matches. 3. Given `ovt_fuzz_container_isobmff` and its seed corpus, When it runs for 60 s, Then no crash, leak or timeout is reported.
**Size:** L.
**Notes / pitfalls:** GPAC and mp4ff disagree on a few field names; maps change, OVT names do not.

### WI-1.572 — Golden suite: H.264/HEVC/AV1 header parsers
**Goal:** `syntax-elements` and `header-struct` parity for all three codecs.
**Depends on:** WI-0.567, WI-0.566, WI-1.109, WI-1.110, WI-1.113, WI-1.114, WI-1.118, WI-1.120 (syntax parsers).
**Produces:** golden test entries, `core/tests/syntax/test_syntax_*.cpp` (one per header type with hand-checked fields), fuzz targets `fuzz_syntax_*` + seeds.
**Interface:** `ovt syntax <in> --format json --all-units --fields name,span,value` (§14.4.1) and `--fields name,value,enum_name,children` (§14.4.2); `tools/corrupt.py --truncate <pct>|--flip <n>`.
**Steps:** per codec (3 steps): wire `trace_headers` + `gstparse` goldens, add hand tests for tricky elements (H.264 `scaling_list`, HEVC `st_ref_pic_set` inter-RPS prediction, AV1 `frame_refs_short_signaling`, `global_motion_params`), add fuzz targets; 4. diagnostics tests on corrupted copies (`tools/corrupt.py`) asserting `Diagnostic.code` and that parsing continues with the next unit.
**Acceptance tests:** 1. Given the `fast` corpora, When `ctest -R syntax_golden`, Then green for `trace_headers` (positions) and `gstparse` (values). 2. Given `tools/corrupt.py --truncate 50%` of an SPS, When parsed, Then `OVT_E_CORRUPT`, a `Diagnostic` with `code` matching `SYN-H26[45]-\d{4}|SYN-AV1-\d{4}` and `span` inside the unit, no crash under ASan.
**Size:** L.
**Notes / pitfalls:** AV1 CBS positions include the OBU header; the normaliser, not the test, subtracts it (§14.4.1).

### WI-1.573 — Golden suite: StreamIndex
**Goal:** `StreamIndex` frames/POC/RAP/refs/hidden frames verified.
**Depends on:** WI-1.572, WI-1.121, WI-1.122, WI-1.123, WI-1.124 (index).
**Produces:** `tests/golden/<codec>/<id>.ovt-index.json` compared with: `ffprobe -show_frames` (pict_type, key_frame, pts, coded_picture_number) for H.26x; `inspect` frame list for AV1; HM/JM decode order POC lists (`TAppDecoder -o` log with `POC` lines) for POC; `core/tests/index/*.cpp`.
**Interface:** `ovt index <in> --format json [--with-units] [--refgraph]` (chapter 12 §12.2.4) → index JSON; `bench_index_*` of §14.9.3.
**Steps:** 1. ffprobe normaliser; 2. wire goldens; 3. AV1 hidden/show-existing tests; 4. `bench_index_*` added to perf.
**Acceptance tests:** 1. Given `jctvc_RAP_A_docomo_6`, When indexed, Then RASL frames after the CRA are marked non-decodable in `diagnostics` only when random access starts there, and `is_rap` is set on CRA. 2. Given `syn_av1_hidden_altref_showexisting`, When indexed, Then `display_order` length equals shown frame count and every `show_existing_of` points to an earlier `decode_index`. 3. Given the 1-hour file, When `bench_index_h264_1h_1080p`, Then ≤ 2 000 ms on M1.
**Size:** M.
**Notes / pitfalls:** `ffprobe -show_frames` numbers frames in decode order per `coded_picture_number` only for H.264; use `pts` ordering for display order.

### WI-1.574 — Decoded-picture MD5 suite and HW/SW tests
**Goal:** §14.5.2–14.5.5 tests for the playback software path.
**Depends on:** WI-2.171 (playback decoder), WI-0.552.
**Produces:** `core/tests/decode/test_decode_md5_corpus.cpp`, `core/tests/decode/test_decode_hw_vs_sw.cpp`, `cli/tests/test_dump_md5.py` (`ovt dump --md5`).
**Interface:** `ovt dump <in> --md5 [--stage recon --plugin <id> | --sw] [--bitdepth 8|16]` (chapter 12 §12.2.6; `ovt decode` alias) → `<md5>  <decode_index>` lines + sequence MD5.
**Steps:** 1. MD5 hashing of planes per §14.5.3; 2. corpus loop; 3. HW PSNR floor test (`[needs-display]`/`[needs-android]`); 4. per-frame localisation output.
**Acceptance tests:** 1. Given all `md5`-tagged corpus entries, When `ctest -R decode_md5`, Then every stream matches its reference MD5. 2. Given a HW decoder, When compared to SW, Then luma PSNR ≥ 50 dB (8-bit) / 56 dB (10-bit) for every frame, and the test records whether the HW output was bit-exact.
**Size:** M.
**Notes / pitfalls:** `aomdec --md5` hashes all frames in one digest; `dav1d --muxer md5` is per frame — the test uses dav1d's list to localise the first bad frame.

### WI-1.575 — Render parity suite (mpv/libplacebo) and `measure()` tests
**Goal:** §14.4.15 automated for SDR and HDR targets.
**Depends on:** WI-1.403, WI-1.412 (offscreen render and PNG export; no CLI is needed), WI-1.413, WI-0.568.
**Produces:** `tests/oracles/mpv_capture.py`, `tests/oracles/normalize/nits_reference.py`, `tests/golden/render/*.png` (LFS), `render/tests/test_render_parity.cpp` (`[needs-gpu]`).
**Interface:** `mpv_capture.py --mode sdr|hdr --frame <n> <in> <out.png>` (§14.4.15, `--start=<seconds> --hr-seek=yes`); `imgdiff.py --mode hdr-parity`; `nits_reference.py <in> --frame n`.
**Steps:** 1. capture script; 2. reference nits script; 3. offscreen render test; 4. CI lavapipe setup; 5. macOS MoltenVK nightly.
**Acceptance tests:** 1. Given `syn_h265_main10_pq_hdr` frame 10, When rendered to the SDR target, Then `p99 ΔE_ITP ≤ 1.0` vs the mpv capture. 2. Given the same frame, When rendered to the HDR (PQ) target, Then `p99 ΔE_ITP ≤ 0.5` vs the mpv capture. 3. Given `syn_pat_pq_patches_1000` frame 0, When `measure()` runs, Then MaxCLL/FALL are within 1 % of `nits_reference.py`.
**Size:** M.
**Notes / pitfalls:** tone-mapping constants change between libplacebo majors; the test pins the major and fails loudly on a mismatch rather than widening tolerances.

### WI-1.576 — GUI model tests, screenshots and interaction scripts (phase 1 scope)
**Goal:** §14.10.1 tests for the phase-1 models, 5 screenshot scenes, scripts 1–2 and 5 of §14.10.3.
**Overlap:** the test-IPC server in `app/src/testipc/` is produced by WI-1.465; this WI delivers tests, scripts and screenshots only. See chapter 16 §16.3.
**Depends on:** WI-0.568, WI-1.465 (test-IPC server), WI-1.455, WI-1.457, WI-1.458, WI-1.460, WI-1.462.
**Produces:** `app/tests/qml/tst_{ContainerTreeModel,SyntaxTreeModel,HexViewModel,TimelineModel,HdrMetadataModel,Shortcuts}.qml`, reference screenshots under `app/tests/screenshots/<platform>/`, `app/tests/scripts/test_{open_click_field_hex,timeline_scrub,hdr_panel}.py`.
**Interface:** §14.10.1 test list, §14.10.2 scenes, §14.10.3 scripts 1, 2 and 5 over the test IPC of WI-1.465.
**Steps:** 1. QML model tests with fixture data. 2. Screenshot scenes (both themes). 3. Scripts 1, 2, 5. 4. CI wiring in the `gui` job of `ci.yml` (owned by WI-1.465 / chapter 13).
**Acceptance tests:** 1. Given `QT_QPA_PLATFORM=offscreen` on Linux and macOS, When `ctest -R qmltests` runs, Then all tests pass. 2. Given the reference images, When the screenshot job runs, Then all scenes pass `imgdiff --mode ui`. 3. Given `jctvc_AMP_A_Samsung_7`, When script 1 runs, Then the highlighted bit span equals the `trace_headers` golden span for `pic_width_in_luma_samples`.
**Size:** L.
**Notes / pitfalls:** `tst_Shortcuts.qml` reads the §11.4.3 table (chapter 11) as its expected list — the single keyboard map.

### WI-1.577 — Android instrumentation smoke (phase 1)
**Goal:** §14.10.4 steps 1–4 (+5 on HDR devices) automated.
**Overlap:** primary for Android instrumentation CI (absorbs WI-A.457). See chapter 16 §16.3.
**Depends on:** WI-0.050, WI-0.051, WI-A.452, WI-A.454 (Android shell), WI-0.553 (`syn_*`).
**Produces:** `android/app/src/androidTest/java/org/ovt/SmokeTest.kt`, `.github/workflows/android-nightly.yml` (emulator + `pixel-tablet` + `android-hdr` device jobs).
**Interface:** §14.10.4 steps; intent extra `OVT_OPEN_URI`; logcat line `OVT_HDR dataspace=<BT2020_PQ|BT2020_HLG|SRGB>` as defined by chapter 13 WI-0.052 (emitted by `ovt_jni_surface_attach`; this WI only reads it); fixture `syn_h264_multislice_cavlc.mp4` (recipe 29).
**Steps:** 1. Smoke test class. 2. Emulator job (API 36 `google_apis` x86_64, swiftshader; chapter 13 §13.7.1). 3. Device jobs via the self-hosted Android runner of WI-0.560. 4. Artefact upload.
**Acceptance tests:** 1. Given the emulator job, When `./gradlew -p android :app:connectedFossDebugAndroidTest`, Then all tests pass and the screenshot artefact is uploaded. 2. Given the Pixel Tablet, When the same command runs, Then all tests pass and `adb logcat -d -s OVT_HDR` shows `dataspace=SRGB` (device is SDR). 3. Given the `android-hdr` device, When step 5 runs, Then it passes with `dataspace=BT2020_PQ`.
**Size:** M.
**Notes / pitfalls:** the Pixel Tablet never runs HDR assertions (D12).

### WI-2.580 — Validator implementation and rule tests
**Goal:** `ovt::analysis::validate` with all V-rules of §14.6.1 and one negative test each.
**Overlap:** superseded by WI-0.086 (validator V-001…V-032 in `core/src/model/validator.cpp`); the `ovt analyze --validate` flag moves to WI-2.521 and the cross-plugin consistency checks of §14.6.2 move to WI-2.153. See chapter 16 §16.3.
**Depends on:** WI-0.086 (validator, ch. 6), WI-2.151.
**Produces:** `core/tests/analysis/test_analysis_validator.cpp` only (zero-violation check over every phase-2 golden `FrameAnalysis`); the validator itself (`core/src/model/validator.cpp`), its negative tests and the `--validate` flag belong to WI-0.086 / WI-2.521.
**Interface:** `ovt::model::validate_frame_analysis` (chapter 06 WI-0.086); `ovt::test::analysis_frame(...)` helper of §14.6.1.
**Steps:** 1. Hook `validate_frame_analysis` into `analysis_frame(...)`. 2. Golden sweep test. 3. Remove any duplicate rule code from `core/tests/`.
**Acceptance tests:** 1. Given a hand-built buffer violating V-011, When validated, Then exactly `V-011` is reported with the offending cell index. 2. Given every golden `FrameAnalysis` of phase 2, When validated, Then zero violations.
**Size:** M.
**Notes / pitfalls:** superseded WI: the rule implementations and negative tests belong to chapter 06 WI-0.086; this WI only keeps the golden sweep so that a rule added later cannot silently break the phase-2 goldens.

### WI-2.581 — Golden suite: `ovt-plugin-aom` vs `inspect`
**Goal:** §14.4.5 and `test_xplugin_av1_aom_vs_inspect` green on the `aom` fast subset and `syn_av1_*`.
**Depends on:** WI-0.566, WI-0.086 (validator), WI-2.157 (`ovt-plugin-aom` with accounting).
**Produces:** golden entries, `plugins/ovt-plugin-aom/tests/test_aom_*.cpp`, `fuzz_plugin_push_aom`.
**Interface:** `ovt analyze <in> --plugin aom --all --order decode --format json` (§14.4.5) vs `<id>.aom-inspect.json`.
**Steps:** 1. Wire goldens. 2. Name map fixes. 3. Threads invariance test. 4. Fuzz target. 5. Bench row.
**Acceptance tests:** 1. Given the fast set, When `ctest -R golden_av1_inspect`, Then every grid field, `symbols` and `bits_x8` match exactly. 2. Given the same stream, When analysed with `threads=1` and `threads=4`, Then the FlatBuffer bytes are identical. 3. Given `syn_av1_gop60_1080p` on `m1-air-8gb`, When `bench_analysis_aom_gop60_1080p` runs, Then it meets its §14.9.3 budget.
**Size:** M.
**Notes / pitfalls:** `bits_x8` per block must be summed from the oracle's symbols; the oracle has no per-block total.

### WI-2.582 — Golden suite: `ovt-plugin-de265` vs HM grid
**Goal:** §14.4.4 and `test_xplugin_hevc_de265_vs_hm` green on the `jctvc` Main/Main10 fast subset.
**Depends on:** WI-0.567, WI-0.563 (`hm_cu_dump`), WI-0.086 (validator), WI-2.164 (`ovt-plugin-de265`).
**Produces:** golden entries, `plugins/ovt-plugin-de265/tests/*.cpp`, `fuzz_plugin_push_de265`, recon equality test vs FFmpeg.
**Interface:** `ovt analyze <in> --plugin de265 --all --format json --fields …` (§14.4.4) vs `<id>.hm-grid.json`.
**Steps:** 1. Wire goldens. 2. Recon equality. 3. Fuzz target. 4. Bench row.
**Acceptance tests:** 1. Given the fast set, When compared, Then `pred, qp, intra_mode_*, mv*, ref*, tx_log2_*, cbf, partition_id` and the block tree match exactly. 2. Given the fast set, When `PlaneStage::Reconstruction` is compared with the FFmpeg hevc software decode, Then it is bit-exact. 3. Given `syn_h265_gop60_1080p` on `m1-air-8gb`, When `bench_analysis_de265_gop60_1080p` runs, Then it meets its §14.9.3 budget.
**Size:** M.
**Notes / pitfalls:** HM MVs are 1/4-pel; the ×4 conversion lives in the normaliser.

### WI-2.583 — Golden suite: `ovt-plugin-ffmpeg` (H.264 MVs/QP) vs `ffmpeg_mvs` and JM QP
**Goal:** phase-2 H.264 parity per §14.4.3/14.4.3a.
**Depends on:** WI-0.567, WI-0.086 (validator), WI-2.170 (`ovt-plugin-ffmpeg` side data mapping).
**Produces:** golden entries, `plugins/ovt-plugin-ffmpeg/tests/*.cpp`, `fuzz_plugin_push_ffmpeg`.
**Interface:** `ovt analyze <in> --plugin ffmpeg --all --order decode --format json` (§14.4.3) vs `<id>.ffmpeg-mvs.json` and `<id>.jm-trace.json`.
**Steps:** 1. Wire the two goldens. 2. QP accumulation check. 3. Fuzz target.
**Acceptance tests:** 1. Given `jvt` fast streams, When compared with `<id>.ffmpeg-mvs.json`, Then every exported MV (≥ 8×8 partitions) matches in 1/16-pel after `motion_scale` conversion. 2. Given the same streams and the JM `trace`, When compared, Then the per-MB final QP matches.
**Size:** M.
**Notes / pitfalls:** FFmpeg reports the block centre in `dst_x/dst_y`; convert to the top-left rect before comparing.

### WI-2.584 — Overlay performance and GUI tests (phase 2 scope)
**Goal:** `bench_overlay_*` in the perf harness; QML tests for `SelectionInfoModel`/`OverlayToggleModel`; screenshots `picture_overlay_*`; script 3 of §14.10.3; YUView CSV round-trip test.
**Depends on:** WI-0.560, WI-1.576, WI-2.438, WI-2.481, WI-2.482, WI-2.485.
**Overlap:** `tests/perf/render/bench_overlay.cpp` is produced by chapter 10 WI-2.438; this WI registers it in the harness and adds the GUI tests. See chapter 16 §16.3.
**Produces:** budget rows for `bench_overlay_*` in `tests/perf/budgets.json`, `app/tests/qml/tst_FrameAnalysisModel.qml`, screenshot scenes `picture_overlay_*`, `app/tests/scripts/test_block_click_selection_info.py`, `core/tests/model/test_yuview_csv_roundtrip.cpp`, `tools/yuview_csv_lint.py` (D27: checks an OVT YUView statistics CSV against the YUView 2.x import grammar — header `%;syntax-version;v1.22`, `%;seq-specs;…`, `%;type;<id>;<name>;<kind>` declarations before use, row field counts per declared kind; exit 1 with line number on the first violation), `tools/tests/test_yuview_csv_lint.py`.
**Interface:** §14.9.3 rows `bench_overlay_4k_mv_arrows`, `bench_overlay_build`; §14.10.3 script 3; `ovt analyze --format yuview`; `python3 tools/yuview_csv_lint.py <file.csv>` exit 0/1.
**Steps:** 1. Budget rows and nightly registration. 2. QML tests. 3. Scenes. 4. Script 3. 5. YUView round trip. 6. `yuview_csv_lint.py` and its tests.
**Acceptance tests:** 1. Given `fa_4k_dense.bin` on `m1-air-8gb` (and `fa_1080p_dense.bin` on `pixel-tablet`), When `bench_overlay_4k_mv_arrows` runs, Then p95 meets its §14.9.3 budget on each device. 2. Given script 3 on `syn_h265_amp_sao_on.265`, When a block is clicked, Then Selection Info values equal `ovt analyze` JSON for the clicked block. 3. Given `ovt analyze syn_h265_amp_sao_on.265 --format yuview -o a.csv`, When `tools/yuview_csv_lint.py a.csv` runs and the file is imported back, Then the linter exits 0 and the imported block values equal the original. 4. Given a CSV with a statistic row whose type id was never declared, When the linter runs, Then exit 1 naming that line.
**Size:** M.
**Notes / pitfalls:** the 1080p Android fixture is `fa_1080p_dense.bin` (same generator, `--size 1080p`).

### WI-3.590 — Golden suite: PSNR/SSIM kernels vs FFmpeg psnr, the x264 kernel oracle and libvmaf float_ssim
**Goal:** §14.4.10 (psnr) and §14.4.12 green; per-block map tests; cross-architecture tolerance rule.
**Depends on:** WI-0.565, WI-3.306, WI-3.307, WI-3.308 (kernels).
**Produces:** golden entries, `core/tests/metrics/test_metrics_{psnr,ssim,ssim_x264,maps,xarch}.cpp`, `tests/perf/metrics/bench_metrics.cpp` rows `bench_metrics_psnr_ssim_1080p`, `bench_metrics_psnr_1080p`, `bench_metrics_maps`.
**Interface:** `ovt metrics -r … -d … -m psnr,ssim,ssim_x264 --align none --threads 1 --format json` (chapter 12 §12.2.9); goldens `<id>.ffpsnr.json`, `<id>.x264ssim.json`, libvmaf `float_ssim` from `<id>.vmaf_v0.6.1.vmaf.json`.
**Steps:** 1. PSNR goldens (FFmpeg `%0.2f` rounding rule + float64 recomputation). 2. `ssim_x264` vs the kernel oracle. 3. `ssim` vs libvmaf `float_ssim`. 4. Map conservation test. 5. Cross-architecture goldens: results recorded on x86_64 (AVX2) compared on arm64 (NEON) and the scalar build. 6. Bench rows.
**Acceptance tests:** 1. Given the `netflix` and `syn_*` pairs of §14.4.10, When `psnr` runs, Then PSNR per frame equals FFmpeg's printed `%0.2f` value after rounding and the float64 recomputation within 1e-6 dB, and identical frames map `inf` to `psnr_max`. 2. Given the same pairs, When `ssim_x264` and `ssim` run, Then `ssim_x264` is within 1e-6 of the x264 kernel oracle (8-bit integer path and 10-bit float path) and `ssim` (Gaussian 11×11) within 1e-4 of libvmaf `float_ssim`. 3. Given `map_block = 16`, When `psnr` runs with maps, Then the sum of per-block MSE × block area equals frame MSE × frame area within 1e-9 relative. 4. Given goldens recorded on x86_64 (AVX2), When the suite runs on arm64 (NEON) and on the scalar build, Then integer and fixed-point outputs (PSNR MSE sums, `ssim_x264`) bit-identical between x86_64, arm64 and the scalar build; float outputs (`ssim`, `ms_ssim`) within **1e-4 relative** (chapter 9 §9.4.1). 5. Given `syn_metrics_1080p_pair` on `m1-air-8gb`, When `bench_metrics_psnr_ssim_1080p` runs with 4 threads, Then it meets its §14.9.3 budget.
**Size:** M.
**Notes / pitfalls:** no `tiny_ssim`, no `ssim_ffmpeg` (D10, chapter 9 §9.4.3); SIMD lane order makes float SSIM non-bit-exact across ISAs — never tighten the 1e-4 rule without a fixed-order accumulation.

### WI-3.591 — Golden suite: libvmaf host bit-exact vs `vmaf` CLI
**Goal:** §14.4.11 bit-exact, all models and features, pooling and subsample.
**Depends on:** WI-0.565, WI-3.309, WI-3.310 (libvmaf host and exporter), WI-3.530.
**Produces:** golden entries, `core/tests/metrics/test_metrics_vmaf.cpp`, `tests/perf/metrics/bench_metrics.cpp` row `bench_metrics_vmaf_1080p`, exporter precision test.
**Interface:** `ovt metrics -r … -d … -m vmaf --model version=<name> --opt vmaf.features=… --align none --format vmaf-json -o …` (§14.4.11) vs `<id>.<model>.vmaf.json`, compared with `tests/oracles/normalize/vmaf_json_compare.py` (produced by chapter 12 WI-3.530).
**Steps:** 1. Wire goldens for every bundled model. 2. Feature set variants. 3. `--subsample 5`. 4. Exporter `%.6f` precision test. 5. Bench row.
**Acceptance tests:** 1. Given every `netflix` pair × model × feature set, When `ovt metrics --format vmaf-json`, Then `diff` with the CLI JSON (after `fps` removal) is empty, including all four `pooled_metrics` keys. 2. Given `--subsample 5` on the same pairs, When both tools run, Then the documents are identical. 3. Given `syn_metrics_1080p_pair` on `m1-air-8gb`, When `bench_metrics_vmaf_1080p` runs with 4 threads, Then it meets its §14.9.3 budget.
**Size:** M.
**Notes / pitfalls:** the vmaf CLI has no pooling option; OVT's pooling choice (`--pool`) affects only the OVT JSON, never the libvmaf-compatible export.

### WI-3.592 — Golden suite: normalisation stage and alignment
**Goal:** zimg conversions and alignment search verified.
**Depends on:** WI-3.301, WI-3.302, WI-3.303, WI-3.304, WI-3.305 (normalisation and alignment).
**Produces:** `core/tests/metrics/test_metrics_normalize.cpp`, `test_metrics_align.cpp`, goldens from `ffmpeg -vf zscale=...` (`zscale=t=linear:npl=100,format=gbrpf32le` etc.) for PQ→linear, range and primaries conversions, `tests/perf/metrics/bench_metrics.cpp` rows `bench_metrics_normalize_1080p_pq`, `bench_metrics_align`.
**Interface:** `NormalizationConfig` / `--normalize <json>` and `--align auto|N|none` (chapter 9 §9.2.4, §9.3.1; chapter 12 §12.2.9); `syn_align_drop7` fixture.
**Steps:** 1. zscale goldens. 2. Alignment tests (drop, duplicate, none). 3. Range/primaries tests. 4. Bench rows.
**Acceptance tests:** 1. Given a 10-bit PQ frame, When normalised to `LinearLight`, Then max \|Δ\| vs `zscale` output ≤ 1e-5 (float). 2. Given `syn_align_drop7` (7 leading frames dropped from the distorted stream), When aligned, Then `offset_frames == +7` (chapter 9 §9.3.1 sign) with `confidence ≥ 0.5` and `status == "ok"`; Given 2 duplicated frames mid-stream, Then `VQ-ALIGN-0002` and per-GOP re-alignment finds them. 3. Given `syn_metrics_scale_range_pair`, When normalised, Then normalisation converts and PSNR equals the matched-range PSNR within 0.05 dB.
**Size:** M.
**Notes / pitfalls:** the confidence definition is `S(o*) − S(o2)` (chapter 9 §9.3.1), so a clean synthetic pair rarely exceeds ≈ 1; thresholds above 0.5 would be wrong for real content.

### WI-3.593 — Golden suite: UVQ host and BD-rate
**Goal:** §14.4.13 (bjontegaard) and §14.4.14 green.
**Depends on:** WI-0.565, WI-3.315 (UVQ host), WI-3.313 (BD-rate).
**Produces:** `core/tests/metrics/test_metrics_uvq.cpp` (`[slow][needs-corpus]`), `core/tests/metrics/test_metrics_bdrate.cpp`, golden entries, `tests/perf/metrics/bench_metrics.cpp` row `bench_metrics_uvq_chunk`.
**Interface:** `ovt metrics -d <clip> -m uvq --opt uvq.version=… --opt uvq.fps=1 --format json` (§14.4.14); `ovt bdrate --anchor --test --metric --interpolator --range --format json` (chapter 12 §12.2.10).
**Steps:** 1. UVQ parity (every pinned model version). 2. BD sets. 3. Error-path parity. 4. Bench row.
**Acceptance tests:** 1. Given 3 `ytugc` clips, When UVQ runs via ONNX Runtime (CPU EP), Then \|Δ MOS\| ≤ 0.01 per chunk and per clip vs the reference CLI for every pinned model version. 2. Given the committed RD sets, When `ovt bdrate` runs each interpolator, Then BD-rate is within 1e-6 % of the `bjontegaard` package for PCHIP and Akima on all sets and for cubic on the 4-point sets (the package's cubic mode accepts exactly 4 points; OVT's cubic is the least-squares fit of chapter 9 §9.8.4 for ≥ 4 points, checked on the 6-point sets against `numpy.polyfit(D, log10 R, 3)` integration to 1e-9); a 3-point set with cubic fails with `VQ-BD-0002`. 3. Given a non-overlapping RD set, When `ovt bdrate` runs, Then it exits **8** (`MISMATCH`, chapter 12 §12.1.3) with diagnostic `VQ-BD-0001` and the Python package raises likewise (documented behaviour parity).
**Size:** M.
**Notes / pitfalls:** the reference CLI is TensorFlow, OVT is ONNX Runtime — a ≤ 0.01 MOS budget assumes the tf2onnx export of chapter 9 §9.6 with opset ≥ 13.

### WI-3.594 — Metrics GUI tests and Android metric parity
**Goal:** QML tests for metrics models, screenshot `metrics_panel_vmaf`, script 4, and an Android instrumentation test comparing PSNR/SSIM/VMAF results with desktop goldens.
**Depends on:** WI-1.576, WI-1.577, WI-3.450, WI-3.451, WI-3.452, WI-3.317.
**Produces:** `app/tests/qml/tst_MetricsModel.qml`, `app/tests/scripts/test_metrics_run.py`, `android/app/src/androidTest/java/org/ovt/MetricsParityTest.kt`.
**Interface:** §14.10.3 script 4; §14.10.2 scene `metrics_panel_vmaf`; `ovt metrics … --format vmaf-json` on both platforms.
**Steps:** 1. QML tests. 2. Scene. 3. Script 4. 4. Android parity test.
**Acceptance tests:** 1. Given script 4 on `syn_metrics_1080p_pair`, When PSNR runs in the GUI and in the CLI, Then the pooled PSNR values are equal exactly (same core call). 2. Given a 720p pair (the first 60 frames of `syn_align_drop7`'s reference and distorted files) on `pixel-tablet`, When `MetricsParityTest` runs, Then the libvmaf-compatible JSON is byte-identical to the macOS CLI output for fixed-point features and the VMAF score; float features agree within 1e-4 relative (chapter 9 §9.4.1).
**Size:** M.
**Notes / pitfalls:** run the Android job on `pixel-tablet` (SDR is irrelevant for metrics).

### WI-4.595 — Golden suite: full H.264 `AvcMb` vs JM trace
**Goal:** phase-4 H.264 export patch verified field by field.
**Depends on:** WI-2.583, WI-4.102 (FFmpeg patch consumed into `AvcMb`).
**Produces:** extended comparison in `test_xplugin_h264_ffmpeg_vs_jm`, goldens for all `jvt` fast streams incl. MBAFF/PAFF.
**Interface:** `AvcMb` field table (chapter 06) ↔ `normalize/jm_trace.py` keys (identity map).
**Steps:** 1. Enable the full key set in the comparer. 2. MBAFF/PAFF goldens. 3. Fix normaliser gaps.
**Acceptance tests:** 1. Given every `jvt` fast stream, When compared with the JM trace, Then `mb_type, sub_mb_type, ref_idx_l0/l1, intra4x4_pred_mode, intra_chroma_pred_mode, cbp, mb_qp_delta, transform_size_8x8_flag` match JM exactly for every MB. 2. Given `jvt_CAMA1_Sony_C` (MBAFF), When analysed, Then `Block.ext.mb_field_decoding_flag` matches and the cell grid uses frame coordinates per §0.1.9.
**Size:** M.
**Notes / pitfalls:** JM traces skipped MBs implicitly; the normaliser synthesises them (§14.4.3).

### WI-4.596 — Golden suite: `ovt-plugin-hm` and `ovt verify --hash-sei`
**Goal:** HM plugin equals `hm_cu_dump` incl. RExt; picture-hash verification tested per §14.5.4.
**Depends on:** WI-0.563, WI-4.105, WI-4.106, WI-0.553 (`tools/corrupt.py`).
**Produces:** golden entries for `jctvc` incl. `RExt`, `tests/golden/h265/corrupt/*.bit` (generated with `tools/corrupt.py` of WI-0.553), `core/tests/verify/test_verify_picture_hash.cpp`.
**Interface:** `ovt verify <in> --hash-sei [--plugin hm]` (chapter 12 §12.2.12; `--picture-hash` alias) → per-picture `match|mismatch` report with `DEC-H265-0001`.
**Steps:** 1. HM plugin goldens incl. RExt. 2. Corrupt set generation. 3. Hash verification tests.
**Acceptance tests:** 1. Given `jctvc_RExt_Main_422_10_A_RExt_Sony_2`, When analysed with the HM plugin, Then grid equals `hm_cu_dump`. 2. Given `syn_broken_h265_hash_mismatch` (picture 17 corrupted), When `ovt verify --hash-sei` runs, Then exactly the affected picture reports `mismatch` with `DEC-H265-0001`.
**Size:** M.
**Notes / pitfalls:** CRC and checksum variants (hash_type 1/2) need their own corrupt samples; MD5 alone is not enough.

### WI-4.597 — Golden suite: Matroska, MPEG-TS, DASH/HLS virtual files
**Goal:** EBML and TS trees vs `mediainfo --Details=1` and `mkvinfo -v -v -z`; DASH/HLS virtual file vs segment concatenation.
**Depends on:** WI-4.203, WI-4.207, WI-4.210.
**Produces:** goldens under `tests/golden/{ebml,mpegts,dash,hls}/`, normalisers `mkvinfo.py`, `mediainfo_ts.py`, fuzz targets `fuzz_container_ebml`, `fuzz_container_mpegts`, `fuzz_container_m3u8`, `fuzz_container_mpd`.
**Interface:** `ovt probe <in> --format json --expand all` for MKV/TS; `ovt probe <mpd|m3u8> --representation <n>` (chapter 12 §12.2.2) → virtual-file box tree; `mkvinfo -v -v -z`, `mediainfo --Details=1 --Output=XML`.
**Steps:** 1. Build MKV/TS from `syn_*`. 2. Normalisers. 3. Goldens. 4. Virtual-file equality. 5. Fuzz targets.
**Acceptance tests:** 1. Given `syn_mkv_h264` and `syn_ts_cc_errors`'s clean input `syn_ts_clean.ts`, When `ovt probe --format json --expand all` runs, Then element/packet offsets and sizes match MediaInfo and MKV element ids/sizes match mkvinfo. 2. Given `dashif_testcase_1a` served by `tools/tests/fixture_server.py`, When opened as a virtual file, Then `ovt index` equals `ovt index` of `cat init.mp4 seg1..3.m4s`. 3. Given each new fuzz target, When it runs for 60 s from its seed corpus, Then no crash, leak or timeout is reported. 4. Given `ts_h264_dvb`, When `ovt probe --format json` runs, Then PSI tables equal `tstables --all-sections` (TSDuck) output after normalisation.
**Size:** L.
**Notes / pitfalls:** MediaInfo trace offsets are decimal in XML, hex in text; always `--Output=XML`.

### WI-4.598 — Python package tests, sidecar protocol tests, HDR scopes tests
**Goal:** pytest for `python/ovt`; sidecar protocol round-trip with a mock metric and with ColorVideoVDP (optional, `[slow]`); scope renderers tested against `nits_reference.py`.
**Depends on:** WI-4.540, WI-4.541, WI-4.542 (Python), WI-4.304, WI-4.305 (sidecar), WI-4.453, WI-4.454 (HDR scopes).
**Produces:** `python/tests/test_*.py`, `core/tests/metrics/test_metrics_sidecar.cpp` + `tests/oracles/mock_sidecar.py`, `render/tests/test_scopes.cpp`.
**Interface:** `ovt` Python API of chapter 12 §12.4; sidecar protocol of chapter 9 §9.7; `ScopeTexture` of chapter 10 §10.7.
**Steps:** 1. Python tests. 2. Mock sidecar + protocol tests. 3. ColorVideoVDP `[slow]` test. 4. Scope tests.
**Acceptance tests:** 1. Given the installed package and `syn_mp4_elst.mp4`, When `python -I -c "import ovt; t=ovt.probe('syn_mp4_elst.mp4'); print(t.tracks[0].codec)"` runs, Then it prints `h264`. 2. Given the mock sidecar returning `frame_index × 0.5`, When run through the metric plugin host, Then `MetricsResult.frames[i].values[0] == 0.5 i` and shared-memory frames are released (no leak under ASan). 3. Given `syn_pat_pq_patches_1000`, When the nits waveform is computed, Then the waveform's peak row corresponds to MaxCLL within 1 %.
**Size:** M.
**Notes / pitfalls:** run Python tests with `python -I`; shared-memory frames must be unlinked on every exit path (ASan + `ls /dev/shm` check).

### WI-4.599 — Phase-4 acceptance replay and feature-matrix audit
**Goal:** every step of §14.11 Phase 4 and Track A automated where possible and recorded; chapter 03 must-have matrix ticked with evidence links.
**Depends on:** none (runs last in M4.4, after every other phase-4 WI is Done; an ordering by milestone, not a dependency edge — issue #17).
**Produces:** `docs/acceptance/phase-4.md`, `tools/acceptance_replay.py` (runs the automatable steps of §14.11 for any phase and produces the Markdown record skeleton), `docs/acceptance/feature-matrix.md`, `tools/check_feature_matrix.py` (fails on a must-have row without a test name or acceptance record).
**Interface:** `acceptance_replay.py --phase 0|1|2|3|4|A [--only <step>] [--record docs/acceptance/phase-<n>.md]`; exit 1 on any automatable step failing.
**Steps:** 1. Step table from §14.11 as data. 2. Runner. 3. Markdown skeleton. 4. Feature-matrix generator from chapter 03 rows.
**Acceptance tests:** 1. Given a phase-4 build, When `tools/acceptance_replay.py --phase 4` runs, Then every automatable step prints PASS and the Markdown skeleton lists manual steps with empty result fields. 2. Given the feature matrix, When `tools/check_feature_matrix.py` runs, Then every must-have row links to a test name or an acceptance record.
**Size:** M.
**Notes / pitfalls:** `docs/acceptance/` is a chapter 0 §0.4 directory; manual steps are never auto-ticked.

---

### Glossary additions (chapter 14)

Every term this chapter uses is defined in chapter 2 (Glossary). The entries below add chapter-specific detail that chapter 2 does not carry; entries that merely repeated chapter 2 were removed.

- **libFuzzer target** — a function `LLVMFuzzerTestOneInput` exercising one parser with mutated input under sanitizers; OVT has one per parser (§0.7.1).
- **Kernel oracle** — an oracle built from the vendored, unmodified reference implementation of a metric kernel (the x264 SSIM functions of §14.4.12) rather than from a tool's printed output.
