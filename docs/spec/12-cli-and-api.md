# 12 — CLI, C API and Python API

> **Status:** normative chapter of the OVT Requirements & Architecture Specification. Conforms to `00-backbone.md` (ADR-001 C++20 / C ABI, ADR-002 licensing split, ADR-003 layers, ADR-010 FlatBuffers, ADR-013 process model, ADR-015 decoder plugins). Where this chapter names a FlatBuffers table it means the table of that exact name in `core/schema/*.fbs` (chapter 06). Licence of everything under `cli/` and `python/` is GPL-3.0-or-later; `core/include/ovt/*.h` is LGPL-2.1-or-later; `core/include/ovt/abi/*.h` is BSD-2-Clause (ADR-002).

This chapter is written for implementers who will build the `ovt` command-line tool, the public C API of `libovt-core` and the Python package `ovt` without having used a commercial analyzer. Everything a front-end can show MUST be reachable through these three surfaces (ADR-003). The GUI (chapter 11) is a *client* of the same C API; a feature that exists only in the GUI is a specification bug.

---

## 12.1 Principles

### 12.1.1 Requirements realised by this chapter

Requirement ids are defined only in chapter 04; this table copies chapter 04's sentences and names what realises them here. Ids in the `FR-<AREA>-1nn` range were added to chapter 04 from this chapter's review. FR-CLI-003 uses the exit-code table of §12.1.3 (D3) and FR-API-002 the ownership rule of §12.3.1 rule 3 (D4).

| ID | Requirement (chapter 04 text) | Realised by | Acceptance test (chapter 04 text) |
|---|---|---|---|
| FR-API-001 | The C API in `core/include/ovt/*.h` MUST be the only public interface of `libovt-core`, MUST compile as C11 and C++20, MUST use only the types declared in chapter 0 §0.6.1 (`ovt_status`, `ovt_Buffer`, opaque handles, `ovt_ColorDescription`, `ovt_PlaneView`, `ovt_FrameId`), and every function MUST be documented with Doxygen including ownership of every pointer. | §12.3.1 (header set, C11 conventions, Doxygen ownership rule 8), WI-1.511 | Given the headers, when `core/tests/c_api_compile.c` is compiled with `clang -std=c11 -Wall -Werror -fsyntax-only` and `doxygen -q` runs with `WARN_AS_ERROR = YES`, then both exit 0. |
| FR-API-002 | Ownership MUST follow D4 and chapter 12 §12.3.1 rule 3: a FlatBuffer or buffer returned through an out-parameter is owned by the caller and released with `ovt_buffer_free` (or the type-specific `ovt_<type>_release`, e.g. `ovt_picture_release` for the ref-counted `ovt_Picture`); a pointer returned by value (`ovt_container_tree`, `ovt_stream_index`, `ovt_file_display_name`) stays owned by the handle and is valid until that handle's `*_close`; every ABI struct starts with `uint32_t struct_size`; all entry points are `noexcept`; and the API MUST never require the caller to free memory with `free()`. | §12.3.1 rule 3 as decided in D4 (out-parameters are caller-owned and released with `ovt_buffer_free`/`ovt_<type>_release`; nothing is ever freed with `free()`) | Given `test:ApiOwnership` runs under ASan and LeakSanitizer, when it opens, indexes, analyses one frame and closes `corpus/h265/gop.hevc` 100 times, then zero leaks and zero use-after-free reports occur. |
| FR-API-003 | The C API MUST be versioned with `OVT_API_VERSION_MAJOR/MINOR`, exported symbols MUST be limited to `ovt_*` via a version script / `-fvisibility=hidden`, and a minor version increase MUST NOT remove or change the signature of any function. | §12.3.2 (`OVT_API_VERSION*`, version script, ABI policy), WI-1.511 step 6 | Given two consecutive releases, when `abi-compliance-checker` (Linux) compares `libovt-core.so`, then it reports no removed symbols and no changed signatures for a minor bump. |
| FR-API-004 | Cancellation and progress MUST be exposed through `ovt_Job` (`ovt_job_create`, `ovt_job_cancel`, `ovt_job_progress` filling an `ovt_JobProgress` whose `fraction` is 0.0–1.0, `ovt_job_set_progress_callback`), and every API function that can run longer than 100 ms MUST accept a nullable `ovt_Job*`. | §12.3.5 (`ovt_Job`; `ovt_job_set_progress_callback` is the spelling of `ovt_job_set_progress_callback`), WI-1.512 | Given `test:ApiJobCancel`, when `ovt_stream_build_index` on `corpus/perf/1h_1080p_h264.mp4` is cancelled from another thread after 200 ms, then it returns `OVT_E_CANCELLED` within 250 ms and `ovt_job_progress` was observed strictly increasing. |
| FR-API-005 | Core objects MUST document thread-safety per chapter 0 §0.7.2: an `ovt_File` and `ovt_Container` MAY be shared read-only across threads; `ovt_Stream`, `ovt_Analysis`, `ovt_Decoder` MUST be used by one thread at a time; violations MUST be detected in debug builds with an assertion naming the object. | §12.3.1 rule 4 (threading), WI-1.511 | Given `test:ApiThreadMisuse` (debug build), when two threads call `ovt_analysis_frame` on one `ovt_Analysis` concurrently, then the process aborts with the message `ovt_Analysis used from two threads`. |
| FR-API-006 | Logging MUST be routed through `ovt_set_logger`; when no logger is set the library MUST be silent on stdout and stderr. | §12.3.4 (`ovt_set_logger`; default logger is silent until a level is set — see §12.3.4), WI-1.512 | Given `test:ApiSilentDefault`, when the full open/index/analyze sequence runs with no logger set, then captured stdout and stderr are empty. |
| FR-API-007 | The Python package `ovt` (pybind11, `python/`) MUST expose `ovt.open(path) → Stream`, `Stream.index() → StreamIndex`, `Stream.container → ContainerTree`, `Stream.analyze(decode_index, planes=…) → FrameAnalysis`, `Stream.decode(display_index) → Picture` (NumPy views of planes, zero-copy), `ovt.metrics(ref, dist, metrics=[…], **normalisation) → MetricsResult`, `ovt.bdrate(...)`, with FlatBuffer-backed objects offering attribute access and `.to_json()`, and NumPy arrays for every `CellGrid` vector (`grid.qp` shape `(cells_h, cells_w)`); wheels are per interpreter (`cp311`, `cp312`, `cp313`; pybind11 has no stable-ABI build). | §12.4.2 object model (`ovt.open`, `Stream.index/analyze/decode`, `ovt.metrics.run`, `ovt.bdrate`, NumPy `CellGrid` views), WI-4.540–WI-4.542 | Given `pip install ./python`, when `python -c "import ovt, numpy; s=ovt.open('corpus/h265/gop.hevc'); fa=s.analyze(2); assert fa.grid.qp.shape==(fa.grid.cells_h, fa.grid.cells_w); print(fa.to_json()[:1])"` runs, then it prints `{` and exits 0, and `python/tests/test_parity.py` shows `fa.to_json()` equal to `ovt analyze --frames 2 --format json --compact`. |
| FR-API-008 | The Python package MUST ship wheels for macOS arm64/x86_64 and Linux x86_64/arm64 (manylinux_2_28) via `cibuildwheel`, bundling `libovt-core` and the AV1/libvmaf components, with AVC/HEVC decoder plugins loaded from the system `ovt` installation when present (ADR-015). | §12.4.1 (wheels, cibuildwheel, bundled components, codec pack), WI-4.540, chapter 13 §13.7.1 | Given the CI release job, when it finishes, then one wheel per platform and supported interpreter (`cp311`, `cp312`, `cp313`) exists and `pip install ovt-*.whl && python -c "import ovt; print(ovt.__version__)"` succeeds on each platform runner. |
| FR-API-009 | The plugin ABI headers (`core/include/ovt/abi/*.h`) MUST be self-contained (no includes other than `<stdint.h>` and `<stddef.h>`), BSD-2-Clause licensed, and a third-party plugin compiled against them MUST load in `ovt` without any OVT source other than those headers. | §12.3.1 header table (`ovt/abi/*.h`, BSD-2-Clause, self-contained), §12.3.11 loading rules, WI-1.511, WI-2.520 | Given `tests/abi/external_plugin/` (built with only the two ABI headers on the include path), when `ovt plugins --plugin-dir tests/abi/external_plugin/build` runs, then the plugin is listed with its `id`. |
| FR-API-010 | The Python module and every API consumer MUST never evaluate input content as code; `options_json` passed to metric plugins MUST be parsed with a strict JSON parser and rejected on unknown keys. | §12.4.2 "Security", §12.3.13 (`config_json` parsed strictly), WI-4.540 | Given `test:MetricOptionsStrict`, when `create("{\"__import__\":1}")` is called on the `psnr` plugin, then it returns `OVT_E_INVALID_ARG`. |
| FR-CLI-001 | The `ovt` executable MUST implement exactly the subcommands of chapter 12 §12.2.0: `info`, `probe`, `syntax`, `index`, `frames`, `dump`, `extract`, `cache`, `completion`, `render` in phase 1; `analyze`, `plugins`, `stats` in phase 2; `metrics`, `bdrate`, `hdr`, `compare`, `scenes`, `models` in phase 3; `verify`, `doctor`, `hrd` in phase 4 (aliases `decode` = `dump`, `units` = `syntax --list`), with kebab-case flags and `--help` for each subcommand generated from one declarative table per command in `cli/src/cmd/<name>.cpp`. | §12.2.0 (subcommand list and phases; `decode` is an alias of `dump`, `measure` is `hdr --measure`, `--help` per subcommand), WI-1.500 | Given a build, when `ovt --help` runs, then it lists exactly the subcommands of the current phase and `ovt <sub> --help` exits 0 for each. |
| FR-CLI-002 | Every subcommand MUST support `--format {text,json,fb,csv,…}` (chapter 12 §12.1.2; `--format json`, `--text` and `--flatbuffers` are aliases; `--pretty`/`--compact` select JSON layout), and `--output <path>` (`-o`, default stdout), and the JSON MUST validate against the published JSON Schemas in `docs/spec/schemas/*.schema.json`, which are generated from the FlatBuffers schemas by `tools/fbs_to_jsonschema.py`. | §12.1.2 (`--json`, `--compact`, `--flatbuffers`, `-o`), §12.1.4, WI-1.501 (schemas `docs/spec/schemas/*.schema.json` from `tools/fbs_to_jsonschema.py`) | Given `corpus/{h264,h265,av1}/gop.*`, when `ovt index --format json` runs, then `check-jsonschema --schemafile docs/spec/schemas/StreamIndex.schema.json` passes for each output. |
| FR-CLI-003 | Exit codes MUST be those of chapter 12 §12.1.3: 0 OK, 1 ERROR (internal), 2 USAGE, 3 IO (`OVT_E_IO`), 4 UNSUPPORTED (`OVT_E_UNSUPPORTED`), 5 CORRUPT (`OVT_E_CORRUPT`, output still produced when partial results exist and `--partial-ok` is set), 6 PLUGIN, 7 CANCELLED (SIGINT), 8 MISMATCH (verify/compare), 9 PARTIAL; diagnostics MUST go to stderr as JSON Lines when `--format json` is in effect and as human text otherwise. | §12.1.3 (canonical exit-code table, D3; chapter 04 uses the same numbering), §12.1.2 `--log-level`/stderr JSON Lines | Given `corpus/broken/h265_truncated_sps.hevc`, when `ovt syntax --format json --partial-ok <file> > out.json 2> err.jsonl` runs, then `$? == 5`, `out.json` is valid JSON and `err.jsonl` contains a line with `"code":"SYN-H265-0003"`. |
| FR-CLI-004 | `ovt probe <file>` MUST output the `ContainerTree` (with `--tree` for all boxes, default: tracks and top-level boxes), `TrackInfo` list, `ovt_ColorDescription` per track, `bitrate_bps`, duration, frame count and the sniffed `format`, within 300 ms for a 1-hour MP4 (FR-CNT-018). | §12.2.2 `ovt probe` (`--tree` alias, `--tracks`, `--hdr`, `--bitrate`), WI-1.503 | Given `corpus/mp4/hdr10_hevc.mp4`, when `ovt probe --format json` runs, then the output contains `format`, `tracks[0].codec == "H265"`, `tracks[0].color.transfer == 16`, `tracks[0].bitrate_bps` within 1 % of `ffprobe -show_format` `bit_rate`. |
| FR-CLI-005 | `ovt syntax <file>` MUST output the `SyntaxNode` trees of all units (`--unit a-b`, `--frame N`, `--type SPS,PPS,SEI` filters; `--filter "<expr>"` per FR-SYN-015), and `ovt syntax --format text` MUST output an indented text form `name = value (descriptor) [derived] @bit_offset+len` compatible in spirit with `trace_headers`. | §12.2.3 `ovt syntax` (`--unit`/`--units`, `--frame`, `--type`/`--types`, `--filter`, `--text`/`--trace`), WI-1.504 | Given `corpus/h265/basic.hevc`, when `ovt syntax --type SPS --text` runs, then the output contains the line `sps_temporal_mvp_enabled_flag = 1 (u(1)) @…` with the bit offset equal to the JSON `span.bit_offset`. |
| FR-CLI-006 | `ovt index <file>` MUST output the `StreamIndex` (with `--summary` for a one-screen table: codec, profile/level, resolution, frame count, GOP lengths min/avg/max, frame type counts, average bit rate, RAP count) and MUST write/read the on-disk cache under the cache root of chapter 6 §6.7 (D32: `$XDG_CACHE_HOME/ovt/` on Linux, `~/Library/Caches/org.openvideotools.ovt/` on macOS, the app cache dir on Android) keyed by `source_hash` unless `--no-cache`. | §12.2.4 `ovt index` (`--summary`, cache via §12.1.2 `--cache-dir`/`--no-cache`), WI-1.505 | Given `corpus/perf/1h_1080p_h264.mp4`, when `ovt index --summary` runs twice, then the second run reports `cache: hit` and completes in ≤ 200 ms. |
| FR-CLI-007 | `ovt dump <file>` MUST write decoded frames as raw planar YUV (`--raw`), Y4M (`--y4m`), PNG (`--png`, 8-bit tone-mapped as rendered or `--png16`), or per-frame MD5 (`--md5`, hex per plane and combined, matching the HM/libaom `--md5` convention), honouring `--frames a-b`, `--display-order\|--decode-order`, `--sw\|--hw`, `--nocrop`, `--plane-stage <stage>` (phase 2), `--pixel x,y` and `--histogram y\|u\|v`. | §12.2.6 `ovt dump` (alias `ovt decode`; `--raw`, `--y4m`, `--png`, `--png16`, `--md5`, `--frames`, `--order`/`--display-order`/`--decode-order`, `--sw`/`--hw`, `--no-crop`/`--nocrop`, `--stage`/`--plane-stage`, `--pixel`, `--histogram`), WI-1.507, WI-2.521 | Given `corpus/h265/jct-vc/WPP_A_ericsson_MAIN_2.bit`, when `ovt dump --sw --md5 --frames 0-9` runs, then the ten MD5 lines equal the first ten lines of the conformance `.md5` file. |
| FR-CLI-008 | `ovt analyze <file> --frame N \| --frames a-b` MUST output `FrameAnalysis` as `--format json`, `--flatbuffers`, `--format yuview`, `--format mv-csv` or `--format aom-inspect` (subset of libaom `inspect` JSON: `frame`, `blockSize`, `mode`, `skip`, `motionVectors`, `referenceFrame`, `transformSize`, `transformType`, `cdef`, `qindex`), with `--plugin <id>` selection and `--planes <stages>` to dump plane stages as raw files. | §12.2.8 `ovt analyze` (`--frame`, `--frames`/`--range`, `--format yuview|mv-csv|aom-inspect`, `--plugin`, `--planes`), WI-2.521 | Given `corpus/av1/gop.ivf`, when `ovt analyze --frames 0 --format aom-inspect` runs and `aomdec … \| inspect` is run for the same frame, then the `blockSize`, `mode` and `qindex` arrays are equal. |
| FR-CLI-009 | `ovt metrics [--reference R] --distorted D [--distorted D2 …] --metric m1,m2,…` MUST run the metrics for each distorted file against the reference (`--reference` is optional for no-reference metrics such as `uvq`, `cambi` and `si_ti`) with the normalisation options `--scale-kernel`, `--offset`, `--crop`, `--roi`, `--domain`, `--display`, `--fps-policy`, `--chroma-policy`, `--threads`, `--map N\|codec`, `--pool`, `--model`, output `--format json\|csv\|vmaf-json\|fb`, and `--quick` (every 5th frame, `n_subsample` for VMAF). | §12.2.9 `ovt metrics` (canonical flags plus the chapter-04 spellings listed in the alias table of §12.2.9), WI-3.530 | Given `syn_metrics_1080p_pair` (chapter 14 recipe 25; reference `ref.y4m`, distorted `dist.mp4`), when `ovt metrics --reference ref.y4m --distorted dist.mp4 --metric psnr,ssim,vmaf --format csv` runs, then the CSV header is `frame,display_index,psnr_y,psnr_u,psnr_v,ssim_y,ssim_u,ssim_v,vmaf` and the row count equals the aligned frame count. |
| FR-CLI-010 | All numeric output MUST use `.` as decimal separator and no thousands separator regardless of `LANG`/`LC_ALL`, and the CLI MUST set `LC_NUMERIC=C` internally before any formatting. | §12.1.4 (number formatting; `LC_NUMERIC=C`), WI-1.500 step 6 | Given `LC_ALL=de_DE.UTF-8`, when `ovt probe --format json corpus/h264/gop.mp4 \| jq .tracks[0].duration_s` runs, then it prints a number with a `.` decimal point. |
| FR-CLI-011 | The CLI MUST handle SIGINT by cancelling the running `ovt_Job`, flushing partial output when `--partial-ok` is set, and exiting with code 7 (CANCELLED) within 200 ms. | §12.1.3 (signals; code 7 `CANCELLED` per D3), §12.1.2 `--partial-ok`, WI-1.500 step 3, WI-1.512 | Given `ovt index corpus/perf/1h_1080p_h264.mp4 --format json -o out.json`, when SIGINT is sent after 500 ms, then the process exits with 7 within 200 ms and `out.json` is absent (or valid partial JSON with `--partial-ok`). |
| FR-CLI-012 | Reading from stdin (`ovt probe -`, `ovt index -`) MUST be supported for Annex B, OBU and Y4M inputs by spooling to a temporary file when the input is not seekable, and `--raw-format WxH:pix_fmt[:fps]` (chapter 12 §12.1.5, D21) MUST be accepted for headerless YUV from stdin. | §12.1.5 (`-` input, spooling, `--raw-format WxH:pix_fmt[:fps]`), WI-1.500 | Given `cat corpus/h265/basic.hevc \| ovt index - --summary`, when it runs, then the summary equals `ovt index corpus/h265/basic.hevc --summary`. |
| FR-CLI-013 | `ovt extract <file>` MUST implement FR-SYN-018 with the options of chapter 12 §12.2.7: `--units <sel>` (exactly these units), `--frames <sel>` (these frames plus, with `--min-cvs`, default on, the parameter sets and decode chain from the preceding RAP; `--no-min-cvs` copies only the frames' own units), `--raps` (every RAP picture with its parameter sets), `--to annexb\|obu\|ivf` (output framing) and `-o <path>`; the selector grammar is that of chapter 12 §12.1.2 `--frames`. (The earlier spellings `--unit`, `--frame`, `--format` survive only as aliases in chapter 12 §12.2.0.) | §12.2.7 `ovt extract` (`--units`, `--frames`, `--min-cvs`, `--raps`, `--to`), WI-1.508 | Given `corpus/h264/gop.mp4`, when `ovt extract --raps --to annexb -o raps.264` runs, then `ffprobe -show_frames raps.264` lists only `pict_type=I` frames and the count equals the RAP count in `ovt index --summary`. |
| FR-CLI-014 | `ovt verify <file>` MUST run FR-SYN-019 (`--hash-sei`; D21), FR-SYN-020 (`--levels`), plugin cross-check (`--plugin a --against b`; `--plugin-compare a,b` is an alias, chapter 12 §12.2.0) and syntax range checks, MUST output a report JSON with `pass\|fail` per check and all `Diagnostic`s, and MUST exit 0 only when every requested check passes. | §12.2.12 `ovt verify` (`--hash-sei`, `--levels`, `--plugin`+`--against`; aliases `--hash`, `--plugin-compare` in §12.2.0), WI-4.543 | Given `corpus/jctvc_*`, when `ovt verify --hash-sei --levels <file>` runs for each, then every exit code is 0; given `corpus/syn_broken_h265_hash_mismatch`, then exit code is 8 (MISMATCH) and `checks.hash == "fail"`. |
| FR-CLI-015 | The CLI MUST support `--log-level trace\|debug\|info\|warn\|error` and `--log-file`, MUST default to `warn`, and MUST never print to stdout anything other than the requested output so that `ovt … --format json \| jq` always succeeds. | §12.1.2 (`--log-level`, `--log-file`, default `warn`), §12.1.4 (stdout carries only the result), WI-1.500 | Given `ovt index corpus/broken/h265_truncated_sps.hevc --format json --log-level trace 2>/dev/null \| jq .`, when it runs, then `jq` exits 0. |
| FR-CLI-016 | `ovt compare A B` MUST implement the first-mismatch finder (FR-PLN-009, `--first-diff`), a frame-by-frame identity report (`--identity`), and a difference image export (`--diff-image --frame N --gain G -o diff.png`). | §12.2.19 `ovt compare` (`--first-diff`, `--identity`, `--diff-image --frame N --gain G -o`), WI-3.530 | Given `syn_metrics_1080p_pair`'s reference (A) and `syn_metrics_1080p_bitflip_f37` (B, chapter 14 recipe 52), when `ovt compare --identity A B --format json` runs, then `frames[37].differing_samples == [1,0,0]`. |
| FR-CLI-101 | Every capability of `ovt-studio` MUST be available as an `ovt` subcommand that reads the same inputs and emits the same FlatBuffers records as the GUI consumes (chapter 12 §12.2). | §12.2 (whole grammar), chapter 11 feature matrix | Given the feature matrix of chapter 11, when each row is checked against chapter 12 §12.2, then every row names a subcommand and flag (phase-4 audit WI-4.599). |
| FR-CLI-102 | `ovt` MUST emit machine-readable output by default when stdout is not a TTY (`--format json`) and human text when it is (`--format text`). | §12.1.2 `--format`, WI-1.500 step 2 | Given `ovt info a.mp4 \| cat`, when it runs, then stdout parses as JSON; given an interactive terminal, then a human table is printed. |
| FR-CLI-103 | `ovt` MUST support the formats `json`, `fb`, `csv`, `yuview`, `text` (and `aom-inspect`, `mv-csv` for `analyze`, `vmaf-json` for `metrics`); a format that a subcommand does not support MUST produce exit code 2 (USAGE) and a message naming the supported formats. | §12.1.4, §12.2.0 table, WI-1.500 | Given `ovt info a.mp4 --format yuview`, when it runs, then the exit code is 2 and stderr contains `supported: json, fb, text`. |
| FR-CLI-104 | CLI output MUST be deterministic: two runs on the same input with the same flags MUST produce byte-identical stdout (chapter 0 §0.7 rule 5); no timestamps, hostnames, thread ids, absolute cache paths or pointer values may appear in the output. | §12.1.4, WI-1.510 | Given any corpus file, when `ovt syntax f --format fb` runs twice, then `cmp` of the two outputs succeeds. |
| FR-CLI-105 | All logs and progress MUST go to stderr; stdout carries only the result. | §12.1.2 `--progress`, `--log-level`, WI-1.500 step 8 | Given `ovt index f --log-level trace --progress 2>/dev/null`, when it runs, then stdout is valid JSON. |
| FR-CLI-106 | `ovt` MUST accept `-` as an input path meaning stdin for formats that do not require random access (Annex B, OBU, IVF, Y4M); for ISOBMFF input from stdin it MUST spool to a temporary file under `--cache-dir` and continue. | §12.1.5, WI-1.500 | Given `cat a.h264 \| ovt syntax -`, when it runs, then the output equals `ovt syntax a.h264`. |
| FR-CLI-107 | `ovt` MUST be usable without a display server, GPU or Qt (no Qt dependency in `cli/`; `OVT_BUILD_CLI` independent of `OVT_BUILD_GUI`, chapter 13 §13.2.2). | §12.2.16 (`--no-gpu`), chapter 13 §13.2.2 (`OVT_BUILD_CLI` independent of `OVT_BUILD_GUI`), WI-1.500 | Given a Linux container without `libQt6*`, when `ovt --version` runs, then it succeeds. |
| FR-CLI-108 | The same `ovt` binary MUST run on macOS, Linux and (as a test-only binary pushed with `adb`) Android; the Android APK does not ship the CLI. | chapter 13 §13.4, WI-1.500 | Given the `android-arm64` preset, when it is built, then `cli/ovt` exists and `adb shell /data/local/tmp/ovt --version` prints the version. |
| FR-CLI-109 | `ovt --help` and `ovt <sub> --help` MUST print within 50 ms and MUST NOT load any plugin or open any file. | §12.1.2, WI-1.500 notes | Given `time ovt analyze --help`, when it runs on the M1 reference, then wall time is ≤ 50 ms. |
| FR-API-101 | `pip install ovt` on a supported platform MUST import without any system dependency other than libc (glibc ≥ 2.28 on `manylinux_2_28`; libstdc++ linked statically into the extension) or macOS 13. | §12.4.1, WI-4.540 step 8 | Given a fresh `python:3.12` manylinux container, when `pip install ovt && python -c "import ovt; ovt.doctor()"` runs, then it exits 0. |
| FR-API-102 | `fa.grid.*`, `pic.y/u/v` and the `plane()` method of the Python `FrameAnalysis` wrapper MUST be zero-copy (no allocation proportional to the data size). | §12.4.2 "Zero-copy rule", WI-4.541 | Given a 4K frame, when `tracemalloc` wraps `fa.grid.qp`, then the peak allocation is < 4 KiB. |
| FR-API-103 | Every `ovt_status` ≠ `OVT_OK` MUST raise the corresponding `OvtError` subclass with the `Diagnostic` attached (chapter 12 §12.4.2). | §12.4.2 exception classes, WI-4.540 step 3 | Given a truncated file, when `ovt.open(f).video.index()` runs, then `ovt.Corrupt` is raised with `.diagnostic["code"]` set. |
| FR-API-104 | `ovt.metrics.run(...)` with `metrics=["vmaf"]` MUST give pooled values equal to the `vmaf` CLI output within 1e-4. | §12.4.2 metrics, WI-4.542 | Given the Netflix test vectors (chapter 14), when both run, then `abs(diff) < 1e-4` for every frame. |
| FR-API-105 | The Python stubs MUST describe every public symbol and `mypy --strict` on `python/examples/` MUST pass. | §12.4.1 "Typing", WI-4.545 | Given the CI job `python-typecheck`, when it runs on `python/`, then it exits 0. |
| FR-API-106 | `import ovt` MUST take ≤ 300 ms (no plugin scan at import; scan on first use). | §12.4.2 `ovt.plugins`, WI-4.540 | Given `python -X importtime -c "import ovt"`, when it runs on the M1 reference, then the cumulative import time is ≤ 300 ms. |

### 12.1.2 Global options (accepted before or after the subcommand)

| Flag | Type | Default | Description |
|---|---|---|---|
| `--format <f>`, `-f` | enum `json\|fb\|csv\|yuview\|text` plus the subcommand-specific `aom-inspect`, `mv-csv` (`analyze`) and `vmaf-json` (`metrics`) | `json` if stdout is not a TTY, else `text` | Output encoding. See §12.1.4. |
| `--json` | flag | | Alias of `--format json` (D3; chapters 04, 09, 11 and 14 use this spelling). JSON is pretty-printed on a TTY and compact otherwise; `--pretty`/`--compact` override. |
| `--text` | flag | | Alias of `--format text`. |
| `--flatbuffers` | flag | | Alias of `--format fb` (chapter 04 FR-CLI-002 spelling). |
| `--pretty` / `--compact` | flag | by TTY | Force 2-space-indented or single-line JSON regardless of the TTY test. Mutually exclusive (exit 2). |
| `--output <path>`, `-o` | path | `-` (stdout) | Write the result to a file instead of stdout. Required for `fb` when stdout is a TTY (refuses to print binary to a terminal; exit 2). |
| `--cache-dir <dir>` | path | `$OVT_CACHE_DIR` else the platform cache root of chapter 06 §6.7 (D32): `~/Library/Caches/org.openvideotools.ovt/` (macOS), `$XDG_CACHE_HOME/ovt/` (Linux; `~/.cache/ovt/` when unset), `Context.getCacheDir()/ovt/` (Android) | Location of the on-disk index/analysis cache (chapter 06 §6.7). |
| `--no-cache` | flag | off | Neither read nor write the cache. |
| `--plugin-dir <dir>` | path, repeatable | `$OVT_PLUGIN_PATH` (colon-separated) plus built-in search paths (§12.3.11) | Additional directories searched for `libovt_plugin_*.{so,dylib}`. |
| `--threads <n>` | int ≥ 0 | 0 (= hardware concurrency, capped at 16) | Worker threads for `ovt::jobs::Pool`; decoders receive `min(n, 8)`. |
| `--log-level <l>` | enum `trace\|debug\|info\|warn\|error\|fatal\|off` | `warn` | Log threshold for stderr. Maps to `ovt_log_fn` levels 0..5. |
| `--log-file <path>` | path | | Append log lines to this file instead of stderr (same threshold). Diagnostics still go into the result's `diagnostics[]`; with `--format json` the log lines written to the file (or stderr) are JSON Lines `{"level","module","msg"}`, otherwise human text (chapter 04 FR-CLI-003/FR-CLI-015). |
| `--partial-ok` | flag | off | On exit 7 (`CANCELLED`) or 5 (`CORRUPT`) keep the partial `--output` file instead of deleting it; stdout output is always flushed as far as it was produced (§12.1.3). Without the flag a partial `-o` file is removed so that scripts never consume a truncated result silently. |
| `--progress` | flag | off | Emit progress to stderr. If stderr is a TTY: a single rewriting line `[####      ] 42% frames 1200/2850 ETA 00:12`. If not a TTY: one JSON Lines record per ≥ 1 % step `{"progress":0.42,"done":1200,"total":2850,"stage":"index"}`. |
| `--time` | flag | off | Append a `timing` object to the envelope (`--format json`/`fb`) or print it as a `timing:` footer on stderr (`text`/`csv`): `{container_ms, index_ms, analysis_ms, seek_ms, decode_ms_per_frame, first_frame_ms, metrics_fps, wall_ms, cache_hit}`. Each subcommand fills the members it measured and omits the others (`cache_hit` is a boolean; `metrics_fps` = scored frames / scoring wall time). Chapter 04 performance acceptance tests (NFR-001…NFR-006, FR-DEC, FR-VQ) read these members; `dump --time` adds its per-frame members (§12.2.6). (OI-13.) |
| `--quiet`, `-q` | flag | off | Equivalent to `--log-level error`, suppresses the `text` format footer. |
| `--color <m>` | enum `auto\|always\|never` | `auto` | ANSI colour in `text` output. |
| `--version`, `-V` | flag | | Prints `ovt <semver> (core <semver>, api <OVT_API_VERSION>)` and exits 0. With `--format json`: `{"ovt":"1.2.0","core":"1.2.0","api":"1.2.0","build":{"commit":"<sha>","profile":"release","platform":"macos-arm64"},"features":["aom","de265","vmaf","uvq"]}`. |
| `--help`, `-h` | flag | | Help for the tool or the subcommand. |

Precedence: command-line flag > environment variable (`OVT_CACHE_DIR`, `OVT_PLUGIN_PATH`, `OVT_THREADS`, `OVT_LOG_LEVEL`, `OVT_FORMAT`) > built-in default. The alias flags (`--json`, `--text`, `--flatbuffers`, and the per-subcommand aliases listed in §12.2.0) are ordinary CLI11 flags that set the same variable as the canonical option; `--help` lists them under "aliases" and the GUI's "Copy as command" (§12.5) always emits the canonical spelling. `--threads` and `--log-level` MUST be honoured identically by the GUI (chapter 11 settings dialog).

The parser is **CLI11** (vcpkg port `cli11`, BSD-3-Clause). Subcommands and flags are `kebab-case` (§0.4). Unknown flags are an error (exit 2), never ignored. Every flag has a long form; only `-f`, `-o`, `-q`, `-h`, `-V`, `-t` (track), `-n` (frame count), `-r`/`-d`/`-m` (`metrics`) have short forms.

### 12.1.3 Exit codes

| Code | Name | When | Maps from `ovt_status` |
|---|---|---|---|
| 0 | `OK` | Result produced. Diagnostics of severity Info/Warning may be present. | `OVT_OK` |
| 1 | `ERROR` | Unclassified failure (internal error, assertion). A bug; always logs a stack trace at `--log-level debug`. | `OVT_E_INTERNAL`, `OVT_E_NOMEM` |
| 2 | `USAGE` | Bad flag, missing argument, unsupported format for the subcommand, mutually exclusive flags. The message always ends with `See 'ovt <sub> --help'.` | `OVT_E_INVALID_ARG` |
| 3 | `IO` | File not found, permission denied, read error, cannot write `--output`. | `OVT_E_IO` |
| 4 | `UNSUPPORTED` | Format/codec/feature recognised but not implemented (e.g. MXF in phase 1, a plugin missing for the codec). | `OVT_E_UNSUPPORTED` |
| 5 | `CORRUPT` | Input violates its specification badly enough that the requested result is incomplete. Partial output IS still written (as much as could be produced) together with `diagnostics[]`. | `OVT_E_CORRUPT` |
| 6 | `PLUGIN` | A plugin failed to load, has the wrong ABI version, or returned an error. | `OVT_E_PLUGIN` |
| 7 | `CANCELLED` | SIGINT/SIGTERM received, or `--timeout` elapsed. Partial output is written and flushed. | `OVT_E_CANCELLED` |
| 8 | `MISMATCH` | Only `ovt verify` and `ovt metrics --fail-below`: the check ran and found a difference / threshold violation. | n/a |
| 9 | `PARTIAL` | Multi-input commands (`metrics` with several `--distorted`, `bdrate` with several files) where some inputs succeeded and others failed; per-input status is in the output. | n/a |

Rules: codes 3–7 are chosen from the *first* error that aborted the command. Signals: `SIGINT` cancels the current `ovt_Job`, waits ≤ 2 s for workers, writes partial output (to stdout always; to `-o` only with `--partial-ok`, §12.1.2), exits 7 within 200 ms of the signal for index/probe/syntax commands (decoders may need the full 2 s drain); a second `SIGINT` exits immediately with 130. This table is the only exit-code table of the specification (D3): chapter 04 FR-CLI-003, chapter 08 WI-1.235 and chapter 14 use these numbers. Diagnostics of severity `Error` in `diagnostics[]` do not by themselves change the exit code unless the requested result is incomplete (then 5); `--strict` (accepted by every subcommand that can emit diagnostics) turns any `Error`-severity diagnostic into exit 5.

**Diagnostic registry (`CLI-*`, D2; owned by this chapter).** Codes raised by the command-line layer itself (argument parsing, output writing); every other code comes from the library and is passed through unchanged. Each is written to stderr as `error: <code> <message>` and, when an envelope can still be produced, into `diagnostics[]`.

| Code | Severity | Message template | When raised | Exit |
|---|---|---|---|---|
| `CLI-0001` | Error | `unknown option '{opt}' for '{subcommand}'` (or `unknown subcommand '{name}'`) | CLI11 parse failure; the message appends `--help` guidance and the closest spelling from the alias table of §12.2.0 | 2 |
| `CLI-0002` | Error | `format '{fmt}' is not supported by '{subcommand}'` | `--format` value outside the subcommand's set (§12.2.0 table) | 2 |
| `CLI-0003` | Error | `bad selector '{sel}': {reason}` | frame/sample/track selector (§12.1.5 grammar) does not parse, or an index exceeds the stream (`{reason}` = `index 500 > last frame 299`) | 2 |
| `CLI-0004` | Error | `cannot write '{path}': {errno text}` | `-o`/`--planes-dir` target not creatable or not writable | 3 |
| `CLI-0005` | Warning | `partial result: {n} of {m} items failed` | `--partial-ok` set and at least one item failed (§12.1.3 rules) | 9 |
| `CLI-0006` | Error | `options {a} and {b} are mutually exclusive` | e.g. `--frame` with `--all`, `--hull` without `--anchor`, `--json` with `--format csv` | 2 |
| `CLI-0007` | Error | `stdin ('-') may be used for one input only and must be a regular file or pipe` | `-` given twice, or the input needs seeking and stdin is not seekable (§12.1.5) | 2 |
| `CLI-0008` | Error | `plugin '{id}' not found; installed: {list}` | `--plugin <id>` / `--verify-with <id>` names an unloaded plugin (`ovt plugins` lists them) | 6 |
| `CLI-0009` | Error | `input '{path}': {reason}` | file missing, unreadable or not sniffable as any supported format (wraps `IO-*`/`CNT-*` library diagnostics for the top-level message) | 3 or 4 |

### 12.1.4 Output formats

**`json`.** UTF-8, one JSON document, 2-space indentation when stdout is a TTY, compact otherwise (`--pretty` forces indentation). Every document is an *envelope*:

```json
{
  "$schema": "https://openvideotools.org/schema/ovt-cli-1.json",
  "ovt_version": "1.2.0",
  "schema_version": 1,
  "command": "probe",
  "input": { "path": "a.mp4", "size_bytes": 1234567, "source_hash": "<hex sha-256 as in StreamIndex.source_hash>" },
  "result": { },
  "diagnostics": [ { "severity": "Warning", "code": "CNT-2001", "message": "stts.sample_delta == 0 for a non-last sample", "span": {"bit_offset": 0, "bit_length": 0}, "spec_ref": "ISO/IEC 14496-12 §8.6.1.2" } ],
  "status": "OK"
}
```

`input.path` is the path exactly as given on the command line (so output is deterministic for the same invocation). `result` is the JSON mapping of the FlatBuffers root table named by the subcommand (table 12.2.0), produced by OVT's own reflection-based printer (`ovt_fb_to_json`, §12.3.8; chapter 06 §6.8.1 is the normative field-for-field rule set). The printer follows `flatc --json` where flatc has a rule and adds the rules flatc lacks: field names as in the schema (`snake_case`), enums as their *names* (`"PredKind": "Inter"` → `"pred": "Inter"`), structs as objects, vectors as arrays, absent fields omitted, `ulong`/`long` as JSON numbers when |v| ≤ 2^53 and as decimal **strings** otherwise (flatc prints them as numbers, which loses precision in JavaScript consumers), `[ubyte]` vectors as **base64 strings** (flatc has no base64 mode; `flatc --json` would print byte arrays) **except** `CellGrid` vectors, which are arrays of numbers, and `float`/`double` printed with the shortest round-trip representation (`std::to_chars`), `NaN`/`±Inf` as the strings `"nan"`, `"inf"`, `"-inf"`. All numbers use `.` as decimal separator and no thousands separator regardless of `LANG`/`LC_ALL`; the CLI calls `setlocale(LC_NUMERIC, "C")` before any formatting and the printer never uses locale-dependent functions (chapter 04 FR-CLI-010). The nested FlatBuffers in `Block.ext` and `FrameAnalysis.filter_params` are decoded in place into `ext_decoded` / `filter_params_decoded` objects, keyed by `ext_type`. JSON Schema files for the envelope and for every root table are generated at build time by `tools/fbs_to_jsonschema.py` from the `.fbs` files into `docs/spec/schemas/<Table>.schema.json` (chapter 04 FR-CLI-002; WI-1.501) and the `json` output of every subcommand MUST validate against them.

**JSON compatibility policy (normative; the CI-consumer guarantee).** `schema_version` in the envelope is an integer that is bumped only on a breaking change of the envelope or of a root table's JSON mapping. Within one `schema_version` changes are **additive only**: new fields, new enum names and new diagnostic codes may appear; existing field names, types, units, enum names, the envelope keys and the exit-code table never change or disappear. A consumer MUST ignore unknown fields. A breaking change bumps `schema_version`, bumps `OVT_API_VERSION_MAJOR` (§12.3.2) in the same release, is listed in `CHANGELOG.md` under `changed`, and the previous layout stays readable by `ovt cache import` (§12.2.13) for one major version. The generated JSON Schemas (`docs/spec/schemas/`, WI-1.501) are versioned by the same integer (`ovt-cli-<schema_version>.schema.json`) and `tools/check_schema_compat.py` (chapter 13 WI-0.003) fails a PR that removes or retypes a field without a bump. This paragraph is the policy chapter 17 OI-14 referred to; it is owned here.

**`fb`.** Binary FlatBuffers. Framing: the output is an **OVT FlatBuffer stream** = the 8-byte magic `OVTFBS\x01\x00` followed by zero or more *size-prefixed* FlatBuffers (`flatbuffers::FinishSizePrefixed`), each carrying its 4-byte `file_identifier`. Identifiers are fixed in the schemas (chapter 06 §6.1.1 is the authoritative list): `ContainerTree` = `OVTC`, `StreamIndex` = `OVTI`, `SyntaxNode` (root for `syntax`) = `OVTS`, `FrameAnalysis` = `OVTA`, `MetricsReport` = `OVTR`, `PluginList` = `OVTP`, `CliEnvelope` = `OVTE`. `MetricsResult` is not a root table: `ovt metrics` emits one `MetricsReport` record (its `results[]` hold the per-distorted × metric × model `MetricsResult`s). The first record of every stream is a `CliEnvelope` (schema below) and the remaining records are the results; a consumer reads the identifier to know the root type. Planes (`ovt dump`) are never inside the fb stream; they go to a separate file.

The envelope table is `ovt.fb.CliEnvelope` (`core/schema/ovt_model.fbs`, chapter 06 §6.3 "CLI / API report tables", `file_identifier "OVTE"`): fields `ovt_version`, `schema_version`, `command`, `input_path`, `input_size`, `source_hash`, `record_count` (number of result records that follow), `diagnostics`, `status`. Chapter 06 is the only place the schema text lives; this chapter never redefines tables.

**`csv`.** RFC 4180, comma separator, `\n` line ending, header row first, UTF-8, no BOM, numbers with `.` decimal separator and at most 6 fractional digits, booleans as `0`/`1`, enums as names. Column sets are fixed per subcommand (§12.2). A `csv` request on a tree-shaped result (`probe`, `syntax`) flattens the tree with the columns `depth,path,name,descriptor,value,bit_offset,bit_length,spec_ref` where `path` is `/`-joined names with `[i]` for array elements.

**`yuview`.** YUView statistics CSV ("Statistics Extension Syntax", semicolon-separated, `%;syntax-version;v1.2` header). Exact mapping of `CellGrid`/`Block`/`MotionVector` to YUView types is normative in chapter 06 §6.8.2; `analyze --format yuview` MUST produce a file that YUView 2.14 opens over the matching decoded YUV without warnings.

**`vmaf-json`.** Only for `metrics`: the libvmaf `--json` layout (chapter 09 §9.9 key mapping; also available as a side file through `--vmaf-json <path>`, §12.2.9).

**`mv-csv`.** Only for `analyze`: one row per `MotionVector` (`--csv-rows mv`, §12.2.8).

**`aom-inspect`.** Only for `analyze` on AV1: the JSON emitted by libaom `examples/inspect.c` (per-frame object with `frame`, `frameType`, `baseQIndex`, `tileCols`, `tileRows`, and the `--combined`-style arrays `blockSize`, `transformSize`, `transformType`, `mode`, `uv_mode`, `skip`, `filter`, `cdef_level`, `cdef_strength`, `motionVectors`, `referenceFrame`, `segment_id`, `qindex`), one object per line. Fields OVT cannot derive for other codecs are omitted (`aom-inspect` for H.264/HEVC is exit 2).

**`text`.** Human-readable; never parsed by tools; may change between versions. Tables use fixed-width columns; sizes use SI suffixes with one decimal (`12.3 MB`); times as `HH:MM:SS.mmm`.

### 12.1.5 Input selection conventions (shared by all subcommands)

| Flag | Type | Default | Description |
|---|---|---|---|
| `<input>` | positional path or `-` | required | Container or elementary stream. Format sniffed by `ovt::container` (chapter 08); `--input-format <isobmff\|ivf\|annexb\|obu\|y4m\|raw>` overrides sniffing. |
| `--track <n>`, `-t` | int | first video track | Container track index as listed by `ovt info`. For elementary streams only `0` is valid. |
| `--codec <c>` | enum `h264\|h265\|av1` | from container / sniffed | Force the codec for Annex B / raw inputs whose sniffing is ambiguous. |
| `--raw-format <WxH:pix_fmt[:fps_num/fps_den]>` | string | | Geometry for headerless `.yuv` inputs, e.g. `1920x1080:yuv420p10le:30000/1001` (D21; the bare `--raw` is the `dump` output shorthand of §12.2.6 and is **not** accepted as an input-geometry flag). `pix_fmt` is an FFmpeg pixel-format name (`gray`, `gray10le`, `yuv420p`, `yuv420p10le`, `yuv422p`, `yuv444p12le`, …; chapter 08 §8.6.6 lists the accepted set); the frame rate part is optional and defaults to `25/1` (`--raw-fps <num/den>` overrides it). The same string is the `raw_format` argument of `ovt_container_open_format` (§12.3.7) and the `raw=` argument of `ovt.open` (§12.4.2); chapter 04 FR-CLI-012 and FR-CNT-014, chapter 08 §8.6.6 and chapter 14 use this flag with the shortened spelling `--raw-format WxH:fmt`. |
| `--raw-candidates` | flag | off | For a `.yuv` input without `--raw-format`: print the candidate geometries chapter 08 §8.6.6 proposes from the file size (as `diagnostics[]` of code `CNT-RAW-0002`) and exit 4 instead of guessing. |
| `--frames <sel>` | selector | all | Frame selection: `N`, `A-B` (inclusive), `A-` (open-ended), `A-B/S` (step), `A,B,C`, `rap` (RAPs only), `every:N`, `all`; indices are **display indices** unless `--order decode`. Multiple selectors are unioned. This **selector grammar** is the only one in the CLI: `--units`, `--samples`, `--range`, `--boxes` and every other range-valued option use it. |
| `--order <o>` | enum `display\|decode` | `display` | Interpretation of frame indices in flags and ordering of per-frame output. |
| `--timeout <s>` | float seconds | 0 (none) | Cancel the job after this wall time; exit 7. |

---

## 12.2 The `ovt` command-line grammar

### 12.2.0 Overview

```
ovt [global options] <subcommand> [subcommand options] [<input> ...]

Subcommands (phase in brackets):
  info        (1) one-screen summary of a file: format, tracks, codec, duration, HDR
  probe       (1) container tree (boxes/fields with byte offsets), tracks, samples, fragments, HDR metadata
  syntax      (1) NAL/OBU unit list and header syntax tree with bit offsets; --trace text
  index       (1) build/print the StreamIndex (frames, RAPs, sizes, refs); warms the cache
  frames      (1) per-frame table (type, size, QP, POC, pts, refs)
  dump        (1) decoded pictures per stage as YUV/Y4M; MD5
  extract     (1) write unit/sample/frame ranges to a new elementary stream or Y4M
  analyze     (2) block-level FrameAnalysis export (fb/json/yuview/aom-inspect)
  plugins     (2) list analysis/metric plugins with capabilities and licences
  metrics     (3) full/no-reference quality metrics with normalisation and pooling
  bdrate      (3) Bjøntegaard-delta between RD curves
  hdr         (3) HDR metadata and per-frame light-level measurements
  verify      (4) decoded-picture-hash SEI check; plugin-vs-oracle picture compare
  doctor      (4) environment report: plugins, HW decoders, GPU/HDR, cache
  cache       (1) list / clear / stats / import of the on-disk cache
  completion  (1) shell completion scripts (bash, zsh, fish)
  render      (1) offscreen render of one frame to PNG (SDR/HDR targets, overlays)   §12.2.18
  compare     (3) pixel-level A/B comparison: first difference, identity, diff image §12.2.19
  stats       (2) aggregate block statistics / coding-tool usage over a range        §12.2.20
  hrd         (4) HRD / decoder-model buffer trace                                    §12.2.21
  scenes      (3) shot-boundary detection -> SceneSegment list                        §12.2.22
  models      (3) list / fetch / verify downloadable metric models                    §12.2.23

Aliases accepted for compatibility with other chapters (same grammar, same output): `decode` = `dump` (chapter 04 FR-CLI-007),
`units` = `syntax --list` (WI-1.504; chapter 07 WI-1.128 is superseded, chapter 16 §16.3). `hdr-scan`, `measure`, `render-frame`, `validate`,
`import` and `fbdiff` are NOT subcommands (use `hdr --measure`, `render`, `index --validate` / `analyze --validate`, `cache import`, `dump --diff`; D3).
```

**Alias spellings.** Chapters 04, 07, 09 and 14 use older spellings for some options. The following aliases are accepted by the parser as flags that set the same variable as the canonical option; everything else about them (type, default, output) is the canonical option's. New text MUST use the canonical spelling.

| Alias (where) | Canonical |
|---|---|
| `--json`, `--text`, `--flatbuffers` (global) | `--format json`, `--format text`, `--format fb` (§12.1.2) |
| `--tree` (`probe`) | `--boxes --expand all` |
| `--units <sel>`, `--types <list>`, `--text` (`syntax`) | `--unit <sel>`, `--type <list>` (comma list = repeated flag), `--format text` |
| `--nocrop`, `--display-order`, `--decode-order`, `--plane-stage <s>` (`dump`/`decode`) | `--no-crop`, `--order display`, `--order decode`, `--stage <s>` |
| `--unit <sel>`, `--format <annexb\|obu\|ivf>` (`extract`) | `--units <sel>`, `--to <fmt>` |
| `--frames <sel>`, `--grid <list>`, `--include-hidden` (`analyze`) | `--range <sel>`, `--fields <list>`, `--order decode` |
| `--blocks`, `--mvs`, `--symbols`, `--ext` (`analyze`) | accepted no-ops (these parts are included by default; `--no-blocks` etc. drop them) |
| `--ref`, `--dist`, `--metrics <list>`, `--vmaf-model <m>`, `--no-align`, `--no-scale`, `--offset <n>`, `--quick`, `--map <n>`, `--display <m>`, `--interp <i>` (`metrics`, `bdrate`) | see the alias table of §12.2.9 and §12.2.10 |
| `--hash`, `--picture-hash`, `--plugin-compare <a>,<b>` (`verify`) | `--hash-sei`, `--hash-sei`, `--plugin <a> --against <b>` |
| `--format yuview-csv` (older spelling, accepted as an alias) | `--format yuview` |
| `--raw <WxH:pix_fmt[:fps]>` (older spelling of the input geometry) | **not an alias**: `--raw` is the `dump` output shorthand (§12.2.6); the input geometry is `--raw-format <WxH:pix_fmt[:fps]>` (§12.1.5). A `--raw` followed by a geometry string is rejected with exit 2 and the hint `use --raw-format`. |

| Subcommand | Root FlatBuffers table(s) in `fb`/`json` | `csv` | `yuview` | `text` |
|---|---|---|---|---|
| `info` | `ContainerTree` (tracks only, `root` empty) + `StreamIndex` summary (`frames` empty) | no | no | yes |
| `probe` | `ContainerTree` | flattened | no | yes |
| `syntax` | `StreamIndex` with `units` (headers) or one `SyntaxNode` with `--unit` | flattened | no | yes (`--trace`) |
| `index` | `StreamIndex` | no | no | yes (summary) |
| `frames` | `StreamIndex.frames` as `[FrameInfo]` records | yes | no | yes |
| `dump` | `CliEnvelope` + `DumpManifest` | yes (md5 table) | no | yes |
| `extract` | `CliEnvelope` + `ExtractManifest` | no | no | yes |
| `analyze` | `[FrameAnalysis]` | per-cell | yes | yes (summary) |
| `plugins` | `PluginList` | yes | no | yes |
| `metrics` | `MetricsReport` (`results:[MetricsResult]`) | per-frame | no | yes |
| `bdrate` | `BdRateResult` | yes | no | yes |
| `hdr` | `HdrReport` | per-frame | no | yes |
| `verify` | `VerifyReport` | per-frame | no | yes |
| `doctor` | `DoctorReport` | no | no | yes |
| `cache` | `CacheReport` (`import`: `CliEnvelope` only) | yes | no | yes |
| `render` | `DumpManifest` (one `DumpOutput`, `container = "png"`) | no | no | yes |
| `compare` | JSON object (§12.2.19; no `fb`) | `--identity` only | no | yes |
| `stats` | JSON object (§12.2.20; no `fb`) | yes | no | yes |
| `hrd` | JSON object (§12.2.21; no `fb`) | yes | no | yes |
| `scenes` | `[SceneSegment]` records | yes | no | yes |
| `models` | JSON list (§12.2.23; no `fb`) | yes | no | yes |

`CliEnvelope`, `DumpManifest`, `ExtractManifest`, `PluginList`, `HdrReport`, `VerifyReport`, `DoctorReport`, `CacheReport` (and their nested tables) are defined in chapter 06 §6.3 (`core/schema/ovt_model.fbs`, block "CLI / API report tables"); `BdRateResult` and `RdPoint` are defined in chapter 06 §6.5 (`ovt_metrics.fbs`). §12.2.17 is only an index into those definitions. There is no separate `cli/schema/ovt_cli.fbs`.

### 12.2.1 `ovt info`

**Purpose.** The "what is this file" answer in one screen. Reads container headers and the first parameter sets only; never decodes pixels; MUST finish in ≤ 200 ms for any file on an M1 (NFR). Does not build the index unless `--full`.

| Flag | Type | Default | Description |
|---|---|---|---|
| `--full` | flag | off | Also build the StreamIndex (uses cache) to report exact frame counts, GOP statistics and per-type sizes. |
| `--all-tracks` | flag | off | Include non-video tracks (audio, subtitles, metadata) in `tracks`. |

**Output (`result`).**

```json
"result": {
  "format": "isobmff", "brands": ["isom","iso6","mp41"], "duration_s": 60.027, "size_bytes": 23456789,
  "fragmented": true, "encrypted": false, "fast_start": true,
  "tracks": [ { "index": 0, "handler": "vide", "codec": "H265", "codec_string": "hvc1.2.4.L153.B0",
                "coded_size": {"x":0,"y":0,"w":3840,"h":2160}, "crop": {"x":0,"y":0,"w":3840,"h":2160},
                "chroma_format": 1, "bit_depth_luma": 10, "bit_depth_chroma": 10, "fps": "60000/1001",
                "sample_count": 3600, "bitrate_bps": 12345678, "language": "und",
                "color": { "primaries": 9, "transfer": 16, "matrix": 9, "full_range": 0, "max_cll": 1000, "max_fall": 400,
                           "mdcv_max_luminance": 10000000, "has_hdr10plus": false, "has_dolby_vision": false } } ],
  "index_summary": { "frames": 3600, "raps": 60, "gop_len_max": 60, "frame_types": {"I":60,"P":1200,"B":2340},
                     "avg_frame_bytes": {"I": 240000, "P": 60000, "B": 20000} }
}
```

`tracks[].color` is the JSON image of `ovt_ColorDescription` (§0.5.6); `index_summary` present only with `--full`.

**Examples.**

```
ovt info bbb_4k_hdr.mp4                          # text table on a terminal
ovt info --full --format json clip.mp4 | jq .result.index_summary
ovt info --all-tracks movie.mp4 -f csv            # exit 2: csv unsupported; use probe --tracks -f csv
```

### 12.2.2 `ovt probe`

**Purpose.** Container inspection (core capability 5). Emits the `ContainerTree` (§0.5.5) with every box, every decoded field and its `BitSpan`, plus the derived views of chapter 08 (`TrackInfo`, `SampleTable`, `EditList`, `FragmentTimeline`, `EncryptionInfo`, `HdrMetadata`). Lazy expansion (`ovt_container_expand`) is driven by `--expand`.

| Flag | Type | Default | Description |
|---|---|---|---|
| `--expand <sel>` | `all\|none\|<depth N>\|<fourcc list>` | `all` | Which boxes to expand into fields and children. `all` expands everything except `mdat` payloads (never read). `2` expands two levels. `moov,trak,stsd` expands only those box types (and their ancestors). |
| `--boxes` | flag | on unless another view flag is given | Include the box tree (`root`). |
| `--tracks` | flag | off | Include `tracks[]` (`TrackInfo`). With `-f csv`: one row per track, columns `index,handler,codec,codec_string,coded_w,coded_h,crop_w,crop_h,chroma_format,bit_depth_luma,bit_depth_chroma,fps,sample_count,bitrate_bps,language,encrypted`. |
| `--samples [<sel>]` | optional frame selector | off | Include the sample table for `--track`: `index,offset,size,dts,pts,duration,sync,chunk,sample_group_flags`. Default selector `all`; large tables (> 100k rows) print a warning on stderr. |
| `--fragments` | flag | off | Include `FragmentTimeline` (one row per `moof`: sequence number, base data offset, earliest pts, duration, sample count, byte range). With `-f csv`: `sequence_number,moof_offset,moof_size,base_data_offset,earliest_pts,duration,sample_count,data_offset,data_size`. |
| `--hdr` | flag | off | Include `HdrMetadata` derived from `colr`, `mdcv`, `clli`, `dvcC/dvvC`, `st2086`-type boxes plus the SPS VUI / sequence header colour fields, and a `conflicts[]` list where container and bitstream disagree. |
| `--edits` | flag | off | Include `EditList` with the resolved presentation timeline. |
| `--enc` | flag | off | Include `EncryptionInfo` (`pssh` systems, `tenc` default KID/IV size, `senc` per-sample subsample map; never attempts decryption). |
| `--offset <n>` | int | | Print only the box whose `file_span.offset == n` (and its subtree). Used by the GUI's "copy as command". |
| `--raw-fields` | flag | off | Add `payload_span` hex (first 64 bytes, base64 in JSON) for undecoded boxes. |
| `--brands` | flag | off | Include `brands` (`ftyp`/`styp` major and compatible brands) and the brand-rule findings of chapter 08 §8.3.1 as `diagnostics[]`; with `-f csv`: one row per brand (`position,brand,known`). |
| `--bitrate <window>` | duration (`100ms`, `1s`) | | Add `bitrate[]`: one `{t_ms, bps}` row per window over the track's sample sizes and decode times (chapter 04 FR-CNT-004 `ovt probe --bitrate 100ms`); needs `--track`. |
| `--bytes <offset>:<len>` | `uint64:uint32` | | Print `len` bytes at `offset` as a hex dump (text) / base64 (json) through the container byte service (`ovt_container_bytes`, §12.3.7); `len` ≤ 1 MiB. Used by the GUI hex view's "copy as command". |
| `--demux-check` | flag | off | Also open the file through the libavformat demuxer (chapter 08 §8.7) and report every sample whose offset/size/pts differs from the sample table as `CNT-0901` diagnostics (chapter 08 §8.7.6). |
| `--list-diagnostics` | flag | off | Print the container diagnostic registry (chapter 08 §8.5: code, severity, message template, spec reference) as the result and exit 0; no input file is required. |
| `--validate` | flag | off | Run the chapter 08 validation rules and populate `diagnostics[]` (exit 5 on Error severity). |
| `--validate-level <l>` | enum `quick\|standard\|deep` | `standard` | Depth of `--validate`: `quick` = structural rules only (chapter 08 §8.5 group 0/1), `standard` = all rules that need no sample reads, `deep` = also reads the first and last sample of every chunk and every `sidx` target (chapter 08 WI-1.224). |
| `--representation <n>` | int | first video | Phase 4, DASH/HLS manifests only (`ovt_container_open_manifest`, chapter 08 §8.6.7): which Representation/variant to open. |
| `--tr101290` | flag | off | Phase 4, MPEG-TS only: run the ETSI TR 101 290 priority-1/2 checks and add `ts_report` (chapter 08 WI-4.207). |

**Output.** `result` = `ContainerTree`. In `text`, the tree is printed with indentation, `fourcc [offset+size] version/flags` per line and `  name = value (bits o..o+l)` per field, e.g.

```
ftyp   @0 +24
  major_brand = "isom" (bits 64..96, ISO/IEC 14496-12 §4.3)
  minor_version = 512 (bits 96..128)
  compatible_brands[0] = "isom" …
moov   @24 +34567
  mvhd   @32 +108  v1 f=0
    creation_time = 3786096823 ("2023-12-25T10:13:43Z")
```

**Examples.**

```
ovt probe clip.mp4                                   # full tree, text or json
ovt probe clip.mp4 --expand moov,trak,stsd,hvcC --format json | jq '.result.root[]'
ovt probe frag.cmfv --fragments --samples 0-99 -t 0 -f csv -o samples.csv
ovt probe hdr.mp4 --hdr --validate --validate-level deep
ovt probe --list-diagnostics -f csv
ovt probe clip.mp4 --bytes 0:64
```

### 12.2.3 `ovt syntax`

**Purpose.** Bitstream syntax inspection (core capability 1). Lists units (NAL/OBU, §0.5.3 `Unit`) and prints the parsed header `SyntaxNode` tree with bit spans, or an FFmpeg-`trace_headers`-like text.

| Flag | Type | Default | Description |
|---|---|---|---|
| `--unit <n>` | int, repeatable | | Print only `Unit.index == n` (full `SyntaxNode` tree as the root record). |
| `--frame <n>` | int, repeatable | | Print all units of frame `n` (display index; `--order decode` for decode index). |
| `--type <t>` | string, repeatable | | Filter by `Unit.type_name` (e.g. `SPS`, `PPS`, `IDR_W_RADL`, `SEI_PREFIX`, `OBU_SEQUENCE_HEADER`, `OBU_FRAME`) or by number (`--type 33`). Case-insensitive. |
| `--list` | flag | default when no `--unit/--frame/--type` | Unit list only: `index,type,type_name,layer_id,temporal_id,offset,length,prefix_length,frame_decode_index,sample_index`; `header` omitted. |
| `--depth <n>` | int | unlimited | Limit the syntax tree depth. |
| `--hex` | flag | off | Attach `raw_hex` (the unit's `file_span` bytes) and `rbsp_hex` (EPB removed) to every printed unit (base64 in JSON, hex dump in text). Also prints `epb_positions`. |
| `--trace` | flag | off | Text mode: emit the trace format of §12.2.3.1 (implies `--format text`). |
| `--sei` | flag | off | Shortcut for `--type SEI_PREFIX --type SEI_SUFFIX --type OBU_METADATA` with decoded SEI payloads (chapter 07 lists the decoded SEI types: pic_timing, buffering_period, decoded_picture_hash, mastering_display_colour_volume, content_light_level_info, user_data_unregistered, HDR10+ ITU-T T.35, alternative_transfer_characteristics). |
| `--parameter-sets` | flag | off | Shortcut: all VPS/SPS/PPS/sequence headers (`StreamIndex.parameter_sets`). |
| `--diagnostics-only` | flag | off | Print only units that carry `Diagnostic`s. |
| `--filter <expr>` | string | | Keep only units whose header tree satisfies the expression (chapter 04 FR-SYN-015; chapter 07 §7.4 defines the grammar: `name op literal` with `op` ∈ `== != < <= > >=`, `&&`, `\|\|`, `!`, parentheses; names resolve against the unit's `SyntaxNode` tree, e.g. `--filter "slice_qp_delta > 10"`). |
| `--fields <list>` | comma list of `name,descriptor,value,enum_name,span,spec_ref,children,diagnostics` | all | Restrict the `SyntaxNode` members printed per node (smaller JSON for oracle diffs, chapter 14 §14.4). |
| `--all-units` | flag | off | Print the full header tree of every unit (`--list` off, no type filter), i.e. the whole `units[]` with `header` populated. |

**Output.** `result` = `StreamIndex` with `frames` empty and `units` filtered, or a single `SyntaxNode` with `--unit`. `csv` flattens as in §12.1.4.

**Alias.** `ovt units <file> [--track N]` (WI-1.504; chapter 7 WI-1.128 is superseded) is accepted and is exactly `ovt syntax <file> --list`; `--format table` there means `--format text`.

**12.2.3.1 Trace text format (`--trace`).** One line per syntax element, modelled on FFmpeg's `-bsf trace_headers` so that existing diff scripts work:

```
[ovt] Unit 12  type 34 (PPS)  offset 0x000003F2  length 7  temporal_id 0
[ovt] ---- Picture Parameter Set ----  (H.265 7.3.2.3.1)
[ovt]        0  pps_pic_parameter_set_id                           1 = 0
[ovt]        1  pps_seq_parameter_set_id                           1 = 0
[ovt]        2  dependent_slice_segments_enabled_flag              0 = 0
[ovt]        3  output_flag_present_flag                           0 = 0
[ovt]        4  num_extra_slice_header_bits                      000 = 0
```

Columns: `[ovt]`, bit offset (6 wide, right-aligned, decimal, from RBSP start), name (padded to 50), bit string as read (right-aligned to 16; for `ue(v)`/`se(v)` the full code word; for bytes `--`), `=`, value, and when `enum_name` or `derived` is set: ` (PART_2NxN)` / ` -> CtbSizeY = 64`. Arrays print `name[i]`. Diagnostics print as `[ovt] !! WARN SYN-H265-0007 ...` immediately after the offending element. Chapter 14 uses this format as the diff target against FFmpeg `trace_headers` output (after a normalising `sed` listed there).

**Examples.**

```
ovt syntax clip.h265 --list                       # unit table
ovt syntax clip.mp4 -t 0 --type SPS --type PPS --format json
ovt syntax clip.obu --unit 7 --hex                # one OBU with hex + tree
ovt syntax clip.264 --frame 0 --trace > frame0.trace
ovt syntax clip.hevc --filter "slice_qp_delta > 10" --json
```

### 12.2.4 `ovt index`

**Purpose.** Build the `StreamIndex` (§0.5.3) for a track, store it in the cache, print it or a summary. Header-only pass; MUST meet the §0.7 budget (1-hour 1080p H.264 ≤ 2 s on an M1).

| Flag | Type | Default | Description |
|---|---|---|---|
| `--rebuild` | flag | off | Ignore a cached index and rebuild. |
| `--summary` | flag | off (`text` default prints summary) | Print only counts: frames, RAPs, GOP lengths (min/avg/max), frame-type histogram, bytes per type, temporal layers, diagnostics count. |
| `--with-units` | flag | on for `fb`, off for `json` | Include `units[]` (can be millions of entries; JSON gets big). |
| `--with-headers` | flag | off | Include `Unit.header` trees inside `units[]`. |
| `--gop` | flag | off | Add `gops[]` derived view: one record per RAP-to-RAP span (`start_decode_index,length,bytes,types`). |
| `--refgraph` | flag | off | Add `refgraph[]`: one `{decode_index, refs:[decode_index]}` edge list per frame built from `FrameInfo.declared_refs` (chapter 04 FR-SYN-017 `ovt index --refgraph --json`); `text` prints a DOT digraph. |
| `--validate` | flag | off | Run the `StreamIndex` validation rules of chapter 06 §6.9 (`validate_stream_index`, WI-0.086) on the built or cached index and append every violation to `diagnostics[]` as `{code:"V-nnn", severity, message, path}`. Exit 0 when no violation, **5** (`CORRUPT`) when at least one Error-severity violation is reported (the index is still written), 3 for unreadable input (§12.1.3). `text` prints one line per violation. |

**Output.** `result` = `StreamIndex` (filtered per flags). `text --summary` prints:

```
codec H265  frames 3600 (I 60 / P 1200 / B 2340)  shown 3600  RAPs 60  GOP 60/60/60
bytes  I 14.4 MB (26%)  P 72.0 MB (54%)  B 46.8 MB (20%)  total 133.2 MB  avg 37.0 kB/frame
temporal layers 3  max temporal_id 2  fields no  diagnostics 0
```

**Examples.**

```
ovt index clip.mp4 --summary
ovt index clip.mp4 --rebuild --format fb -o clip.index.fb
ovt index clip.ivf --gop -f json | jq '.result.gops | length'
ovt index clip.mp4 --validate -f json | jq '.diagnostics[] | select(.code|startswith("V-"))'
```

### 12.2.5 `ovt frames`

**Purpose.** The per-frame table shown in the GUI timeline, as CSV/JSON. Reads the index (builds it if absent).

| Flag | Type | Default | Description |
|---|---|---|---|
| `--columns <list>` | comma list | all | Subset and order of columns (names below). |
| `--stats` | flag | off | Append running statistics columns: `cum_bytes`, `bitrate_window_bps` (sliding 1 s window by pts). |
| `--refs` | flag | off | Include `declared_refs` and `dpb_after` (JSON arrays; in CSV `|`-joined). |

**Columns (CSV header, in this order).** `display_index,decode_index,type,shown,is_rap,structure,temporal_id,layer_id,poc,pts,dts,time_s,size_bytes,unit_count,qp_base,coded_w,coded_h,crop_w,crop_h,bit_depth,chroma_format,first_unit_offset`. With `--refs`: `declared_refs,dpb_after`. With `--stats`: `cum_bytes,bitrate_window_bps`.

**Output.** `fb`/`json`: `[FrameInfo]` records (one per selected frame) in `--order`. Hidden AV1 frames (`shown=false`) have `display_index=-1` and are listed only with `--order decode`.

**Examples.**

```
ovt frames clip.mp4 -f csv -o frames.csv
ovt frames clip.mp4 --frames 0-299 --columns display_index,type,size_bytes,qp_base --stats
ovt frames clip.obu --order decode --refs -f json
```

### 12.2.6 `ovt dump`

**Purpose.** Decoded pictures to disk. Stage `recon` uses the playback path (ADR-009, software decoder forced for determinism unless `--hw`); every other stage requires an analysis plugin supporting that `PlaneStage`.

| Flag | Type | Default | Description |
|---|---|---|---|
| `--stage <s>` | enum `recon\|pred\|residual\|predeblock\|presao\|precdef\|prelr\|coeffs`, repeatable | `recon` | Which `PlaneStage` to dump (`presao`/`precdef` both map to `PreSaoOrCdef`; `prelr` = `PreLoopRestoration`; `coeffs` = `Coefficients`, written as signed 16/32-bit). One output file per stage; `-o` is then a pattern with `{stage}`. |
| `--plugin <id>` | string | default for codec | Analysis plugin id (see `ovt plugins`) for non-`recon` stages. |
| `--container <c>` | enum `y4m\|yuv` | `y4m` | Y4M writes a header (`YUV4MPEG2 W H F C420p10 ...`); `yuv` is headerless planar. |
| `--bitdepth <n>` | int 8..16 or `native` | `native` | Output sample depth. Promotion is left shift; reduction is rounding right shift (no dithering). `coeffs` ignores it. |
| `--order <o>` | `display\|decode` | `display` | Output order. `decode` includes AV1 hidden frames. |
| `--crop` / `--no-crop` | flag | `--crop` | Apply the conformance window / render size. |
| `--md5` | flag | off | Compute per-frame MD5 over the cropped planes (Y, U, V concatenated, little-endian 16-bit for > 8 bit) exactly as `ffmpeg -f framemd5` does; written to `--md5-file` (default `<output>.md5`) as `display_index,decode_index,md5` and to the manifest. |
| `--hw` | flag | off | Allow hardware decoding for `recon` (output may then differ between devices; a warning is logged). |
| `--plane <p>` | enum `y\|u\|v\|all` | `all` | Write only one plane (headerless `yuv` only). |
| `--sw` | flag | default | Force the software playback decoder (explicit spelling of the default; mutually exclusive with `--hw`). |
| `--seek <n>` | int | | Shorthand for `--frames n-n` plus `--time` (chapter 4 FR-DEC acceptance tests); with `--frames` it sets the start of the range instead. |
| `--raw`, `--y4m`, `--png`, `--png16` | flag | | Shorthands for `--container yuv`, `--container y4m`, `--container png` (8-bit, tone-mapped as rendered, via `ovt render` defaults), `--container png16` (16-bit, code values). `--raw` here is the raw-plane **output** flag (D21); the headerless-YUV **input** geometry is `--raw-format` (§12.1.5). One PNG per frame (`-o` pattern with `{display_index}`). |
| `--range <r>` | enum `auto\|full\|limited` | `auto` | Override the signalled video range for `png` output (chapter 4 FR-HDR-005). |
| `--info` | flag | off | Add `coded_size`, `crop`, `chroma_format`, `bit_depth_luma/chroma`, `color` (`ovt_ColorDescription` JSON) per frame to the manifest; with `--frames` and no `-o` nothing is written to disk. |
| `--pixel <x>,<y>` | int pair, repeatable | | Add `pixels:[{display_index,x,y,y_value,u,v}]` (coded coordinates, chroma at the co-sited chroma sample) to the manifest; no picture output unless `-o`. |
| `--histogram <p>` | enum `y\|u\|v` | | Add a `2^bit_depth`-bin histogram of the plane over the cropped frame to the manifest. |
| `--time` | flag | off | The global `--time` of §12.1.2; for `dump` the `timing` object carries `seek_ms`, `decode_ms_per_frame`, `first_frame_ms` and `wall_ms`, and the same members are repeated in the manifest (chapter 4 NFR acceptance tests). |
| `--no-film-grain` | flag | off | AV1: skip film-grain synthesis (hashes then match grain-less references; default applies grain, matching libaom test vectors — chapter 14 §14.3). |
| `--coded-size` | flag | off | Dump/hash coded (uncropped) planes; equals `--no-crop`. |
| `--diff <A> <B>` | two paths | | Structural FlatBuffers diff (chapter 06 WI-0.085 canonical builder) of two stored records (`.fa`, `.fa.zst`, `.index.fb`, `.fb` streams or `.json` exports of the same root type): `result` = `{root_type, differences:[{path, a, b}]}` with FlatBuffers-style paths (`grid.qp[5]`), `text` prints one line per difference. No input stream is opened; `-o` is the report. Exit 0 when identical, 8 (`MISMATCH`) otherwise. This is the facility chapters 06 and 14 call `fbdiff` (D3). |

**Alias.** `ovt decode …` is accepted with exactly this grammar (chapter 4 FR-CLI-007 and the FR-DEC/FR-PLN acceptance tests use that spelling). **MD5 format.** `--md5` without `-o` prints one line per frame `<display_index>,<decode_index>,<md5>` to stdout in `text` format (the `--md5-file` layout above; chapter 14 §14.3 normalises `ffmpeg -f framemd5` output to it), and the **sequence MD5** as the last line `sequence,<md5>`: the MD5 over the concatenation of all per-frame hashed bytes in output order, which is what `aomdec --md5` and `dav1d --muxer md5` print (both tools print only this single sequence digest, not per-frame lines; `dav1d --muxer md5` hashes the cropped planes in 8/16-bit little-endian like OVT). In `json`/`fb` the same values are in `DumpManifest.outputs[].md5[]` and `DumpManifest.outputs[].sequence_md5`.

**Output.** `result` = `DumpManifest` (chapter 06 §6.3, index in §12.2.17): output paths, frames written, geometry, bit depth, md5 list, decoder/plugin id and version. Pictures are written to `-o`; if `-o` is `-`, Y4M goes to stdout and the manifest to stderr as a single JSON line.

**Examples.**

```
ovt dump clip.mp4 -o clip.y4m --md5
ovt dump clip.hevc --stage recon --stage pred --stage residual --plugin de265 -o clip_{stage}.y4m --frames 0-59
ovt dump clip.obu --stage precdef --bitdepth 8 --container yuv -o precdef.yuv --order decode
ovt dump --diff golden/tiny16_hevc.fa out/tiny16_hevc.fa        # exit 8 and `grid.qp[5]: 26 != 27` on a difference
```

### 12.2.7 `ovt extract`

**Purpose.** Write parts of a stream to a new file: unit ranges, sample ranges, frames (with the minimum set of units needed to decode them: all active parameter sets + the preceding RAP and every frame up to the target, "min-CVS"), or a whole track as an elementary stream.

| Flag | Type | Default | Description |
|---|---|---|---|
| `--units <sel>` | selector over `Unit.index` | | Copy exactly these units. |
| `--samples <sel>` | selector over sample index | | Copy these container samples (all their units). |
| `--frames <sel>` | frame selector | | Copy these frames **plus** what is needed to decode them (`--min-cvs`, default on). |
| `--raps` | flag | off | Shorthand for `--frames rap --no-min-cvs --with-sei`: every RAP picture with its parameter sets (chapter 04 FR-CLI-013); the output decodes as an all-intra stream. |
| `--min-cvs` / `--no-min-cvs` | flag | on | With `--frames`: include parameter sets and the decode chain from the preceding RAP. `--no-min-cvs` copies only the frames' own units. |
| `--to <fmt>` | enum `annexb\|obu\|ivf\|mp4` | `annexb` for H.26x, `obu` for AV1 | Output format. `mp4` (phase 4) writes an un-fragmented single-track MP4 with the original `stsd`. `ivf` adds the 32-byte IVF header. |
| `--decode` | flag | off | Instead of bitstream units, decode and write the selected frames as Y4M (shortcut for `dump --frames`). |
| `--with-sei` / `--no-sei` | flag | with | Keep SEI / metadata OBUs belonging to selected frames. |
| `--temporal-id <n>` | int | | Keep only units with `temporal_id ≤ n` (sub-stream extraction). |

**Output.** `result` = `ExtractManifest` (chapter 06 §6.3, index in §12.2.17): unit indices written, byte count, frames covered, decode-chain length.

**Examples.**

```
ovt extract clip.mp4 -t 0 --to annexb -o clip.h265             # whole track
ovt extract clip.mp4 --frames 1234 -o frame1234.h265            # min-CVS to picture 1234
ovt extract clip.obu --units 0-20 --to ivf -o head.ivf
ovt extract clip.h264 --temporal-id 0 -o base_layer.h264
```

### 12.2.8 `ovt analyze`

**Purpose.** Block-level analysis export (core capabilities 3 and 4): runs the analysis path (ADR-009) and writes `FrameAnalysis` records (§0.5.4).

| Flag | Type | Default | Description |
|---|---|---|---|
| `--frame <n>` | int, repeatable | | Analyse this frame (display index; decode index with `--order decode`). Frames between the preceding RAP and `n` are decoded but not emitted. |
| `--range <A-B>` | selector | | Analyse and emit a range. |
| `--all` | flag | | Every frame of the stream. |
| `--plugin <id>` | string | default for codec (§12.3.11) | Analysis plugin id, e.g. `aom`, `de265`, `ffmpeg`, `hm`. |
| `--planes <stages>` | comma list of `PlaneStage` names (as in `dump --stage`) or `none` | `none` | Also export planes for each emitted frame to `--planes-dir` as `{decode_index:06}_{stage}_{plane}.raw` + a JSON sidecar; `planes[]` `blob_id`s in the output are these file names. |
| `--planes-dir <dir>` | path | `<output>.planes/` | Target directory for planes. |
| `--fields <list>` | comma list of `CellGrid` vector names | all | Restrict exported grid vectors (`qp,pred,mv0_x,...`) to shrink output. |
| `--no-blocks`, `--no-mvs`, `--no-symbols`, `--no-ext` | flags | off | Drop `blocks[]`, `mvs[]`, `symbols[]`, or the nested `ext` tables respectively. |
| `--yuview-types <list>` | comma list | all | For `--format yuview`: which statistics types to write (chapter 06 §6.8.2 names them). |
| `--yuview-poc-column <c>` | enum `display_index\|poc` | `display_index` | For `--format yuview`: value written in the POC column (chapter 06 §6.8.2 "POC column"; maps to `YuviewOptions.poc_column_is_poc`). |
| `--threads` | (global) | | Passed to the plugin as `threads` in `create()`. |
| `--csv-rows <r>` | enum `cell\|mv` | `cell` | Row kind for `--format csv`: `cell` (below) or `mv` = one row per `MotionVector`: `decode_index,display_index,x,y,w,h,list,ref,mv_x,mv_y,mvd_x,mvd_y,predictor,flags` (`--format mv-csv` is the alias of `--format csv --csv-rows mv`; chapter 04 FR-CLI-008). |
| `--verify-with <id>` | plugin id | | Decode every emitted frame a second time with plugin `id` (typically `hm`) and compare reconstruction planes sample-by-sample and every `CellGrid` vector; differences become `VER-0001`/`VER-0002` diagnostics (chapter 07 §7.6.4 "Verify mode", WI-4.106). Exit 8 when any frame differs, 6 when `id` is not available. |
| `--validate` | flag | off | Run the `FrameAnalysis` validation rules V-001…V-032 of chapter 06 §6.9 (`validate_frame_analysis`, WI-0.086) on every emitted record and append violations to `diagnostics[]` as `{code:"V-nnn", severity, message, path}` (`path` is the FlatBuffers-style locator, e.g. `grid.qp[1234]`). Exit 0 when no violation, **5** (`CORRUPT`) when any Error-severity violation is reported (records are still written), 6 when the plugin itself fails (§12.1.3). Also accepts a stored `.fa`/`.fa.zst` file or a cache directory instead of a stream (`ovt analyze --validate out.fa`), in which case nothing is decoded. |

**Output.** `fb`: one `FrameAnalysis` record per emitted frame in `--order`. `json`: `result` = array of `FrameAnalysis`; `CellGrid` vectors are JSON arrays. `csv`: one row per **cell**: `decode_index,display_index,cx,cy,block_id,partition_id,pred,qp,intra_mode_y,intra_mode_c,mv0_x,mv0_y,mv1_x,mv1_y,ref0,ref1,tx_log2_w,tx_log2_h,tx_type,cbf,bits_x8,deblock_bs_v,deblock_bs_h,filter_idx` (missing vectors → empty cells). `yuview`: §12.1.4. `aom-inspect`: §12.1.4. Determinism: `FrameAnalysis.plugin_version` is a fixed string per build, so output is byte-stable.

**Examples.**

```
ovt analyze clip.obu --frame 30 --format json | jq '.result[0].grid.qp | length'
ovt analyze clip.hevc --range 0-59 --plugin de265 --planes recon,pred --format fb -o gop0.fb
ovt analyze clip.ivf --all --format yuview -o clip_stats.csv
ovt analyze clip.obu --frame 0 --format aom-inspect
ovt analyze clip.hevc --range 0-59 --plugin de265 --validate -f json | jq '[.diagnostics[] | select(.code|startswith("V-"))] | length'
```

### 12.2.9 `ovt metrics`

**Purpose.** Quality metrics (core capability 2) through the chapter 09 pipeline: decode reference and distorted streams (playback path, software), temporal alignment, normalisation into each metric's `WorkingDomain`, scoring, pooling, export. Bit-exact with the `vmaf` CLI for VMAF models (phase 3 exit criterion).

| Flag | Type | Default | Description |
|---|---|---|---|
| `--reference <path>`, `-r` | path | required for full-reference metrics | Reference stream (any supported input, incl. headerless YUV with `--raw-format`). `--reference-raw <WxH:pix_fmt[:fps]>` (grammar of §12.1.5) for a headerless YUV reference. |
| `--distorted <path>`, `-d` | path, **repeatable** | required | One or more distorted streams; each produces its own `MetricsResult` set inside the single `MetricsReport`. `--distorted-raw <WxH:pix_fmt[:fps]>` as above (applies to all). |
| `--metric <id>`, `-m` | string, repeatable | `psnr,ssim` | Metric ids from `ovt plugins --metrics` (`psnr`, `psnr_hvs`, `ssim`, `ssim_x264`, `ms_ssim`, `vmaf`, `vmaf_neg`, `cambi`, `ciede2000`, `uvq`, `xpsnr`, …). `--metric all-fr` = every full-reference metric available. |
| `--model <spec>` | string, repeatable | `vmaf_v0.6.1` | libvmaf model spec, e.g. `version=vmaf_v0.6.1neg` or `path=/m/vmaf_4k_v0.6.1.json`; may repeat (one `MetricsResult` per model). Also `--uvq-model <dir>` for the ONNX files. |
| `--normalize <json>` | JSON string or `@file.json` | `{}` | `NormalizationConfig` JSON (chapter 09 §9.2.4, schema `core/schema/normalization_config.schema.json`): top-level objects `crop` (`auto_detect`, `auto_apply`, explicit rectangles), `scale` (`to`, `kernel`), `chroma_upsample`, `bit_depth`, `range` (`reference|limited|full`), `colour`, `display_model`, `frame_rate`, `temporal` (alignment), `spatial`; unknown keys are a usage error (exit 2). `--align`, `--roi`, `--scale-kernel`, `--fps-policy` and `--chroma-policy` are shorthands that set the corresponding keys. The resolved config is echoed in `MetricsResult.normalization`. |
| `--align <m>` | `auto\|N\|none` | `auto` | Temporal alignment (chapter 09 §9.3, D9): `auto` searches the offset over ±30 frames by normalised cross-correlation of downscaled luma and reports `confidence = S(o*) − S(o2)` ∈ [0, 2] (below `min_confidence` 0.05 → Warning `VQ-ALIGN-0001` (chapter 09 §9.11.4 registry) and offset 0); `N` forces the offset; `none` assumes aligned. Sign convention: offset `o` means distorted frame `i` corresponds to reference frame `i + o`. Result in `alignment_offset_frames` / `alignment_confidence`. |
| `--maps` | flag | off | Export per-frame quality maps (`FrameScore.map_blob_id`) to `--maps-dir` as 16-bit PGM, one file per frame at `<maps-dir>/<metric>/<display_index:06>.pgm` (e.g. `out/psnr/000010.pgm`; chapter 4 FR-VQ-011), block size `map_block_size`. The cache keeps the binary `.ovtmap` form of chapter 9 §9.9.4. |
| `--maps-dir <dir>` | path | `<output>.maps/` | |
| `--pool <list>` | comma list of `mean,harmonic_mean,min,max,p5,p95,median,all` (chapter 09 §9.5.5; `harmonic` is accepted as an alias of `harmonic_mean`) | `mean,harmonic_mean,min` | Which pooled values to print (all are stored in `MetricsResult`; this only affects `text`/`csv`). |
| `--vmaf-json <path>` | path | | Additionally write a **libvmaf-compatible JSON** (`vmaf --json` layout: `version`, `fps`, `frames[].frameNum`, `frames[].metrics.{vmaf,psnr_y,...}`, `pooled_metrics`, `aggregate_metrics`) so existing dashboards consume it unchanged; `--format vmaf-json` writes the same document as the main output. Chapter 09 §9.9 gives the key mapping. (Not `--json <path>`: `--json` is the global alias of `--format json`, §12.1.2.) |
| `--csv <path>` | path | | Additionally write the per-frame CSV regardless of `--format`. |
| `--fail-below <metric>=<value>` | repeatable | | Exit 8 if the pooled mean of `metric` is below `value` (e.g. `vmaf=93`). |
| `--subsample <n>` | int | 1 | Score every n-th frame (libvmaf `--subsample` semantics). |
| `--opt <id>.<key>=<json>` | repeatable | | Per-metric option (chapter 09 §9.5 option tables), e.g. `--opt uvq.fps=2`, `--opt uvq.version="1.0"`, `--opt cambi.window_size=63`, `--opt vmaf.features='["float_ssim","cambi"]'`, `--opt vmaf.phone=true`, `--opt scdet.threshold=0.3`; the value is JSON (strings need quotes). Unknown key → exit 2. |
| `--group-by <list>` | comma list of `type,tid,scene` | none | Additional pooled rows per frame type / temporal layer / scene (chapter 09 §9.8, `MetricsReport.groups[]`); needs a `StreamIndex` of the distorted stream. |
| `--display-model <m>` | enum `pq_1000\|pq_4000\|pq_10000\|hlg_1000\|bt1886_100` | from `ovt_ColorDescription` | Display model for HDR-aware metrics (`cvvdp`, `de_itp`; chapter 09 §9.6). |
| `--frames <sel>` | selector | all | Score only the selected display indices of the *reference* (selector grammar of §12.1.5, e.g. `0-299`). |
| `--roi <x>,<y>,<w>,<h>` | 4 ints | full frame | Score only this region (luma coordinates after normalisation; chroma scaled accordingly). Shorthand for `--normalize '{"crop":{...}}'`. |
| `--domain <d>` | enum `code_value\|linear_light\|pu21\|ictcp` | metric default | Force the `WorkingDomain` the frames are converted into before scoring (chapter 09 §9.4); metrics that cannot operate in it → exit 2. |
| `--max-frames <n>` | int | 0 | Stop after n scored frames. |

**Alias spellings (chapter 04 FR-CLI-009, chapter 14 §14.5).** `--ref` = `-r`; `--dist` = `-d`; `--metrics <a,b,c>` = `-m a -m b -m c`; `--vmaf-model <name>` = `--model version=<name>`; `--no-align` = `--align none`; `--offset <n>` = `--align <n>`; `--no-scale` = `--normalize '{"scale":"none"}'`; `--scale-kernel <k>` = `--normalize '{"filter":"<k>"}'`; `--crop <x,y,w,h>` = `--roi`; `--display <m>` = `--display-model <m>`; `--quick` = `--subsample 5`; `--map <n>` = `--maps` with `map_block_size n`; `--fps-policy <p>`, `--chroma-policy <p>` = `--normalize '{"fps":"<p>"}'` / `'{"chroma":"<p>"}'`; `--vmaf-features <list>` = `--opt vmaf.features=["..."]`; `--uvq-version <v>`, `--uvq-fps <n>` = `--opt uvq.version="<v>"`, `--opt uvq.fps=<n>`; `--pool` and `--threads` are canonical already.

**Output.** `result` = one `MetricsReport` whose `results[]` holds one `MetricsResult` per distorted × metric × model (chapter 06 §6.5). `csv`: header `distorted,metric,model,display_index,plane,value` then pooled rows with `display_index` = `pooled_mean` etc. `text` prints a table per distorted file:

```
distorted: enc_q30.mp4   aligned +0 frames (confidence 1.00)   normalisation: scale=ref bicubic, range=limited
metric       mean     harmonic   min      p5       frames  engine
psnr_y      42.113    41.902    36.550   38.812    3600    ovt-kernels 1.0
ssim         0.9812    0.9808    0.9411   0.9601   3600    ovt-kernels 1.0
vmaf        94.21     94.02     81.70    88.90     3600    libvmaf 3.0.0 (vmaf_v0.6.1)
```

**Examples.**

```
ovt metrics -r src.y4m -d enc_q24.mp4 -d enc_q30.mp4 -m psnr -m ssim -m vmaf --vmaf-json out/{distorted}.json
ovt metrics -r src.mp4 -d enc.mp4 -m vmaf --model version=vmaf_v0.6.1neg --normalize '{"scale":"ref","filter":"bicubic"}' --maps
ovt metrics -d ugc.mp4 -m uvq --uvq-model models/uvq/ -f csv -o uvq.csv
ovt metrics -r a.y4m -d b.mp4 -m vmaf --fail-below vmaf=93 --align 2
```

`{distorted}` in `--vmaf-json`/`--csv`/`-o` paths expands to the distorted file's basename without extension; `{metric}` and `{model}` likewise.

### 12.2.10 `ovt bdrate`

**Purpose.** Bjøntegaard-delta rate and quality between an anchor and one or more test RD curves (chapter 09 §9.10).

| Flag | Type | Default | Description |
|---|---|---|---|
| `--anchor <src>` | path or `--rd` list | required | Anchor RD points. A `.csv`/`.json` written by `ovt metrics`/`ovt bdrate --collect`, or inline `--rd "bitrate_kbps:score,bitrate_kbps:score,..."`. |
| `--test <src>` | path or `--rd` list, repeatable | required | Test curve(s). |
| `--metric <id[/component]>` | string | `vmaf` if present else `psnr_y` | Which pooled metric column to use when reading metrics files; `/component` selects a plane or sub-score (`psnr/y`, `vmaf/vmaf_neg`). `--interp <i>` is accepted as an alias of `--interpolator` (chapter 04 FR-VQ, chapter 14); a comma list (`--interp cubic,pchip`) emits one `BdRateResult` per interpolator. |
| `--hull` | flag | off | Convex-hull mode (chapter 09 §9.10): build the Pareto hull over all anchor and test points (several resolutions/encoders per curve) before interpolating; adds `hull[]` points to the result. |
| `--targets <kbps,…>` | comma list | | With `--hull`: also report the hull quality at these bitrates (`targets[]`). |
| `--pool <p>` | `mean\|harmonic_mean` | `mean` | Which pooled value of the metrics files to use as the RD quality. |
| `--interpolator <i>` | enum `pchip\|cubic\|akima` (= `ovt.fb.Interpolator`, chapter 06 §6.5) | `pchip` | Interpolation of log(rate) vs quality. `pchip` is the default of libaom's `tools/` and of the Netflix/AOM CTC; `cubic` is the classic 3rd-order polynomial fit of VCEG-M33 (JVET CTC). |
| `--range <mode>` | `overlap\|anchor` | `overlap` | Integration interval: quality overlap of both curves (standard) or the anchor's range (extrapolating the test). |
| `--collect <files...>` | paths | | Helper: read several `ovt metrics` outputs plus their file sizes/durations to build RD points (`bitrate = size_bytes*8/duration_s`); write a `.json` RD file. |
| `--min-points <n>` | int | 4 | Fail (exit 2 `USAGE`, `VQ-BD-0002`) with fewer points per curve; `--interpolator cubic` requires ≥ 4 points per curve (least-squares cubic fit, chapter 09 §9.8.4); with fewer the cubic interpolator is unavailable (`VQ-BD-0002`, exit 2). Non-overlapping quality ranges with `--range overlap` → `VQ-BD-0001`, exit 8 `MISMATCH` (chapter 09 §9.10 owns the `VQ-BD-*` codes). |

**Output.** `result` = one `BdRateResult` record per `--test` curve (`ovt_metrics.fbs`, chapter 06 §6.5): `anchor_name`, `test_name`, `metric_id`, `plane`, `pooling`, `anchor:[RdPoint]`, `test:[RdPoint]`, `interpolator`, `range_mode`, `bd_rate_percent`, `bd_quality`, `overlap_low`, `overlap_high`, `overlap_fraction`, and `diagnostics[]` (`VQ-BD-0001` non-overlapping ranges, `VQ-BD-0002` fewer than `--min-points` points, `VQ-BD-0003` cubic fit with ≠ 4 points or non-monotonic curve; chapter 09 §9.10). `csv`: `test_name,metric_id,bd_rate_percent,bd_quality,points_anchor,points_test,overlap_low,overlap_high,interpolator` (point counts = `anchor.length`, `test.length`).

**Examples.**

```
ovt bdrate --anchor x265_medium.json --test x265_slow.json --metric vmaf
ovt bdrate --anchor --rd "1000:35.1,2000:38.2,4000:41.0,8000:43.5" --test --rd "900:35.3,1800:38.6,3700:41.4,7500:43.9"
ovt bdrate --collect enc_q*.metrics.json -o rd_x265.json
```

### 12.2.11 `ovt hdr`

**Purpose.** HDR/colour report: declared metadata (container, VUI/sequence header, SEI, HDR10+ / Dolby Vision RPU presence) and measured per-frame light levels (CPU implementation of chapter 10 §10.2.6 `Measurements`, so the CLI result equals the GUI's).

| Flag | Type | Default | Description |
|---|---|---|---|
| `--metadata` | flag | default | Report declared metadata and conflicts (`ovt_ColorDescription` + dynamic metadata list per frame when present). |
| `--measure` | flag | off | Decode (software) and compute per frame: `max_nits`, `fall_nits` (frame average light level), `p99_nits`, `min_nits`, luminance histogram (64 log bins), percentage of pixels above `--threshold` nits; sequence MaxCLL/MaxFALL by the CTA-861.3 method (max over frames). |
| `--threshold <nits>` | float | 1000 | For the "above threshold" percentage. |
| `--dynamic` | flag | off | Print per-frame HDR10+ (ST 2094-40) / DV RPU (presence + profile/level only; no RPU parsing in phase 3) records. |
| `--compare-declared` | flag | off | Add `declared_vs_measured`: declared MaxCLL/MaxFALL versus measured; raises `VQ-HDR-0002` (Warning; chapter 09 §9.11.4 registry) when measured exceeds declared by > 5 %. |

**Output.** `result` = `HdrReport` (chapter 06 §6.3, index in §12.2.17). `csv`: `display_index,max_nits,fall_nits,p99_nits,min_nits,pct_above_threshold`.

**Examples.**

```
ovt hdr clip.mp4
ovt hdr clip.mp4 --measure --compare-declared --frames every:10 -f csv -o light.csv
ovt hdr clip.mp4 --dynamic -f json | jq '.result.dynamic[0]'
```

### 12.2.12 `ovt verify`

**Purpose.** Conformance checks on decoded pictures: (a) decoded picture hash SEI (H.265 D.3.19 / H.264 none / AV1 none — for AV1 and H.264 only mode (b) exists) against the software decode; (b) plugin-vs-oracle comparison (e.g. `de265` vs `hm`, `ffmpeg` vs `de265`, `aom` vs `dav1d` playback decoder).

| Flag | Type | Default | Description |
|---|---|---|---|
| `--hash-sei` | flag | default for HEVC | Compare MD5/CRC/checksum from `decoded_picture_hash` SEI with the decoded planes (per plane). Aliases: `--hash` (chapter 04 FR-CLI-014), `--picture-hash` (chapter 14). |
| `--levels` | flag | off | Check the stream against its signalled profile/tier/level limits (chapter 04 FR-SYN-020: picture size, sample rate, bit rate/CPB from the HRD when present, DPB size, tiles/slices per picture); each violation is a `VER-LVL-nnnn` Error diagnostic and `checks.levels = "fail"`. Needs no decoding. |
| `--against <id>` | string | | Oracle decoder/plugin id (`hm`, `dav1d`, `ffmpeg`, `de265`, `aom`) to compare with `--plugin`. `--plugin-compare <a>,<b>` (chapter 04) = `--plugin <a> --against <b>`. |
| `--plugin <id>` | string | default for codec | Decoder under test. |
| `--tolerance <n>` | int | 0 | Maximum absolute sample difference accepted (0 = bit-exact). |
| `--stop-on-first` | flag | off | Exit at the first mismatching frame. |
| `--report-planes` | flag | off | Write the first mismatching frame's planes from both decoders to `--planes-dir`. |

**Output.** `result` = `VerifyReport` (chapter 06 §6.3, index in §12.2.17): `checks` (`hash`, `levels`, `against`: `pass|fail|skipped`) and per frame `ok`, `plane_mismatch_mask`, `max_abs_diff`, `first_mismatch_xy`, `declared_hash`, `computed_hash`. Exit 0 only when every requested check passes; 8 if any frame mismatches or a level limit is violated; exit 4 if the stream has no hash SEI and neither `--against` nor `--levels` was given.

**Examples.**

```
ovt verify conformance/WPP_A_ericsson_MAIN_2.bit --hash-sei
ovt verify clip.hevc --plugin de265 --against hm --frames 0-59
ovt verify clip.obu --plugin aom --against dav1d --tolerance 0 --stop-on-first
```

### 12.2.13 `ovt cache`

| Form | Description |
|---|---|
| `ovt cache list` | One row per cached entry: `key,kind (index\|analysis\|metrics),source_path_basename,size_bytes,created (ISO 8601),last_used`. Timestamps live in cache metadata only (§0.7 rule 5) — therefore `cache list` output is exempt from FR-CLI-004 and says so in `--help`. |
| `ovt cache stats` | Totals, entry count, limit (`--max-size` setting), hit/miss counters since creation. |
| `ovt cache clear [--kind k] [--older-than <days>] [--source <path>]` | Delete entries. Without filters: everything (asks for confirmation on a TTY unless `--yes`). |
| `ovt cache path` | Print the resolved cache directory. |
| `ovt cache import <path>` | Import stored records into the cache for their `source_hash`: `path` is a `.json` export (reverse of the JSON exporter, chapter 06 §6.8.1 and WI-0.088 — the round-trip MUST be byte-identical), a `.fa`/`.fa.zst` `FrameAnalysis`, an `.index.fb` `StreamIndex` or a directory of them. Records whose `schema_version` or `source_hash` do not match an existing entry create a new entry. `result` = `CacheReport` restricted to the touched entries. Exit 5 when a record fails `ovt_validate_*` (§12.3.8). |
| `ovt cache stats --max-size` | (setting) `--max-size <GiB>` on `stats`/`clear` sets the cap persistently in `cache.json` (default 20 GiB desktop, 2 GiB Android; chapter 06 §6.7.4). |

**Output.** `result` = `CacheReport`.

### 12.2.14 `ovt plugins`

| Flag | Description |
|---|---|
| `--analysis` / `--metrics` / `--decoders` | Restrict to analysis decoders / metric plugins / playback decoders (default: all). |
| `--verbose` | Include `notes`, `grid_fields` names, `plane_stages` names, library path and SHA-256 of the plugin file. |
| `--check` | Load every plugin and call `caps()`/`descriptor()`; exit 6 if any fails. |

**Output.** `result` = `PluginList` (chapter 06 §6.3, index in §12.2.17). `text`:

```
id       kind      codec  version    licence                 abi  path
aom      analysis  AV1    3.12.1+ovt BSD-2-Clause+AOM-Patent 1    /Applications/OVT.app/Contents/PlugIns/libovt_plugin_aom.dylib
de265    analysis  H265   1.0.15+p1  LGPL-3.0-or-later       1    ~/Library/Application Support/ovt/plugins/libovt_plugin_de265.dylib
ffmpeg   analysis  H264   7.1        LGPL-2.1-or-later       1    (built-in)
vmaf     metric    -      3.2.1      BSD-2-Clause-Patent     1    (built-in)
```

### 12.2.15 `ovt completion`

`ovt completion <bash|zsh|fish>` prints a completion script to stdout. CLI11 (2.4.x) has no completion generator of its own; `cli/tools/gen_completions.cpp` (WI-1.500 step 7) walks the `CLI::App` tree (subcommands, options, enum choices from `CLI::IsMember` validators) at build time and writes the three scripts into `build/<preset>/generated/completions/`, which are embedded as string constants. Installed by the Homebrew cask, the `.deb`/`.rpm` and Flatpak (chapter 13 §13.6).

### 12.2.16 `ovt doctor`

**Purpose.** Environment report for bug reports and first-run checks: build info, plugin search paths and what was found, hardware decoders available (VideoToolbox / MediaCodec / VA-API probe per codec at 1080p and 4K), GPU and display HDR capability (Vulkan device list via libplacebo; per display: `hdr_capable`, `max_nits`, `headroom`, colour space), cache state, CPU features (NEON/AVX2/AVX-512 as reported by Highway), `OVT_*` environment variables. `--format json` only (and `text`). Never fails except with exit 1; findings are `diagnostics[]` (`Warning` for a missing HEVC plugin, `Info` for no HDR display).

| Flag | Description |
|---|---|
| `--no-gpu` | Skip the Vulkan/libplacebo probe (headless CI). |
| `--decode-test <path>` | Decode the first 10 frames of `path` with every available playback decoder and report success/fps. |

**Output.** `result` = `DoctorReport` (chapter 06 §6.3, index in §12.2.17).

### 12.2.17 Result tables used by the CLI (index into chapter 06)

All result tables are FlatBuffers tables owned by chapter 06; the CLI adds **no** schema file of its own. Field-level text, defaults and evolution rules are in chapter 06 only.

| Table (namespace `ovt.fb`) | Schema file / section | Used by |
|---|---|---|
| `CliEnvelope` (`"OVTE"`) | `ovt_model.fbs`, chapter 06 §6.3 "CLI / API report tables" | every `fb`/`json` stream (§12.1.4) |
| `DumpManifest`, `DumpOutput`, `FrameHash` | same | `ovt dump` (§12.2.6) |
| `ExtractManifest` | same | `ovt extract` (§12.2.7) |
| `PluginInfo`, `PluginList` (`"OVTP"`) | same | `ovt plugins` (§12.2.14), `ovt doctor` |
| `HdrReport`, `HdrFrameMeasure`, `HdrDynamicRecord` | same | `ovt hdr` (§12.2.11) |
| `VerifyReport`, `VerifyFrame` | same | `ovt verify` (§12.2.12) |
| `CacheReport`, `CacheEntry` | same | `ovt cache` (§12.2.13), `ovt doctor` |
| `DoctorReport` | same | `ovt doctor` (§12.2.16) |
| `RdPoint`, `BdRateResult` | `ovt_metrics.fbs`, chapter 06 §6.5 | `ovt bdrate` (§12.2.10) |
| `StreamIndex`, `FrameInfo`, `SyntaxNode`, `FrameAnalysis` | `ovt_model.fbs`, chapter 06 §6.3 | `syntax`, `index`, `frames`, `analyze` |
| `ContainerTree` / `ContainerRoot` | `ovt_container.fbs`, chapter 06 §6.4 (single normative schema; derivation rules in chapter 08 §8.1.3/§8.4) | `info`, `probe` |
| `MetricsReport` (`"OVTR"`), `MetricsResult`, `SceneSegment` | `ovt_metrics.fbs`, chapter 06 §6.5 | `metrics`, `scenes` |
| `HrdTrace`, `HrdPoint` | `ovt_model.fbs`, chapter 06 §6.3 | `hrd` (§12.2.21) |

### 12.2.18 `ovt render`

**Purpose.** Headless rendering of one frame through an offscreen `VideoSurface` (`DisplayTarget::OffscreenTexture`, chapter 10 §10.2) to PNG, so that GUI output can be regression-tested (chapter 14 §14.6) and HDR/SDR grabs can be scripted (chapter 10 WI-4.457). Requires `libovt-render`; builds without `OVT_BUILD_GUI` still have it when `OVT_BUILD_RENDER` is on, otherwise exit 4.

| Flag | Type | Default | Description |
|---|---|---|---|
| `--frame <n>` | int | required | Display index (`--order decode` for decode index). |
| `--target <t>` | enum `sdr-bt1886-203\|sdr-srgb-203\|pq-bt2020-1000\|pq-bt2020-10000\|hlg-bt2020-1000\|view` | `sdr-bt1886-203` | Output colour target (`view` = the GUI's current defaults: SDR 8-bit tone-mapped). `pq-*` and `hlg-*` write 16-bit PNG with the content's primaries; `sdr-*` write 8-bit PNG. |
| `--kind <k>` | enum `sdr8\|hdr16\|view` | derived from `--target` | Legacy spelling used by chapter 10 WI-4.457: `sdr8` = `--target sdr-bt1886-203`, `hdr16` = `--target pq-bt2020-10000`. |
| `--tone-map <m>` | enum `auto\|bt2390\|bt2446a\|spline\|clip\|none` | `bt2390` | `ovt::render::ToneMap` (chapter 10 §10.3.4). |
| `--gamut <g>` | enum `perceptual\|relative\|clip\|none` | `perceptual` | Gamut mapping mode (chapter 10 §10.3.4). |
| `--dither <d>` | enum `on\|off` | `off` | Dithering (off for reproducible goldens). |
| `--scale <s>` | enum `bilinear\|bicubic\|lanczos\|ewa-lanczos\|nearest` | `bilinear` | Up/downscaler when `--size` differs from the crop size. |
| `--size <WxH>` | string | crop size | Output size in pixels. |
| `--plane <p>` | enum `yuv\|y\|u\|v\|gray` | `yuv` | `ViewState.plane_mode` (chapter 10 §10.2.3). |
| `--overlays <preset>` | string | none | Overlay preset `F1`..`F12` (chapter 11 §11.4.2) drawn from the frame's `FrameAnalysis` (requires a plugin; chapter 10 §10.5). |
| `--out <path>` | path | required | PNG file. |

**Output.** `result` = `DumpManifest` with one `DumpOutput` (`container = "png"`, `md5` of the PNG's decoded RGB(A) bytes). Exit codes: 0; 2 bad flag; 3 cannot write; 4 renderer not built / no GPU or software Vulkan device (`OVT_RENDER_SOFTWARE=1` forces lavapipe on Linux CI, chapter 14 §14.6); 5 corrupt input. **Determinism:** with `--dither off` and `--scale bilinear`, two runs on the same GPU driver are byte-identical; across drivers the tolerance of chapter 14 §14.6 applies.

**Examples.**

```
ovt render clip.mp4 --frame 12 --target sdr-bt1886-203 --tone-map bt2390 --gamut perceptual --dither off --scale bilinear --out f12_sdr.png
ovt render clip.mp4 --frame 12 --target pq-bt2020-1000 --tone-map clip --out f12_hdr.png
ovt render clip.hevc --frame 0 --overlays F4 --plane y --out mv_overlay.png
```

*WI note:* delivered as part of chapter 10 WI-4.457 (`cli/src/cmd/render.cpp`) on top of WI-1.500/WI-1.507; the `render-frame` spelling (older, chapter 14) is **not** accepted.

### 12.2.19 `ovt compare`

**Purpose.** Pixel-level comparison of two decodable inputs or raw/Y4M files, without metrics (chapter 4 FR-PLN-009, FR-CLI-016). Inputs: `ovt compare [options] A B` (two positional inputs; `-t`/`--frames` apply to both; raw inputs need `--raw-format <WxH:pix_fmt[:fps]>` per §12.1.5).

| Flag | Type | Default | Description |
|---|---|---|---|
| `--first-diff` | flag | default | Stop at the first differing sample; `result` = `{frame, plane, x, y, a, b}` (display index, plane 0/1/2, coded coordinates, code values). |
| `--identity` | flag | off | Per-frame report `frames[i].differing_samples = [Y, U, V]` counts and `identical:bool`; sequence `identical_frames`, `total_frames`. |
| `--diff-image` | flag | off | Write `|A − B| × gain` (centred at mid-grey when `--signed`) of frame `--frame N` as 8-bit PNG to `-o`. |
| `--gain <g>` | float | 1 | Multiplier for `--diff-image`. |
| `--signed` | flag | off | Signed difference image (128 = equal). |
| `--tolerance <n>` | int | 0 | Samples differing by ≤ n are counted as equal. |
| `--coded-size` | flag | off | Compare coded (uncropped) planes. |

**Output.** `result` = the JSON object described per flag (no FlatBuffers table; `fb` is refused with exit 2). `csv` for `--identity`: `display_index,diff_y,diff_u,diff_v,identical`. Exit codes: 0 identical (or `--diff-image` written); 8 (`MISMATCH`) when any sample differs beyond tolerance (`--first-diff`/`--identity`); 2/3/4/5 as §12.1.3. Performance (NFR): 300 1080p 8-bit frames in ≤ 1 s on the reference machine (FR-PLN-009).

*WI note:* delivered as part of WI-3.530 (metrics CLI, shares the normalisation front-end of chapter 09 §9.3 with `--no-normalize`).

### 12.2.20 `ovt stats`

**Purpose.** Aggregate block statistics over a frame range from `FrameAnalysis` records (what the GUI Statistics panel shows, chapter 11 §11.3; chapter 4 FR-BLK-0xx): histograms of block sizes, prediction modes, QP, MV magnitude, transform sizes, reference usage, bits per block, plus the AV1/HEVC coding-tool usage table.

| Flag | Type | Default | Description |
|---|---|---|---|
| `--frames <r>` | range | all | Display-index range (§12.1.5). |
| `--plugin <id>` | string | default for codec | Analysis plugin. |
| `--by <g>` | enum `frame\|type\|tid\|all` | `all` | Aggregation grouping (`type` = `FrameType`, `tid` = temporal id). |
| `--toolset` | flag | off | Adds `toolset.<tool>.{blocks,frames,area_pct}` for every codec tool the plugin reports (palette, intrabc, obmc, warped, superres, cdef, lr, sao, pcm, transform-skip, …). |

**Output.** `result` = JSON object `{range, frames, histograms:{<name>:{bins:[...], counts:[...], area_pct:[...]}}, toolset:{...}}`; every `area_pct` vector sums to 100 ± 0.01. `csv`: one row per `(histogram, bin)`: `histogram,bin,count,area_pct`. The GUI's Statistics-panel CSV export (chapter 11) MUST produce identical rows for the same range. Exit codes as §12.1.3 (6 when no plugin).

*WI note:* delivered as part of WI-2.521 (`ovt analyze` family).

### 12.2.21 `ovt hrd`

**Purpose.** Hypothetical reference decoder CPB trace (H.264 Annex C / H.265 Annex C) from the parsed headers and unit sizes (FR-DEC-013; model and equations in chapter 7 §7.4.8, built by WI-4.112). AV1 is not modelled: exit 4 with `DEC-HRD-0001`. Phase 4.

| Flag | Type | Default | Description |
|---|---|---|---|
| `--sched-sel-idx <n>` | int | 0 | HRD schedule / operating point. |
| `--type <t>` | enum `nal\|vcl` | `nal` | `nal` = type II conformance with the NAL HRD parameters; `vcl` = type I with the VCL HRD parameters (chapter 7 §7.4.8 item 7). |
| `--initial-removal-delay <n>` | int (90 kHz ticks) | from buffering period SEI | Overrides `initial_cpb_removal_delay` of every buffering period (chapter 7 §7.4.8 item 8). |

**Output.** `result` = `HrdTrace` (chapter 06 §6.3: `bit_rate_bps`, `cpb_size_bits`, `sched_sel_idx`, `nal_hrd`, `cbr`, `low_delay_hrd`, `conformance_type`, `underflow_count`, `overflow_count`, `frame:[HrdPoint{decode_index, init_arrival_time_90k, final_arrival_time_90k, removal_time_90k, cpb_fullness_bits, overflow, underflow}]`, `diagnostics`); `csv`: `decode_index,init_arrival_time_90k,final_arrival_time_90k,removal_time_90k,cpb_fullness_bits,overflow,underflow`. Exit 4 when the stream carries no HRD parameters (`SYN-HRD-0001`) or is AV1 (`DEC-HRD-0001`); 5 on corrupt timing SEI (`DEC-HRD-0002`) or when any overflow/underflow is reported (the trace is still written).

*WI note:* the subcommand is delivered as part of WI-4.543 (phase-4 CLI commands); the model implementation and the `HrdTrace` builder are chapter 7 WI-4.112 (`core/src/index/hrd.cpp`, C API `ovt_index_hrd_trace`).

### 12.2.22 `ovt scenes`

**Purpose.** Shot-boundary detection producing `SceneSegment`s (chapter 06 §6.5) for per-scene pooling and the GUI timeline (chapter 4 FR-VQ-0xx; chapter 09 §9.8.3).

| Flag | Type | Default | Description |
|---|---|---|---|
| `--method <m>` | enum `ffmpeg-scdet\|hist-diff` | `hist-diff` | Detector (`ffmpeg-scdet` needs the ffmpeg plugin). |
| `--threshold <t>` | float | method default (`hist-diff` 0.4, `scdet` 10) | Cut threshold. |
| `--min-length <n>` | int | 12 | Minimum segment length in frames. |

**Output.** `result` = `[SceneSegment]` records; `csv`: `index,first_display_index,last_display_index,boundary_score,method`. Exit codes as §12.1.3.

*WI note:* delivered as part of WI-3.530.

### 12.2.23 `ovt models`

**Purpose.** Manage downloadable metric model files (UVQ ONNX models, VMAF model variants not bundled; chapter 09 §9.6.5 manifest). Sub-verbs: `ovt models list`, `ovt models fetch <id>` (e.g. `uvq`, `vmaf_4k`), `ovt models verify <id>`, `ovt models path <id>`, `ovt models remove <id>`.

| Flag | Type | Default | Description |
|---|---|---|---|
| `--models-dir <dir>` | path | `$OVT_MODELS_DIR` else `<cache-dir>/models` | Where models live (chapter 09 §9.6.5 directory rules). |
| `--offline` | flag | off | Never download; exit 3 if missing. |
| `--force` | flag | off | Re-download even if present and verified. |

**Output.** `result` = `[{id, version, path, sha256, size_bytes, present:bool, verified:bool, license, url}]`; `csv` with the same columns. Exit 3 on download or checksum failure (`VQ-MODEL-0001`, chapter 09 §9.11.4 registry), 0 otherwise.

*WI note:* delivered as part of WI-3.530 together with the UVQ host of chapter 09.

---

## 12.3 The C API (`core/include/ovt/*.h`)

### 12.3.1 Header set and conventions

| Header | Contents | Umbrella |
|---|---|---|
| `ovt/ovt.h` | includes every header below; nothing else | — |
| `ovt/version.h` | `OVT_API_VERSION*`, `ovt_version*` | |
| `ovt/status.h` | `ovt_status`, `ovt_status_string`, `ovt_Buffer`, `ovt_buffer_free` | |
| `ovt/log.h` | `ovt_log_fn`, `ovt_set_logger`, `ovt_set_log_level` | |
| `ovt/jobs.h` | `ovt_Job` | |
| `ovt/io.h` | `ovt_File` | |
| `ovt/container.h` | `ovt_Container` | |
| `ovt/syntax.h` | `ovt_syntax_*` stateless parsing / trace | |
| `ovt/index.h` | `ovt_Stream` | |
| `ovt/validate.h` | `ovt_validate_frame_analysis`, `ovt_validate_stream_index` (chapter 06 §6.9 rules) | |
| `ovt/hrd.h` | `ovt_HrdOptions`, `ovt_index_hrd_trace` (chapter 07 §7.4.8) | |
| `ovt/decode.h` | `ovt_Decoder`, `ovt_Picture` (reference counted: `ovt_picture_ref`/`ovt_picture_release`), `ovt_PlaneView`, `ovt_picture_export_plane` | |
| `ovt/analysis.h` | `ovt_Analysis` | |
| `ovt/color.h` | `ovt_ColorDescription` (§0.5.6), `ovt_hdr_measure` | |
| `ovt/metrics.h` | `ovt_MetricsJob` (chapter 09 §9.10 job API), `ovt_metrics_push`/`ovt_metrics_finish`, `ovt_bdrate` | |
| `ovt/cache.h` | process-wide cache control; `ovt_Cache`/`ovt_CacheEntry` object API (chapter 06 §6.7) | |
| `ovt/plugins.h` | plugin search path, dynamic loading, static registration, listing | |
| `ovt/abi/analysis_decoder_v1.h`, `ovt/abi/metric_v1.h` | plugin ABIs (§0.6.2, §0.6.3) — BSD-2-Clause | included by `ovt.h` |

Conventions (normative, in addition to §0.4 naming):

1. All headers `core/include/ovt/*.h` (and `ovt/abi/*.h`) are C11 (`extern "C"` guarded), include only `<stdint.h>`, `<stddef.h>` and each other; no C++ types, no FlatBuffers headers. The **C++ helper headers** of the tree (`ovt::Result<T>`, `ovt::jobs::{Pool,Cancel,JobHandle}`, `ovt::Cancelled`, the C++ logging front-end; chapter 07 §7.1.6, chapter 05) live in `core/include/ovt/cxx/*.hpp` (`ovt/cxx/result.hpp`, `ovt/cxx/jobs.hpp`, `ovt/cxx/log.hpp`; D22). They are **not** part of the C API: `ovt/ovt.h` never includes them, they are excluded from the install of chapter 13 §13.2.6, and the C11 compile test of WI-1.511 covers `ovt/*.h` only.
2. Every function returns `ovt_status` except pure getters that cannot fail (documented individually) and `*_free/*_close` which return `void`. Output parameters are last and named `out*`.
3. **Ownership (D4).** (a) Every `ovt_Buffer` or pointer returned through an **out-parameter** (`ovt_Buffer* out`, `ovt_Picture** out`, `ovt_X** out`) is **owned by the caller** and released with exactly one function: `ovt_buffer_free(&buf)` for buffers (no-op on a zeroed buffer), or the type-specific `ovt_<type>_release`/`*_close` for handles (`ovt_picture_release`, `ovt_container_close`, …). The library never requires `free()` and the caller never calls `free()` on API memory. (b) The few functions that return `ovt_Buffer` or `const char*` **by value** (`ovt_container_tree`, `ovt_last_error`, `ovt_version_string`, `ovt_file_display_name`, `ovt_decoder_id`, …) hand out memory **owned by the library or handle**; it stays valid until the next non-const call on that handle (for `ovt_last_error`: until the next API call on the calling thread) or until the handle is closed, and MUST NOT be freed. Chapter 04 FR-API-002 and chapter 0 §0.6.1 state the same rule: a FlatBuffer is handle-owned only when returned by value.
4. **Threading.** Handles (`ovt_File`, `ovt_Container`, `ovt_Stream`, `ovt_Analysis`, `ovt_Decoder`, `ovt_MetricsJob`) are **not thread-safe**: concurrent calls on the same handle are undefined behaviour, with one documented exception — the container **byte service** (`ovt_container_bytes`, `ovt_container_box_at`, `ovt_container_segment_for_offset`, `ovt_container_size`, `ovt_container_stats_json`, chapter 08 §8.8) MAY be called concurrently with each other and with reads on `ovt_Stream`s of the same container (they only read the immutable top-level tree and the file). Different handles may be used from different threads concurrently, including two `ovt_Stream` handles on the same `ovt_Container` (the container serialises its own file reads). Debug builds detect a violation with an assertion naming the object (`ovt_Analysis used from two threads`, chapter 04 FR-API-005). `ovt_Job` functions are thread-safe. The logger callback MAY be invoked from any thread and MUST be reentrant with respect to the API (it must not call back into OVT). `ovt_set_logger` MUST be called before any other API call from any thread.
5. Strings are UTF-8, NUL-terminated, owned by the callee unless an `out` parameter says otherwise; strings returned by value are valid for the life of the handle.
6. No function blocks on a GUI event loop; long operations take an `ovt_Job*` (nullable) and check cancellation ≥ every 50 ms (§0.7 rule 2).
7. **Public-layout structs begin with `uint32_t struct_size` (D4 d).** Every struct with public layout that is passed by pointer across the ABI (`ovt_PlaneView`, `ovt_Picture`, `ovt_ColorDescription`, `ovt_HdrMeasurement`, `ovt_JobProgress`, `ovt_HrdOptions`, `ovt_BdResult`, `ovt_MapView`, `ovt_PlanePair`, and the plugin ABI structs `ovt_AnalysisCaps`, `ovt_AnalysisDecoderApi`, `ovt_MetricApi`) has `uint32_t struct_size` as its first member; the caller sets it to `sizeof(struct)` on input structs and the library sets it on output structs. New fields are only appended; a reader compares `struct_size` before touching an appended field, so a minor version may extend a struct without renaming it. The two value-type pairs `ovt_Buffer {data,size}` and `ovt_FrameId {decode_index, display_index}` are exempt (they are never extended; `sizeof(ovt_Buffer) == 16` is asserted in `test_c_api_abi.cpp`). Chapter 0 §0.5.6 (`ovt_ColorDescription`) and §0.6.2/§0.6.3 (plugin ABI structs) follow the same rule.
8. **No exceptions cross the C ABI (D4 c).** Every exported function is implemented as `noexcept` in `core/src/api/*.cpp`; the implementation catches `ovt::ParseError`, `std::bad_alloc` and every other exception at the boundary and converts it into `OVT_E_CORRUPT`, `OVT_E_NOMEM` or `OVT_E_INTERNAL` with the thread-local last-error `Diagnostic` set (§12.3.3). Callbacks supplied by the caller (`ovt_log_fn`, `ovt_progress_fn`, `ovt_analysis_frame_fn`, `ovt_MetricsEventFn`) MUST NOT throw into the library; the C++ side invokes them inside a `noexcept` wrapper that terminates on a throw.
9. **Documentation.** Every function carries a Doxygen comment that states the ownership of every pointer parameter and return value in the words of rule 3 (chapter 04 FR-API-001; `doxygen -q` with `WARN_AS_ERROR = YES` is a CI gate, WI-1.511).

### 12.3.2 `ovt/version.h`

```c
#define OVT_API_VERSION_MAJOR 1
#define OVT_API_VERSION_MINOR 0
#define OVT_API_VERSION_PATCH 0
#define OVT_API_VERSION ((OVT_API_VERSION_MAJOR << 16) | (OVT_API_VERSION_MINOR << 8) | OVT_API_VERSION_PATCH)
/* Runtime values of the loaded library (may be newer than the headers compiled against). These are the
 * PRODUCT version from the repository's VERSION file (0.1.0 at WI-0.000; FR-GEN-010), not OVT_API_VERSION. */
void        ovt_version(int* out_major, int* out_minor, int* out_patch);       /* never fails */
const char* ovt_version_string(void);                                           /* product version, e.g. "0.1.0" */
const char* ovt_build_info_json(void);  /* {"commit":"...","profile":"release","platform":"macos-arm64","features":["aom","vmaf",...]} */
/* Returns OVT_OK if the running library is ABI-compatible with the given header version (same major, minor >= requested). */
ovt_status  ovt_check_version(uint32_t header_api_version);
```

**ABI stability policy.** The C API follows semantic versioning on `OVT_API_VERSION` independently of the product version printed by `ovt --version`. The two differ until the product reaches 1.0: the product version is `0.1.0` (the `VERSION` file, chapter 13 WI-0.000) while `OVT_API_VERSION_*` is `1.0.0` as defined above. `ovt_version()` / `ovt_version_string()` return the **product** version; `ovt_check_version(OVT_API_VERSION)` compares only the API version; the SONAME follows the API major (`libovt_core.so.1`); and `ovt --version` prints `ovt 0.1.0+<sha> (core 0.1.0, api 1.0.0)` per §12.2 (issue #4). *Patch*: no header change. *Minor*: new functions, new enum values at the end, new structs; existing symbols and struct layouts unchanged; new FlatBuffers fields only appended (FlatBuffers forward/backward compatibility). *Major*: anything else. The shared library SONAME is `libovt_core.so.<major>` / `libovt_core.<major>.dylib`; all symbols are exported via a version script `core/ovt_core.map` (Linux) / exported-symbols list (macOS) and `OVT_API` visibility macro; nothing not in these headers is exported. Plugin ABIs (`abi/*.h`) are versioned separately by `OVT_ANALYSIS_ABI_VERSION` / `OVT_METRIC_ABI_VERSION` and are never modified: a change produces `analysis_decoder_v2.h` with a new entry point `ovt_analysis_decoder_v2()`; the host keeps loading v1 plugins for at least two product major versions. Deprecations are marked with `OVT_DEPRECATED("use ovt_x")` and kept for one major version.

### 12.3.3 `ovt/status.h`

```c
typedef enum ovt_status { OVT_OK = 0, OVT_E_INVALID_ARG, OVT_E_IO, OVT_E_UNSUPPORTED, OVT_E_CORRUPT,
                          OVT_E_NOMEM, OVT_E_CANCELLED, OVT_E_PLUGIN, OVT_E_INTERNAL } ovt_status;
const char* ovt_status_string(ovt_status);             /* "OVT_E_CORRUPT" */
typedef struct ovt_Buffer { const uint8_t* data; size_t size; } ovt_Buffer;
void ovt_buffer_free(ovt_Buffer* buf);                 /* only for caller-owned buffers (rule 12.3.1.3b); zeroes *buf */
/* Last error detail for the calling thread: a Diagnostic FlatBuffer (JSON via ovt_last_error_json), cleared by every API call. */
ovt_Buffer  ovt_last_error(void);                      /* by-value return: owned by the library (rule 3b), valid until the next API call on this thread */
const char* ovt_last_error_json(void);
```

### 12.3.4 `ovt/log.h`

```c
typedef void (*ovt_log_fn)(int level /*0 trace,1 debug,2 info,3 warn,4 error,5 fatal*/, const char* module, const char* msg, void* user);
void ovt_set_logger(ovt_log_fn, void* user);           /* §0.6.1 signature; NULL restores the default stderr logger */
void ovt_set_log_level(int level);                      /* messages below are not formatted at all (cheap) */
```

### 12.3.5 `ovt/jobs.h`

```c
typedef struct ovt_Job ovt_Job;
typedef struct ovt_JobProgress { uint32_t struct_size; double fraction /*0..1 or -1 unknown*/; uint64_t done; uint64_t total; const char* stage; } ovt_JobProgress;
typedef void (*ovt_progress_fn)(const ovt_JobProgress* p, void* user);   /* called from a worker thread, at most every 100 ms */

ovt_status ovt_job_create(ovt_Job** out);
void       ovt_job_set_progress_callback(ovt_Job*, ovt_progress_fn fn, void* user);   /* chapter 04 FR-API-004 */
void       ovt_job_cancel(ovt_Job*);                    /* thread-safe; idempotent; the running call returns OVT_E_CANCELLED within 50 ms of work */
int        ovt_job_is_cancelled(const ovt_Job*);        /* thread-safe */
void       ovt_job_progress(const ovt_Job*, ovt_JobProgress* out);   /* thread-safe snapshot; out->stage valid until job freed */
void       ovt_job_reset(ovt_Job*);                     /* clears cancelled flag and progress; only when no call is using the job */
void       ovt_job_free(ovt_Job*);                      /* must not be running */
/* Global worker pool used by all handles in this process. 0 = hardware concurrency capped at 16. Call before creating handles. */
ovt_status ovt_set_thread_count(int threads);
int        ovt_thread_count(void);
```

A job is passed to one blocking call at a time; passing the same job to two concurrent calls is undefined. Cancellation is cooperative: a call that returns `OVT_E_CANCELLED` leaves its handle in a valid state (the stream can be re-used; partial results are discarded except where documented, e.g. `ovt_stream_build_index` keeps the partial index in the cache marked incomplete).

### 12.3.6 `ovt/io.h`

```c
typedef struct ovt_File ovt_File;
ovt_status ovt_file_open(const char* path, ovt_File** out);                   /* UTF-8 path; read-only; pread()-based with a 64 KiB-page read cache (chapter 08 §8.1 rule 5: no mmap, so bytes_read statistics stay exact); returns IoError::code (chapter 07 §7.1.3), not a flat OVT_E_IO */
/* Opt-in mmap for bulk readers (metrics, Python): disables bytes_read accounting; never used by the container module. */
ovt_status ovt_file_open_mapped(const char* path, ovt_File** out);
/* take_ownership=1: the ovt_File owns fd and closes it, on failure too (except EBADF) — Android: pass the result of ParcelFileDescriptor.detachFd(), never getFd(); take_ownership=0: the library dup()s fd (F_DUPFD_CLOEXEC) and adopts the copy. display_name (nullable) is what ovt_file_display_name returns, else "fd:<n>" with the caller's fd number. FIFOs and sockets are rejected (OVT_E_INVALID_ARG). Mirrors chapter 07 §7.1.2 open_fd() (issue #110). */
ovt_status ovt_file_open_fd(int fd, uint64_t size /*0 or UINT64_MAX = unknown, fstat()*/, int take_ownership, const char* display_name, ovt_File** out);
ovt_status ovt_file_open_memory(const uint8_t* data, size_t size, int copy, ovt_File** out);   /* copy=0: caller keeps data alive */
uint64_t   ovt_file_size(const ovt_File*);
ovt_status ovt_file_read(ovt_File*, uint64_t offset, uint8_t* dst, size_t len, size_t* out_read);
const char* ovt_file_display_name(const ovt_File*);     /* path, "fd:<n>" or "memory" */
void       ovt_file_close(ovt_File*);                   /* only after all containers on it are closed */
```

### 12.3.7 `ovt/container.h`

```c
typedef struct ovt_Container ovt_Container;
ovt_status ovt_container_open(ovt_File*, ovt_Container** out);                 /* sniffs format; builds top-level tree lazily */
ovt_status ovt_container_open_format(ovt_File*, const char* format /*"isobmff","ivf","annexb","obu","y4m","raw"; phase 4: "ts","mkv"*/,
                                     const char* raw_format /*nullable; "1920x1080:yuv420p10le:30000/1001" (§12.1.5 grammar; required for "raw")*/,
                                     const char* codec_hint /*nullable "h264","h265","av1"*/, ovt_Container** out);
ovt_status ovt_container_open_manifest(const char* url_or_path, const char* variant_selector /*nullable; "1" = Representation/variant index*/, ovt_Container** out);   /* phase 4, DASH MPD / HLS playlist (chapter 08 §8.6.9, WI-4.210) */
const char* ovt_container_format(const ovt_Container*);
ovt_Buffer ovt_container_tree(ovt_Container*);                                  /* ContainerTree; by-value → handle-owned (rule 3b); boxes not yet expanded have fields == [] and children == [] and carry Diagnostic CNT-0007 (lazy, chapter 08 §8.5) */
ovt_status ovt_container_expand(ovt_Container*, uint64_t box_offset);           /* decodes fields/children of that box; tree buffer invalidated */
ovt_status ovt_container_expand_all(ovt_Container*, ovt_Job* job);              /* everything except mdat payloads */
int        ovt_container_track_count(const ovt_Container*);
ovt_status ovt_container_track_info(ovt_Container*, int track_index, ovt_Buffer* out /* TrackInfo */);
ovt_status ovt_container_sample_table(ovt_Container*, int track_index, uint32_t first, uint32_t count /*0 = all*/, ovt_Buffer* out /* SampleTable */);
ovt_status ovt_container_fragments(ovt_Container*, int track_index, ovt_Job* job, ovt_Buffer* out /* FragmentTimeline */);
ovt_status ovt_container_encryption(ovt_Container*, int track_index, uint64_t first, uint64_t count /*0 = all*/, ovt_Buffer* out /* EncryptionInfo */);
ovt_status ovt_container_hdr_metadata(ovt_Container*, int track_index, ovt_Job* job, ovt_Buffer* out /* HdrMetadata */, ovt_ColorDescription* out_color /*nullable*/);
ovt_status ovt_container_items(ovt_Container*, ovt_Buffer* out /* ContainerRoot.items as ItemInfoList */);           /* phase 4, HEIF/AVIF (WI-4.213) */
ovt_status ovt_container_item_bytes(ovt_Container*, uint32_t item_id, ovt_Buffer* out);                              /* phase 4 */
ovt_status ovt_container_validate(ovt_Container*, ovt_Buffer* out /* [Diagnostic] as DiagnosticList */);
ovt_status ovt_container_list_diagnostics(ovt_Buffer* out /* DiagnosticRegistry: the CNT-* table of chapter 08 §8.5 */);   /* no handle; `ovt probe --list-diagnostics` */
/* Byte service (chapter 08 §8.8): the only functions that may be called concurrently on one handle (rule 4). */
ovt_status ovt_container_bytes(ovt_Container*, uint64_t offset, uint32_t len /*≤ 1 MiB*/, ovt_Buffer* out);
ovt_status ovt_container_box_at(ovt_Container*, uint64_t offset, uint64_t* out_box_offset, int* out_depth);   /* innermost box containing offset */
ovt_status ovt_container_segment_for_offset(ovt_Container*, uint64_t offset, ovt_Buffer* out_json);          /* {"kind":"box|sample|fragment|gap","track":n,"index":n,"span":{...}} */
uint64_t   ovt_container_size(const ovt_Container*);                                                         /* file size in bytes */
ovt_status ovt_container_stats_json(ovt_Container*, ovt_Buffer* out_json);                                   /* {"bytes_read":n,"boxes_parsed":n,"expanded":n,...} */
void       ovt_container_close(ovt_Container*);                                 /* only after all streams on it are closed */
```

Every out-buffer above is caller-owned (rule 3a); `ovt_container_tree` is the single by-value return (rule 3b). Signatures are identical to chapter 08 §8.8 and its work items; chapter 08 states the semantics, this list is the exported symbol set (`core/ovt_core.map`, WI-1.511).

### 12.3.8 `ovt/syntax.h` and `ovt/index.h`

```c
/* Stateless parsing of one unit (for tools and tests; the stream API below does this for you). */
ovt_status ovt_syntax_parse_unit(uint8_t codec /*ovt.fb.Codec*/, const uint8_t* unit, size_t len, int has_prefix /*start code or OBU size field present*/,
                                 const uint8_t* active_param_sets_fb /*nullable: StreamIndex.parameter_sets buffer for slice headers*/, size_t ps_len,
                                 ovt_Buffer* out /* SyntaxNode */);
ovt_status ovt_syntax_trace(const uint8_t* syntax_node_fb, size_t len, ovt_Buffer* out_text);   /* §12.2.3.1 text */
ovt_status ovt_syntax_to_json(const uint8_t* fb, size_t len, const char* root_type /*"SyntaxNode","StreamIndex",...*/, int pretty, ovt_Buffer* out_text);
ovt_status ovt_fb_to_json(const uint8_t* fb, size_t len, const char* root_type, int pretty, ovt_Buffer* out_text);        /* generic: any root table */
ovt_status ovt_json_to_fb(const char* json, const char* root_type, ovt_Buffer* out_fb);

typedef struct ovt_Stream ovt_Stream;
ovt_status ovt_stream_open(ovt_Container*, int track_index, ovt_Stream** out);
uint8_t    ovt_stream_codec(const ovt_Stream*);                                  /* ovt.fb.Codec */
ovt_status ovt_stream_build_index(ovt_Stream*, ovt_Job* job, ovt_Buffer* out_index /* StreamIndex, caller-owned */);  /* uses/updates cache */
ovt_Buffer ovt_stream_index(ovt_Stream*);                                        /* last built StreamIndex; by-value → handle-owned (rule 3b, D4); .data == NULL before build_index */
ovt_status ovt_stream_index_summary(ovt_Stream*, ovt_Buffer* out /* IndexSummary */);
ovt_status ovt_stream_unit_bytes(ovt_Stream*, uint32_t unit_index, ovt_Buffer* out_raw, ovt_Buffer* out_rbsp);  /* either may be NULL */
ovt_status ovt_stream_unit_syntax(ovt_Stream*, uint32_t unit_index, ovt_Buffer* out /* SyntaxNode; re-parses lazy trees, chapter 07 §7.4.5 */);
ovt_status ovt_stream_frame_info(ovt_Stream*, int decode_index, ovt_Buffer* out /* FrameInfo */);
int        ovt_stream_frame_count(const ovt_Stream*);                            /* -1 before build_index */
int        ovt_stream_display_to_decode(const ovt_Stream*, int display_index);   /* -1 if invalid */
int        ovt_stream_decode_to_display(const ovt_Stream*, int decode_index);    /* -1 if hidden */
int        ovt_stream_rap_before(const ovt_Stream*, int decode_index);           /* decode_index of nearest RAP <= given */
ovt_status ovt_stream_color(ovt_Stream*, ovt_ColorDescription* out);             /* merged container + bitstream description; conflicts logged */
ovt_status ovt_stream_extract(ovt_Stream*, const uint32_t* unit_indices, size_t count, const char* to_format /*"annexb","obu","ivf"*/, ovt_Buffer* out_bytes);
ovt_status ovt_stream_min_cvs_units(ovt_Stream*, int decode_index, int with_sei, ovt_Buffer* out /* UIntList of unit indices */);
void       ovt_stream_close(ovt_Stream*);

/* ovt/validate.h — chapter 06 §6.9 rules (WI-0.086 implements; this is the exported signature). Violations come back as a
   DiagnosticList with code "V-nnn" (severity per rule), path in FlatBuffers locator syntax ("grid.qp[1234]"). */
ovt_status ovt_validate_frame_analysis(const uint8_t* fb, size_t len, const uint8_t* expected_source_hash /*nullable, 32 bytes*/, int codec_override /*-1 none, else ovt.fb.Codec*/, ovt_Buffer* out /* DiagnosticList */);
ovt_status ovt_validate_stream_index(const uint8_t* fb, size_t len, ovt_Buffer* out /* DiagnosticList */);

/* ovt/hrd.h — chapter 07 §7.4.8 (WI-4.112 implements; phase 4). The index must have been built. */
typedef struct ovt_HrdOptions { uint32_t struct_size; int sched_sel_idx; int nal_hrd /*1 = type II (NAL HRD), 0 = type I (VCL HRD)*/; int64_t initial_removal_delay_override /*90 kHz ticks; -1 = none*/; } ovt_HrdOptions;
ovt_status ovt_index_hrd_trace(ovt_Stream*, const ovt_HrdOptions* opts /*nullable = defaults*/, ovt_Buffer* out /* HrdTrace */);   /* OVT_E_UNSUPPORTED for AV1 (DEC-HRD-0001) */
```

### 12.3.9 `ovt/decode.h`

```c
typedef struct ovt_PlaneView {
  uint32_t struct_size;       /* = sizeof(ovt_PlaneView) (rule 7) */
  const uint8_t* data;        /* first sample of row 0 */
  uint32_t width, height;     /* in samples of this plane */
  uint32_t stride_bytes;
  uint8_t  bit_depth;         /* 8..16 */
  uint8_t  sample_bytes;      /* 1 or 2 (little-endian), 4 for Coefficients (int32) */
  uint8_t  is_signed;         /* 1 for Residual / Coefficients */
  uint8_t  plane;             /* 0 Y, 1 U, 2 V */
  uint8_t  stage;             /* ovt.fb.PlaneStage */
  uint8_t  reserved[3];
} ovt_PlaneView;

typedef struct ovt_FrameId { int32_t decode_index; int32_t display_index; } ovt_FrameId;

typedef struct ovt_Picture {
  uint32_t struct_size;       /* = sizeof(ovt_Picture) (rule 7) */
  ovt_PlaneView planes[3];    /* planes[1..2] zero for 4:0:0 */
  uint8_t chroma_format;      /* 0=400 1=420 2=422 3=444 */
  ovt_ColorDescription color;
  ovt_FrameId id;
  int64_t pts; int32_t timebase_num, timebase_den;
  int32_t crop_x, crop_y, crop_w, crop_h;   /* conformance window in luma samples; planes[] are the coded (uncropped) picture */
  uint8_t hw_decoded;         /* 1 if produced by a hardware decoder (not bit-exact across devices) */
  void* opaque;               /* library private: owner of the plane memory and the reference count */
} ovt_Picture;
/* Pictures are reference counted (D4 b). ovt_decoder_next/ovt_decoder_frame hand the caller ONE reference; the picture and its
   planes stay valid across any further decoder calls (including close) until that reference is released. */
void ovt_picture_ref(ovt_Picture*);           /* +1; thread-safe */
void ovt_picture_release(ovt_Picture*);       /* -1; frees planes and the struct when it reaches 0; NULL is a no-op */
/* Per-plane export on CPU planes (chapter 10 §10.6.3): format "pgm" (P5, maxval 2^bd−1, 16-bit big-endian when bd > 8) or "y4m"
   (single-frame Y4M with C420jpeg/C420mpeg2/C420paldv/C422/C444 + XYSCSS=/XCOLORRANGE=). plane < 0 exports all planes (y4m only). */
ovt_status ovt_picture_export_plane(const ovt_Picture*, int plane, const char* format, const char* path);

typedef struct ovt_Decoder ovt_Decoder;
ovt_status ovt_decoder_open(ovt_Stream*, int prefer_hw /*0 software, 1 hw if available, 2 hw required*/, ovt_Decoder** out);
ovt_status ovt_decoder_open_with(ovt_Stream*, const char* decoder_id /*"ffmpeg:hevc_videotoolbox","dav1d","ffmpeg:hevc"*/, ovt_Decoder** out);
const char* ovt_decoder_id(const ovt_Decoder*);
ovt_status ovt_decoder_seek(ovt_Decoder*, int display_index);          /* seeks to the RAP <= target; subsequent next() returns from target */
ovt_status ovt_decoder_next(ovt_Decoder*, ovt_Picture** out);          /* backbone §0.6.1 signature; OVT_OK with *out == NULL at end of stream; *out carries one reference owned by the caller (ovt_picture_release) */
ovt_status ovt_decoder_frame(ovt_Decoder*, int display_index, ovt_Picture** out);  /* seek + decode shortcut; same ownership as ovt_decoder_next */
void       ovt_decoder_close(ovt_Decoder*);   /* pictures still referenced by the caller remain valid (they own their planes) */
```

The decoder keeps at most `max(dpb_size, 4)` pictures of its own alive for reference; a caller that holds many references holds that memory (§0.7 rule 3 budgets apply to the caller). `ovt_Picture.planes[]` point into the picture's own buffers, never into decoder-internal reusable frames: hardware decoders copy out on `next()` (chapter 07 §7.5), software decoders hand over their reference-counted frame (`AVFrame`/`Dav1dPicture`) through `opaque`.

### 12.3.10 `ovt/analysis.h`

```c
typedef struct ovt_Analysis ovt_Analysis;
ovt_status ovt_analysis_open(ovt_Stream*, const char* plugin_id /*nullable = default for codec*/, ovt_Analysis** out);
const char* ovt_analysis_plugin_id(const ovt_Analysis*);
const ovt_AnalysisCaps* ovt_analysis_caps(const ovt_Analysis*);
/* Decodes from the nearest RAP up to decode_index (GOP-local, ADR-009), caches every FrameAnalysis on the way, returns the requested one. */
ovt_status ovt_analysis_frame(ovt_Analysis*, int decode_index, uint32_t plane_stage_mask /*bit per ovt.fb.PlaneStage*/, ovt_Job*, ovt_Buffer* out /* FrameAnalysis, caller-owned */);
/* Range variant: callback per frame in decode order; returning non-zero from cb stops with OVT_E_CANCELLED. */
typedef int (*ovt_analysis_frame_fn)(const ovt_Buffer* frame_analysis, void* user);
ovt_status ovt_analysis_range(ovt_Analysis*, int first_decode_index, int last_decode_index, uint32_t plane_stage_mask, ovt_Job*, ovt_analysis_frame_fn cb, void* user);
/* Planes referenced by FrameAnalysis.planes[].blob_id; valid until the next ovt_analysis_frame/range call or close (LRU of 4 frames, §0.7 rule 3). */
ovt_status ovt_analysis_plane(ovt_Analysis*, const char* blob_id, ovt_PlaneView* out);
ovt_status ovt_analysis_plane_copy(ovt_Analysis*, const char* blob_id, ovt_Buffer* out /* caller-owned tightly packed copy */);
void       ovt_analysis_close(ovt_Analysis*);
```

**Diagnostic registry (ANA-LRU, owned here; chapter 07 §7.9.5 points to this row).**

| Code | Severity | Message template | When raised | Spec reference |
|---|---|---|---|---|
| `ANA-LRU-0001` | Error | `plane blob '{blob_id}' is no longer resident (evicted from the {n}-frame plane LRU)` | `ovt_analysis_plane`/`ovt_analysis_plane_copy` called with a `blob_id` whose frame was evicted by a later `ovt_analysis_frame`/`ovt_analysis_range` call; returns `OVT_E_INVALID_ARG` and the diagnostic in `ovt_last_error_json` | §0.7.3 rule 3 (planes LRU of 4 frames on desktop, 2 on Android; grids LRU of 64 frames, 16 on Android; D5) |

### 12.3.11 `ovt/plugins.h`

```c
/* Search paths, in order (the one list of chapter 07 §7.5.1, issue #212): (1) paths added here, (2) $OVT_PLUGIN_PATH
   (if set and not ending in ':' it replaces the platform defaults; a trailing ':' appends them), (3) platform defaults:
   macOS   <bundle>/Contents/PlugIns/ovt, <bundle>/Contents/PlugIns, <exe>/../lib/ovt/plugins, ~/Library/Application Support/ovt/plugins, /usr/local/lib/ovt/plugins, /opt/homebrew/lib/ovt/plugins
   Linux   <exe>/../lib/ovt/plugins, $XDG_DATA_HOME/ovt/plugins, /usr/local/lib/ovt/plugins, /usr/lib/ovt/plugins, /usr/lib64/ovt/plugins, /usr/lib/<multiarch>/ovt/plugins, /app/lib/ovt/plugins (Flatpak), $APPDIR/usr/lib/ovt/plugins (AppImage)
   Android <nativeLibraryDir>, <filesDir>/plugins  (chapter 13 §13.4.6 rules) */
ovt_status ovt_plugins_add_search_path(const char* dir);
ovt_status ovt_plugins_scan(void);                                       /* dlopen()s every libovt_plugin_*.{so,dylib}; idempotent; thread-safe */
ovt_status ovt_analysis_rescan_plugins(void);                            /* chapter 07 §7.6 spelling: forgets the cached directory listing and calls ovt_plugins_scan() again (already-loaded plugins stay loaded) */
ovt_status ovt_plugins_load_file(const char* path, ovt_Buffer* out /* PluginInfo */);
/* Static registration for builds that link plugins directly (Android codec pack, python wheel, tests). Must be called before scan/first use. */
ovt_status ovt_plugins_register_analysis_decoder(const ovt_AnalysisDecoderApi* api, const char* origin /*"(built-in)"*/);
ovt_status ovt_plugins_register_metric(const ovt_MetricApi* api, const char* origin);
ovt_status ovt_plugins_list(ovt_Buffer* out /* PluginList */);
ovt_status ovt_plugins_set_default(uint8_t codec, const char* analysis_plugin_id);   /* default order otherwise: AV1 aom; H265 de265 > hm > ffmpeg; H264 ffmpeg; then the first registered plugin for the codec, never ffmpeg_hevc (opt-in only, chapter 07 §7.5.1; issues #212, #370) */
const char* ovt_plugins_default(uint8_t codec);
```

Loading rules: the file MUST export exactly one of `ovt_analysis_decoder_v1` / `ovt_metric_v1` (checked with `dlsym`); `abi_version` MUST equal the host's constant or the plugin is rejected with `OVT_E_PLUGIN` and Diagnostic `ANA-PLG-0001`; a second file with the same `<id>` (checked by file name before `dlopen`) loses to the earlier one (first path wins) with Diagnostic `ANA-PLG-0002`; several plugins for one codec may coexist (issue #212). The FFmpeg plugin may additionally export `const char* ovt_plugin_describe(void)` (chapter 07 §7.6.3), which the host may `dlsym` for `ovt plugins` and logs (issue #261). The plugin-host diagnostics `ANA-PLG-0001` (ABI version mismatch), `ANA-PLG-0002` (duplicate plugin id), `ANA-PLG-0003` (Android: codec-pack signature mismatch, chapter 13 §13.4.6) and `ANA-PLG-0004` (macOS/Linux: plugin file world-writable or not owned by the user, chapter 13 §13.5.3) are registered in chapter 07 §7.9 (D2 family `ANA`); a missing decoder plugin for a codec is `DEC-PLUGIN-0001` (chapter 07 §7.9). Plugins are never unloaded before process exit (decoders may keep static state).

### 12.3.12 `ovt/color.h`

```c
/* ovt_ColorDescription is defined in backbone §0.5.6 and lives here. */
typedef struct ovt_HdrMeasurement { uint32_t struct_size; float max_nits, fall_nits, p99_nits, min_nits, pct_above_threshold; uint32_t histogram[64]; /* log2 bins 0.0001..10000 nits */ } ovt_HdrMeasurement;
ovt_status ovt_hdr_measure(const ovt_Picture*, float threshold_nits, ovt_HdrMeasurement* out);   /* CPU; PQ/HLG/SDR-as-203-nits per chapter 10 */
ovt_status ovt_color_to_json(const ovt_ColorDescription*, ovt_Buffer* out_text);
ovt_status ovt_color_from_json(const char* json, ovt_ColorDescription* out);
const char* ovt_color_primaries_name(uint8_t), *ovt_color_transfer_name(uint8_t), *ovt_color_matrix_name(uint8_t);   /* H.273 names */
```

### 12.3.13 `ovt/metrics.h`

The metrics C API is specified by chapter 09 §9.10 (the metrics module owns it); this block is the exported signature set and MUST stay identical to chapter 09's. `spec_json` is the chapter 09 `MetricsJobSpec` (`{"reference":..., "distorted":[...], "metrics":[{"id":"vmaf","options":{"model":"version=vmaf_v0.6.1"}}, {"id":"psnr"}], "normalize":{...NormalizationConfig...}, "align":"auto"|int|"none", "maps":true, "subsample":1, "max_frames":0, "frames":"0-299"}`), parsed strictly (unknown keys → `OVT_E_INVALID_ARG`; chapter 04 FR-API-010).

```c
typedef struct ovt_MetricsJob ovt_MetricsJob;                /* opaque; one job = one reference × N distorted × metrics × models */
typedef struct ovt_MapView  { uint32_t struct_size; const struct ovt_MapBlobHeader* header; const float* values; } ovt_MapView;   /* MapBlobHeader: chapter 09 §9.7 */
typedef struct ovt_PlanePair { uint32_t struct_size; ovt_PlaneView ref, dist; } ovt_PlanePair;
typedef void (*ovt_MetricsEventFn)(const char* event_json, void* user);   /* JobEvent as JSON (state, frames_done/total, eta_seconds), ≤ 10 Hz, worker thread */

ovt_status ovt_metrics_list_descriptors(ovt_Buffer* out_descriptors_json);               /* JSON array of MetricDescriptor (built-in, plugin, sidecar) */
ovt_status ovt_metrics_job_create(const char* spec_json, ovt_MetricsEventFn on_event /*nullable*/, void* user, ovt_MetricsJob** out_job);
ovt_status ovt_metrics_job_run(ovt_MetricsJob* job, ovt_Job* cancel /*nullable*/);       /* blocking; call from a worker thread */
ovt_status ovt_metrics_job_results(const ovt_MetricsJob* job, ovt_Buffer* out_report_fb); /* MetricsReport, file_identifier "OVTR" */
ovt_status ovt_metrics_job_alignment(const ovt_MetricsJob* job, ovt_Buffer* out_alignment_json); /* {"offset_frames":o,"confidence":c,"shift_x":0,"shift_y":0}; valid after the Aligning state */
ovt_status ovt_metrics_job_set_alignment(ovt_MetricsJob* job, int offset_frames, int shift_x, int shift_y);   /* before run(); WI-4.309 */
ovt_status ovt_metrics_job_planes(ovt_MetricsJob* job, uint32_t display_index, uint32_t domain /*ovt.fb.WorkingDomain*/, ovt_PlanePair* out, ovt_Buffer* out_storage);   /* normalised planes for the GUI compare view */
ovt_status ovt_metrics_map(const ovt_MetricsJob* job, const char* map_blob_id, ovt_MapView* out, ovt_Buffer* out_storage);   /* per-frame quality map (FrameScore.map_blob_id) */
void       ovt_metrics_job_release(ovt_MetricsJob* job);
/* In-memory path (Python score_frames, unit tests; WI-3.533): frames pushed in display order into a job created with "reference":"push","distorted":["push"]. */
ovt_status ovt_metrics_push(ovt_MetricsJob* job, const ovt_Picture* ref /*nullable for NR-only*/, const ovt_Picture* dist);
ovt_status ovt_metrics_finish(ovt_MetricsJob* job, ovt_Buffer* out_report_fb);          /* ends the push sequence; equivalent to run()+results() */
/* Exporters over a finished MetricsReport buffer (chapter 09 §9.9). */
ovt_status ovt_metrics_export_csv(const void* report_fb, size_t report_size, const char* base_path);
ovt_status ovt_metrics_export_json(const void* report_fb, size_t report_size, int pretty, ovt_Buffer* out);
ovt_status ovt_metrics_export_vmaf_json(const void* report_fb, size_t report_size, ovt_Buffer* out);
ovt_status ovt_metrics_export_fb(const void* report_fb, size_t report_size, int zstd_level, ovt_Buffer* out);
/* BD-rate (chapter 09 §9.10, maths of WI-3.313). JSON form used by the GUI and Python; curves_json = [{"name":..,"points":[{"kbps":..,"q":..}]}, ...] (anchor first). */
ovt_status ovt_metrics_bd(const char* curves_json, const char* options_json /*{"interpolator":"pchip","range":"overlap","hull":false,"targets":[..]}*/, ovt_Buffer* out_bd_results_fb /* [BdRateResult] + hull */);

/* Plain-C convenience form of the same computation for two curves (WI-3.531). */
typedef struct ovt_BdResult {               /* plain-C mirror of BdRateResult (chapter 06 §6.5); same field names */
  uint32_t struct_size;
  double bd_rate_percent, bd_quality;
  double overlap_low, overlap_high, overlap_fraction;
  int interpolator;                          /* ovt.fb.Interpolator value */
  int log_base_bitrate;                      /* 1 = integrated over log10(bitrate) */
  const char* range_mode;                    /* "overlap" | "anchor"; static strings */
  const char* anchor_name; const char* test_name;   /* point into the caller's input strings */
  ovt_Buffer diagnostics;                    /* DiagnosticList FlatBuffer (VQ-BD-0002 fewer points than the interpolator needs, VQ-BD-0003 non-monotonic, VQ-BD-0004 extrapolated; registry chapter 09 §9.11.4); caller-owned (rule 3a): ovt_buffer_free */
} ovt_BdResult;
ovt_status ovt_bdrate(const double* anchor_kbps, const double* anchor_q, size_t n_anchor,
                      const double* test_kbps, const double* test_q, size_t n_test,
                      const char* interpolator /*"pchip","cubic","akima"*/, const char* range_mode /*"overlap","anchor"*/, ovt_BdResult* out /*struct_size set by caller*/);
```

### 12.3.14 `ovt/cache.h`

```c
ovt_status ovt_cache_set_dir(const char* dir);          /* before any stream is opened; NULL = platform default */
const char* ovt_cache_dir(void);
ovt_status ovt_cache_set_enabled(int enabled);
ovt_status ovt_cache_set_max_bytes(uint64_t max_bytes); /* default 20 GiB desktop, 2 GiB Android (chapter 06 §6.7.4, NFR-010); LRU eviction */
ovt_status ovt_cache_report(ovt_Buffer* out /* CacheReport */);
ovt_status ovt_cache_clear(const char* kind /*nullable*/, const char* source_path /*nullable*/, int older_than_days /*0 = any*/);

/* Object API over one cache root (chapter 06 §6.7; WI-0.091 implements). The process-wide functions above operate on the default cache;
   `ovt cache import` and tests use explicit handles. Files and names exactly as chapter 06 §6.7.1. */
typedef struct ovt_Cache ovt_Cache;
typedef struct ovt_CacheEntry ovt_CacheEntry;                 /* one source_hash directory */
ovt_status ovt_cache_open(const char* root /*nullable = default dir*/, const char* opts_json /*nullable: {"max_bytes":n,"read_only":false}*/, ovt_Cache** out);
ovt_status ovt_cache_entry(ovt_Cache*, const uint8_t source_hash[32], ovt_CacheEntry** out);      /* creates the entry directory if absent */
ovt_status ovt_cache_put_index(ovt_CacheEntry*, const uint8_t* stream_index_fb, size_t len);
ovt_status ovt_cache_get_index(ovt_CacheEntry*, ovt_Buffer* out);                                /* OVT_E_IO when absent */
ovt_status ovt_cache_put_frame(ovt_CacheEntry*, uint32_t decode_index, const uint8_t* frame_analysis_fb, size_t len);
ovt_status ovt_cache_get_frame(ovt_CacheEntry*, uint32_t decode_index, ovt_Buffer* out);
ovt_status ovt_cache_put_plane(ovt_CacheEntry*, uint32_t decode_index, uint8_t stage /*ovt.fb.PlaneStage*/, uint8_t plane, const ovt_PlaneView* view);
ovt_status ovt_cache_get_plane(ovt_CacheEntry*, uint32_t decode_index, uint8_t stage, uint8_t plane, ovt_Buffer* out_storage, ovt_PlaneView* out_view);
ovt_status ovt_cache_put_report(ovt_CacheEntry*, const char* kind /*"metrics","hdr","verify"*/, const uint8_t* fb, size_t len);
ovt_status ovt_cache_get_report(ovt_CacheEntry*, const char* kind, ovt_Buffer* out);
void       ovt_cache_entry_release(ovt_CacheEntry*);
void       ovt_cache_close(ovt_Cache*);                      /* flushes meta.json; entries must be released first */
```

### 12.3.15 Minimal C example (compiled in CI as `core/tests/api_example.c`)

```c
#include <ovt/ovt.h>
#include <stdio.h>
int main(int argc, char** argv) {
  if (ovt_check_version(OVT_API_VERSION) != OVT_OK) return 1;
  ovt_File* f; ovt_Container* c; ovt_Stream* s; ovt_Buffer idx = {0};
  if (ovt_file_open(argv[1], &f) != OVT_OK) { fprintf(stderr, "%s\n", ovt_last_error_json()); return 3; }
  ovt_container_open(f, &c);
  ovt_stream_open(c, 0, &s);
  ovt_Job* job; ovt_job_create(&job);
  ovt_status st = ovt_stream_build_index(s, job, &idx);
  if (st == OVT_OK) { printf("frames: %d\n", ovt_stream_frame_count(s)); ovt_buffer_free(&idx); }
  ovt_job_free(job); ovt_stream_close(s); ovt_container_close(c); ovt_file_close(f);
  return st == OVT_OK ? 0 : 5;
}
```

---

## 12.4 Python package `ovt`

### 12.4.1 Scope and packaging

| Item | Decision |
|---|---|
| Binding technology | **pybind11** (vcpkg port `pybind11`, BSD-3) over the **C API only** (no C++ internals), in `python/src/_core.cpp` → extension module `ovt._core`. Pure-Python layer `python/ovt/*.py` provides the object model. |
| Python versions | CPython 3.11, 3.12, 3.13 — **one wheel per interpreter version and platform** (`cibuildwheel` `CIBW_BUILD="cp311-* cp312-* cp313-*"`). pybind11 (verified 2.13) does not support the Limited API / abi3, so a single abi3 wheel is not possible; 3.11 is the oldest supported interpreter (D5). |
| Wheels | `macosx_13_0_arm64`, `macosx_13_0_x86_64` (`MACOSX_DEPLOYMENT_TARGET=13.0`, equal to the app's deployment target, chapter 13 §13.1), `manylinux_2_28_x86_64`, `manylinux_2_28_aarch64` — 12 files per release (3 interpreters × 4 platforms). manylinux_2_28 caps the system `libstdc++` at `GLIBCXX_3.4.25` (GCC 8), so the extension and the bundled `libovt_core` are built with `-static-libstdc++ -static-libgcc` and `auditwheel repair` is the gate. Built by **cibuildwheel** in CI (chapter 13 §13.7.1). **No Android wheel** (Android uses the Qt app; `ovt_py` is not built in android presets). Windows is a non-target. |
| Bundled native code | `libovt_core`, the LGPL FFmpeg build (demux + AV1/VP9 playback), `libovt_plugin_aom`, libvmaf, zimg, ONNX Runtime (x86_64/arm64 CPU EP). **No AVC/HEVC decoder plugins** (ADR-015): they are found via `OVT_PLUGIN_PATH` / `ovt.plugins.add_path()` or installed by the `ovt-codecs` optional extra that downloads the codec pack from the project's release page with SHA-256 verification (`pip install ovt[codecs]` runs nothing at install time; the download happens on first `ovt.plugins.install_codec_pack()` call and prints the patent notice). |
| Licence | `python/` is GPL-3.0-or-later (ADR-002); the wheel's `METADATA` `License-Expression: GPL-3.0-or-later`; `THIRD_PARTY_NOTICES.md` and AOM `PATENTS` in the wheel's `.dist-info/licenses/`. |
| Dependencies | `numpy >= 1.24` (required), `flatbuffers >= 24.3` (required, for generated readers), optional extras: `plot` (`matplotlib`), `pandas`, `codecs`. |
| Build | `pyproject.toml` with **scikit-build-core**; `cmake --preset <platform>-release -DOVT_BUILD_PYTHON=ON` builds `ovt_py` as target; `pip wheel python/` works from an already configured build tree when scikit-build-core's `build-dir` setting (environment variable `SKBUILD_BUILD_DIR`) points at it; otherwise scikit-build-core configures its own tree. |
| Typing | `python/ovt/py.typed` + stubs `python/ovt/_core.pyi` generated by `pybind11-stubgen` and checked into the repo; `mypy --strict python/` passes in CI. |
| Docs | Sphinx (`python/docs/`), `autodoc` + `napoleon`, published to `docs/python/`. Every public function has a docstring with an example. |

### 12.4.2 Object model (`python/ovt/__init__.py`)

```python
import ovt

ovt.__version__: str                      # == ovt_version_string()
ovt.api_version: tuple[int, int, int]

class OvtError(Exception):                # base; subclasses map 1:1 to ovt_status
    status: str                           # "OVT_E_CORRUPT"
    diagnostic: dict | None               # ovt_last_error_json()
class InvalidArgument(OvtError): ...
class IOError_(OvtError): ...             # exported as ovt.IOError
class Unsupported(OvtError): ...
class Corrupt(OvtError): ...
class Cancelled(OvtError): ...
class PluginError(OvtError): ...

def open(path: str | os.PathLike | int, *, format: str | None = None, raw: str | None = None, codec: str | None = None) -> "Container": ...
# int = an already open file descriptor (ovt_file_open_fd). Container is a context manager.

class Container:
    format: str                           # "isobmff", ...
    path: str
    tracks: list["TrackInfo"]             # TrackInfo is a dataclass generated from the FlatBuffers table
    def tree(self, *, expand: str | int | list[str] = "all") -> "BoxNode": ...   # typed view over ContainerTree
    def tree_json(self, **kw) -> dict: ...
    def samples(self, track: int = 0, first: int = 0, count: int | None = None) -> "SampleTable": ...
    def fragments(self) -> "FragmentTimeline": ...
    def hdr_metadata(self, track: int = 0) -> "HdrMetadata": ...
    def validate(self) -> list["Diagnostic"]: ...
    def stream(self, track: int = 0) -> "Stream": ...
    @property
    def video(self) -> "Stream": ...       # first video track
    def close(self) -> None: ...

class Stream:
    codec: str                             # "H264","H265","AV1"
    color: "ColorDescription"              # dataclass mirror of ovt_ColorDescription
    def index(self, *, rebuild: bool = False, progress: Callable[[Progress], None] | None = None) -> "StreamIndex": ...
    @property
    def frames(self) -> "FrameTable": ...   # lazily builds the index; sequence of FrameInfo, len(), [i] by display index, .decode(i), .to_pandas()
    def unit(self, index: int) -> "Unit": ...                 # .raw -> bytes, .rbsp -> bytes, .header -> SyntaxNode, .trace() -> str
    def units(self, *, type: str | None = None, frame: int | None = None) -> Iterator["Unit"]: ...
    def analyze(self, frame: int, *, plugin: str | None = None, planes: Iterable[str] = (), order: str = "display") -> "FrameAnalysis": ...
    def analyze_range(self, first: int, last: int, **kw) -> Iterator["FrameAnalysis"]: ...
    def decoder(self, *, hw: bool = False, id: str | None = None) -> "Decoder": ...
    def picture(self, frame: int, *, order: str = "display") -> "Picture": ...   # convenience: software decode of one frame
    def extract(self, *, units=None, frames=None, min_cvs: bool = True, to: str = "annexb") -> bytes: ...

class SyntaxNode:                          # tree with .name .descriptor .value .enum_name .derived .span (bit_offset, bit_length) .spec_ref .children .diagnostics
    def find(self, name: str) -> "SyntaxNode | None": ...
    def to_dict(self) -> dict: ...
    def trace(self) -> str: ...

class FrameAnalysis:
    frame: "FrameInfo"
    plugin_id: str; plugin_version: str
    grid: "CellGrid"                       # every vector is a numpy array of shape (cells_h, cells_w) with the FlatBuffers element dtype,
                                           # zero-copy over the FlatBuffer memory (read-only), or None when the plugin did not fill it
    blocks: "BlockTable"                   # columnar: .id .parent .kind .rect (N,4) .pred .qp .bits_x8 numpy arrays; .ext(i) -> dict (decoded nested table)
    mvs: "MotionVectorTable"               # .rect (N,4) .mv (N,2) .mvd (N,2) .ref .list .predictor (list[str]) .flags
    refs: list["RefSlot"]
    partitions: list["PartitionInfo"]
    symbols: "SymbolTable | None"
    qp_hist: np.ndarray                    # (256,) uint32
    diagnostics: list["Diagnostic"]
    def plane(self, stage: str = "recon", plane: int = 0) -> np.ndarray: ...   # 2-D view (H, W) uint8/uint16/int16/int32, zero-copy via buffer protocol
    def block_at(self, x: int, y: int, kind: str | None = None) -> "Block": ...
    def to_json(self) -> str: ...
    def to_yuview_csv(self, path: str, *, types: Iterable[str] | None = None, append: bool = False) -> None: ...
    @property
    def raw(self) -> memoryview: ...       # the FlatBuffer bytes (for saving / sending)

class Picture:
    y: np.ndarray; u: np.ndarray | None; v: np.ndarray | None    # 2-D views, (H, W), dtype uint8 or uint16; zero-copy, read-only
    color: "ColorDescription"; id: tuple[int, int]; pts: int; crop: tuple[int, int, int, int]
    def cropped(self) -> "Picture": ...
    def to_rgb(self, *, tone_map: str = "bt2390", nits: float = 203.0) -> np.ndarray: ...   # CPU, uint8 (H,W,3) — for notebooks, not for display accuracy
    def measure_hdr(self, threshold_nits: float = 1000.0) -> "HdrMeasurement": ...

class Decoder:                             # iterator of Picture in display order
    def seek(self, display_index: int) -> None: ...
    def __iter__(self) -> Iterator[Picture]: ...
    def __next__(self) -> Picture: ...

# --- metrics ---
ovt.metrics.descriptors() -> list[MetricDescriptor]              # ovt_metrics_list_descriptors
ovt.metrics.run(reference: str | Stream | None, distorted: str | Stream | Sequence[str | Stream], *, metrics: Sequence[str] = ("psnr", "ssim"),
                models: Sequence[str] = ("version=vmaf_v0.6.1",), normalize: dict | None = None, align: str | int = "auto",
                maps: bool = False, subsample: int = 1, max_frames: int = 0, threads: int = 0,
                progress: Callable[[Progress], None] | None = None) -> list[MetricsResult]
ovt.metrics.score_frames(reference: np.ndarray | tuple[np.ndarray, ...], distorted: ..., *, metrics=("psnr",), bit_depth: int = 8, chroma: str = "420") -> dict[str, float]
#   in-memory single-frame scoring from numpy planes via ovt_metrics_push/finish
class MetricsResult:                       # .descriptor .frames (FrameScore list) .pooled -> dict{"mean":[...],"harmonic":[...],...}
    def to_pandas(self) -> "pandas.DataFrame": ...
    def to_vmaf_json(self, fps: float) -> str: ...
    def map(self, display_index: int) -> np.ndarray | None: ...

ovt.bdrate(anchor: Sequence[tuple[float, float]], test: Sequence[tuple[float, float]], *, interpolator: str = "pchip", range: str = "overlap") -> BdResult
#   BdResult is a NamedTuple(bd_rate_percent, bd_quality, overlap_low, overlap_high, overlap_fraction, interpolator: str, log_base_bitrate: bool, range_mode: str, anchor_name: str, test_name: str, diagnostics: list[Diagnostic])  — same names as BdRateResult (chapter 06 §6.5)

# --- plugins / environment ---
ovt.plugins.list() -> list[PluginInfo]; ovt.plugins.add_path(dir); ovt.plugins.set_default(codec, id); ovt.plugins.install_codec_pack(dest=None)
ovt.cache.dir() -> str; ovt.cache.set_dir(path); ovt.cache.clear(**filters); ovt.cache.report() -> dict
ovt.set_threads(n); ovt.set_log_level("warn"); ovt.set_logger(callable)
ovt.doctor() -> dict
```

**Zero-copy rule.** `CellGrid` vectors, `Picture` planes and analysis planes are exposed through the buffer protocol (`py::buffer_info`) on a holder object that keeps the owning `ovt_Buffer`/`ovt_Picture` alive as long as any numpy view references it (pybind11 `keep_alive` + `py::capsule` destructor calling `ovt_buffer_free` / `ovt_picture_release`; D4 b — the picture holds its own reference, so a view outlives any later `Decoder.__next__()` safely). Arrays are `writeable=False`. A copy is made only when the user calls `np.array(view)` or `.copy()`. Strided planes use `strides=(stride_bytes, sample_bytes)`.

**Thread rule.** The GIL is released (`py::gil_scoped_release`) around every C call that takes an `ovt_Job*`; progress callbacks re-acquire it. A `Stream` MUST NOT be used from two Python threads concurrently (same as the C rule); the Python layer enforces this with a non-blocking lock and raises `RuntimeError("Stream is busy in another thread")`.

**Security (§0.7 rule 8).** The package never calls `eval`/`exec`/`pickle.loads` on data from files; `normalize` dictionaries are validated against the JSON Schema in `docs/schema/normalization.json` before being passed to C.

### 12.4.3 Example scripts (shipped in `python/examples/`, run by the `python-test` CI job against the `fast` corpus subset of chapter 14 §14.2.4)

**`plot_qp_map.py` — QP heat map of one frame with matplotlib**

```python
import sys, ovt, numpy as np, matplotlib.pyplot as plt
with ovt.open(sys.argv[1]) as c:
    s = c.video
    fa = s.analyze(int(sys.argv[2]), planes=["recon"])
    qp = fa.grid.qp                      # (cells_h, cells_w) int16 view; -1 = n/a
    luma = fa.plane("recon", 0)          # (H, W) uint8/uint16 view
fig, ax = plt.subplots(1, 2, figsize=(14, 5))
ax[0].imshow(luma, cmap="gray", vmin=0, vmax=(1 << fa.frame.bit_depth_luma) - 1); ax[0].set_title(f"frame {fa.frame.id.display_index} luma")
im = ax[1].imshow(np.ma.masked_less(qp, 0), cmap="viridis", interpolation="nearest", extent=(0, luma.shape[1], luma.shape[0], 0))
ax[1].set_title(f"QP ({fa.plugin_id})"); fig.colorbar(im, ax=ax[1]); plt.savefig("qp_map.png", dpi=150)
```

**`export_yuview.py` — YUView statistics CSV for a frame range**

```python
import sys, ovt
with ovt.open(sys.argv[1]) as c:
    s = c.video
    first, last = map(int, sys.argv[2].split("-"))
    for i, fa in enumerate(s.analyze_range(first, last)):
        fa.to_yuview_csv(sys.argv[3], append=i > 0)      # header once, then rows grouped by POC
print("open the decoded YUV in YUView and add", sys.argv[3], "as statistics file")
```

**`batch_vmaf.py` — batch VMAF/PSNR with pandas**

```python
import glob, sys, ovt, pandas as pd
ref = sys.argv[1]; dists = sorted(glob.glob(sys.argv[2]))
results = ovt.metrics.run(ref, dists, metrics=["psnr", "vmaf"], models=["version=vmaf_v0.6.1", "version=vmaf_v0.6.1neg"],
                          normalize={"scale": "ref", "filter": "bicubic"}, progress=lambda p: print(f"\r{p.stage} {p.fraction:.0%}", end="", file=sys.stderr))
rows = [{"distorted": r.distorted, "metric": r.descriptor.id, "model": r.model, **{k: v[0] for k, v in r.pooled.items()}} for r in results]
df = pd.DataFrame(rows); print(df.pivot(index="distorted", columns=["metric", "model"], values="mean").round(3))
for r in results:
    if r.descriptor.id == "vmaf": open(f"{r.distorted}.{r.model}.json", "w").write(r.to_vmaf_json(fps=r.fps))
```

### 12.4.4 Requirements

The Python package's requirements are rows FR-API-007, FR-API-008, FR-API-010 and FR-API-101 … FR-API-106 of the chapter table §12.1.1 (D1: requirement ids live only in chapter 04).

---

## 12.5 Scripting from the GUI (optional, phase 4)

These items are **optional** (MAY) and MUST NOT block any phase exit criterion.

1. **"Copy as command" (SHOULD, phase 2).** Every GUI panel that shows a result produced by a C API call offers a context-menu item that copies the equivalent `ovt` command line (e.g. selecting frame 1234 in the picture panel with the QP overlay active → `ovt analyze clip.mp4 -t 0 --frame 1234 --fields qp --format json`). This is cheap (the GUI already holds all parameters) and is the main bridge to scripting.
2. **"Open in terminal with this frame" (MAY, phase 4).** macOS: opens Terminal.app/iTerm via `open -a` with a shell script that `cd`s to the file's directory and pre-types the command; Linux: `$TERMINAL` or `x-terminal-emulator`; ChromeOS/Android: copies to clipboard instead (no terminal). Setting `gui.terminal_command` in chapter 11's settings.
3. **Python console panel (MAY, phase 4).** An embedded CPython (linked `libpython3`, same ABI as the wheel) running `ovt` *in-process* against the already open `Container`, exposing `app.container`, `app.stream`, `app.frame` (current display index), `app.selection` (block id) and `app.show(ndarray)` (displays a numpy plane in a picture panel). Runs on a dedicated thread; the console only touches core handles through the jobs pool (§0.7 rule 2). Requires `OVT_BUILD_GUI_PYTHON=ON`; **off by default** in official builds until the sandboxing of user scripts (§0.7 rule 8 — scripts are user code, so this is about not corrupting GUI state, not security) is specified in chapter 11. Not built on Android.

---

## 12.6 Work items

Phase numbering follows §0.8. "CLI skeleton" items are prerequisites for every `ovt` subcommand; each subcommand WI depends on the corresponding core module WI from chapters 07/08/09 (named there as WI-1.1xx container, WI-1.2xx syntax/index, WI-2.2xx plugins, WI-3.3xx metrics; the exact numbers are bound by the roadmap chapter 16).

### WI-1.500 — CLI skeleton with CLI11, global options, exit codes, envelope writers
**Goal:** Produce the `ovt` executable with all global options of §12.1.2, exit-code mapping of §12.1.3 and the `json`/`fb`/`csv`/`text` writers of §12.1.4, with no subcommands yet except `--version` and `completion`.
**Depends on:** WI-0.000 (repo skeleton), WI-0.083 (FlatBuffers schemas compiled).
**Produces:** `cli/CMakeLists.txt` (target `ovt_cli`, output name `ovt`), `cli/src/main.cpp`, `cli/src/app.h/.cpp` (global option struct `ovt::cli::Globals`), `cli/src/output/{json_writer,fb_writer,csv_writer,text_writer}.h/.cpp`, `cli/src/exit_codes.h`, `cli/tools/gen_completions.cpp` (build-time generator; it writes `ovt.bash`, `ovt.zsh` and `ovt.fish` into the build tree's generated-code root `build/<preset>/generated/completions/` (chapter 13 §13.2.3 generated-code rule), not into the repository, and they are embedded into the binary), `cli/tests/test_globals.cpp`. No schema file: `CliEnvelope` lives in `core/schema/ovt_model.fbs` (chapter 06 §6.3).
**Interface:** §12.1.2, §12.1.3, §12.1.4, `CliEnvelope` table.
**Steps:**
1. Add CLI11 to `vcpkg.json`; create `ovt_cli` target linking `ovt_core`, `fmt`, `flatbuffers`; set GPL-3.0-or-later SPDX headers.
2. Implement `Globals` parsing with env-var fallbacks and precedence rule; `--format` default by `isatty(STDOUT_FILENO)`.
3. Implement `ExitCode from_status(ovt_status)`; install SIGINT/SIGTERM handler that cancels a global `ovt_Job` and sets a flag; second signal → `_exit(130)`.
4. Implement the `fb` stream writer (magic + size-prefixed buffers with identifiers), refusing to write to a TTY without `-o`.
5. Implement the generic JSON envelope writer using `ovt_fb_to_json` and a hand-written envelope; `--pretty`.
6. Implement CSV writer (RFC 4180 quoting) and text table helper (`ovt::cli::Table` with column alignment and SI formatting).
7. Implement `--version` (text and JSON) and `completion`: CLI11 has no completion generator, so `cli/tools/gen_completions.cpp` links the parsing library (`ovt_cli_lib`), walks the `CLI::App` tree (subcommands, options, `IsMember` choices, alias flags) and writes the three scripts; run as a CMake custom command, output embedded via `configure_file`/`xxd`-style string constants.
8. Wire the stderr logger (`ovt_set_logger`) honouring `--log-level` and `--color`; implement `--progress` TTY and JSON Lines modes via `ovt_job_set_progress_callback`.
**Acceptance tests:**
1. Given no args, When `ovt`, Then exit 2 and stderr ends with `See 'ovt --help'.`.
2. Given `ovt --version --format json`, Then stdout is valid JSON with keys `ovt, core, api, build, features`.
3. Given `ovt --format fb --version` on a TTY, Then exit 2 with message `refusing to write binary to a terminal; use -o`.
4. Given `OVT_LOG_LEVEL=trace ovt --log-level error --version 2>err.txt`, Then `err.txt` is empty (flag wins over env).
5. Given `ovt completion zsh`, Then output begins with `#compdef ovt`.
**Size:** M.
**Notes / pitfalls:** Alias flags (`--json`, `--text`, …, §12.2.0) are registered as CLI11 options bound to the same variable with `->excludes()` against their canonical form only where the two could carry different values. CLI11 treats `-` as a positional, not a flag — register it explicitly. Never `exit()` from inside a signal handler; only set an atomic flag and call `ovt_job_cancel` (which is async-signal-safe by construction: it only stores an atomic). Disable CLI11's automatic `--help-all` to keep `--help` under 50 ms.
### WI-1.501 — JSON Schema generation from FlatBuffers schemas
**Goal:** Generate `docs/spec/schemas/*.schema.json` (JSON Schema draft 2020-12) for every root table and the CLI envelope so that consumers can validate `ovt` JSON output (chapter 04 FR-CLI-002).
**Depends on:** WI-1.500.
**Produces:** `tools/fbs_to_jsonschema.py`, `docs/spec/schemas/ovt-cli-1.schema.json`, `docs/spec/schemas/<Table>.schema.json`, CMake custom target `ovt_json_schema` (and `ovt_json_schema_check`), test `cli/tests/test_schema_validate.py`.
**Interface:** §12.1.4 JSON mapping rules.
**Steps:** 1. Run `flatc --jsonschema` for each `.fbs`; 2. post-process: enums as string names, `ulong`/`long` as `oneOf[integer,string]`, `[ubyte]` as base64 `string` (except `CellGrid` vectors: `array` of `integer`), floats allowing the strings `nan`/`inf`/`-inf`, nested `ext` → `ext_decoded` with `oneOf` over codec extension tables (the §12.1.4 printer rules); 3. write the envelope schema referencing per-command result schemas; 4. add a Python test (`check-jsonschema` package) validating `ovt info/probe/syntax/frames/index` outputs for every mini-corpus file.
**Acceptance tests:** 1. Given every MP4 of the `fast` corpus subset (chapter 14 §14.2.4), When `ovt probe f -f json` is piped into `check-jsonschema` (pinned in `tools/requirements.txt`, which this WI extends) with the schema `docs/spec/schemas/ovt-cli-1.schema.json`, Then the output validates. 2. Given a change to `ovt_model.fbs` without regenerating, When the CI target `ovt_json_schema_check` builds, Then CI target `ovt_json_schema_check` fails with a diff.
**Size:** S.
**Notes / pitfalls:** `flatc --jsonschema` emits `integer` for 64-bit; override. Keep the generated files committed so the docs site needs no build step.
### WI-1.502 — `ovt info`
**Goal:** Implement §12.2.1.
**Depends on:** WI-1.500, WI-1.217 (`TrackInfo`), WI-1.121 (index), WI-1.223 (HDR metadata view).
**Produces:** `cli/src/cmd/info.cpp`, `cli/tests/test_info.cpp`, golden files `cli/tests/golden/info/*.json`.
**Interface:** §12.2.1 output JSON; `ovt_container_track_info`, `ovt_stream_color`, `ovt_stream_index_summary`.
**Steps:** 1. Register subcommand and flags; 2. open file/container; collect tracks via `ovt_container_track_info`; 3. read `TrackInfo.codec_string` (rules: chapter 08 §8.1.3; e.g. `avc1.64001F`, `hvc1.2.4.L153.B0`, `av01.0.08M.10`); 4. with `--full`, build index via cache and attach summary; 5. text renderer; 6. golden tests over the mini corpus.
**Acceptance tests:** 1. Given `syn_h265_main10_pq_hdr.mp4` (chapter 14 §14.3.3 recipe 10), When `ovt info -f json` runs, Then `result.tracks[0].color.transfer == 16`. 2. Given any 1-hour file, When `time ovt info f`, Then ≤ 200 ms. 3. Given `-f csv`, When `ovt info` runs with it, Then exit 2.
**Size:** S.
**Notes / pitfalls:** Do not touch `mdat`; `bitrate_bps` for fragmented files comes from `sidx`/`trun` sums, not from `mdat` size.
### WI-1.503 — `ovt probe`
**Goal:** Implement §12.2.2 including `--expand`, `--samples`, `--fragments`, `--hdr`, `--edits`, `--enc`, `--brands`, `--bytes`, `--validate`/`--validate-level`, `--list-diagnostics`, `--demux-check`, flattened CSV and text tree (phase-4 flags `--representation`, `--tr101290` are wired by WI-4.210/WI-4.207).
**Depends on:** WI-1.502, WI-1.204 (lazy expansion), WI-1.224 (validation driver), WI-1.235 (container probe hooks).
**Produces:** `cli/src/cmd/probe.cpp`, `cli/src/output/tree_flatten.cpp`, `cli/tests/test_probe.cpp`, golden `cli/tests/golden/probe/*.{json,txt,csv}`.
**Interface:** `ContainerTree`, `ovt_container_expand(_all)`, `ovt_container_sample_table`, `ovt_container_fragments`, `ovt_container_hdr_metadata`, `ovt_container_encryption`, `ovt_container_validate`, `ovt_container_list_diagnostics`, `ovt_container_bytes` (§12.3.7).
**Steps:** 1. Flags and `--expand` selector parser (`all|none|N|fourcc,...`); 2. expansion driver (depth-limited DFS calling `ovt_container_expand`); 3. derived-view flags; 4. text tree renderer with the exact line format of §12.2.2 (FullBox `vN f=0x…`, dates decoded for `creation_time`); 5. flattened CSV with `path`; 6. `--offset` subtree selection; 7. golden tests vs. MP4Box `-diso` oracle as described in chapter 14.
**Acceptance tests:** 1. Given corpus file `syn_mp4_frag` (chapter 14 §14.3.3), When `--fragments -f csv`, Then row count equals number of `moof` boxes. 2. Given `--expand 1`, When `ovt probe` runs with it, Then no box deeper than level 1 has `fields`. 3. Given corpus file `syn_h265_main10_pq_hdr_mismatch` (`mdcv` ≠ SPS VUI), When `--hdr`, Then `conflicts[]` non-empty. 4. Given any corpus file run twice, When `ovt probe -f json` runs on it both times, Then identical bytes. 5. Given `ovt probe --list-diagnostics -f csv`, When it runs, Then one row per `CNT-*` code of chapter 08 §8.5 and exit 0 without an input file.
**Size:** M.
**Notes / pitfalls:** `--expand all` on a 4-hour fragmented file produces hundreds of thousands of `moof`/`trun` boxes; stream the JSON writer (do not build a DOM). Sample tables with `--samples` default `all` must warn above 100k rows.
### WI-1.504 — `ovt syntax` with `--trace`
**Goal:** Implement §12.2.3 including the trace text format §12.2.3.1.
**Depends on:** WI-1.500, WI-1.106 (trace-line writer), WI-1.130 (`ovt_stream_unit_syntax`, `ovt_stream_unit_bytes`), WI-1.121.
**Produces:** `cli/src/cmd/syntax.cpp`, `core/src/syntax/trace_writer.cpp` (implements `ovt_syntax_trace`), `cli/tests/test_syntax.cpp`, `cli/tests/golden/syntax/*.trace`, `tools/normalize_ffmpeg_trace.sed`.
**Interface:** `ovt_stream_unit_syntax`, `ovt_stream_unit_bytes`, `ovt_syntax_trace`.
**Steps:** 1. Flags and filters (`--unit/--frame/--type/--sei/--parameter-sets/--diagnostics-only/--filter/--fields/--all-units`; the `--filter` expression evaluator over `SyntaxNode` trees lives in `cli/src/syntax_filter.cpp`); 2. list mode CSV/JSON; 3. tree mode; 4. `--hex` with hex dump (16 bytes/row, offset, ASCII column) and EPB markers (`*` after an EPB byte); 5. trace writer with the column spec; 6. golden tests: for every corpus stream, `ovt syntax --trace` vs `ffmpeg -i f -c copy -bsf:v trace_headers -f null -` after `tools/normalize_ffmpeg_trace.sed` (chapter 14 defines the comparison fields: name and value only).
**Acceptance tests:** 1. Given HEVC conformance stream, When `--type SPS -f json`, Then `result.units[0].header.children[].name` contains `sps_video_parameter_set_id`. 2. Given `--unit 3 --hex`, When `ovt syntax` runs with it, Then `rbsp_hex` has fewer bytes than `raw_hex` iff `epb_positions` non-empty. 3. Given `--trace`, When `ovt syntax` runs with it, Then every line starts with `[ovt]`.
**Size:** M.
**Notes / pitfalls:** Bit offsets in the trace are RBSP-relative (§0.1 rule 9); do not convert to file offsets. For slice headers the active SPS/PPS must be the one active *at that unit*, not the last one in the stream.
### WI-1.505 — `ovt index`
**Goal:** Implement §12.2.4 with cache integration and summary.
**Depends on:** WI-1.500, WI-1.121, WI-1.125 (index), WI-0.091 (cache).
**Produces:** `cli/src/cmd/index.cpp`, `cli/tests/test_index.cpp`, `cli/tests/perf/index_1h_1080p.sh`.
**Interface:** `ovt_stream_build_index`, `ovt_stream_index_summary`, `ovt_cache_*`.
**Steps:** 1. Flags; 2. cache control (`--rebuild`, global `--no-cache`); 3. summary derivation (`gops[]`, histograms); 4. `fb` output; 5. perf test script asserting ≤ 2 s on the reference machine (recorded, not gating in CI; chapter 14).
**Acceptance tests:** 1. Given a file indexed once, When run again, Then the second run reads the cache (log line `cache hit`) and is ≥ 5× faster. 2. Given `--summary -f json`, When `ovt index` runs with it, Then `result.frames` is absent and `result.summary.frames` equals `ovt frames f -f csv | wc -l` − 1.
**Size:** S.
**Notes / pitfalls:** `--with-units` JSON for a 1-hour stream is hundreds of MB: stream the writer; never build a DOM. `--validate` runs after the cache lookup so a cached index is validated too.
### WI-1.506 — `ovt frames`
**Goal:** Implement §12.2.5.
**Depends on:** WI-1.505.
**Produces:** `cli/src/cmd/frames.cpp`, `cli/tests/test_frames.cpp`, golden CSVs.
**Interface:** `FrameInfo`, column list §12.2.5.
**Steps:** 1. Column registry with names → extractor lambdas; 2. `--columns` ordering; 3. `--refs` joins; 4. `--stats` running columns (sliding window by pts, window = 1 s in `timebase`); 5. decode/display order handling incl. hidden frames; 6. golden tests.
**Acceptance tests:** 1. Given AV1 stream with hidden frames, When `--order decode -f csv`, Then rows with `shown=0` have `display_index=-1`. 2. Given `--columns type,size_bytes`, When `ovt frames` runs with it, Then CSV header is exactly `type,size_bytes`.
**Size:** S.
**Notes / pitfalls:** Hidden AV1 frames have `display_index = -1`; sorting by display index must keep them stable at the end of decode order (`--order decode` is the only order that lists them).
### WI-1.507 — `ovt dump`
**Goal:** Implement §12.2.6 for `--stage recon` (playback path), the Y4M/YUV/PNG writers with MD5, and `--diff A B` (CLI wiring over the structural diff of chapter 06 WI-0.085); non-recon stages are enabled by WI-2.521.
**Depends on:** WI-1.505, WI-2.171 (playback decoder — a phase-1 deliverable, §16.3 of the roadmap (`16-roadmap-and-work-items.md`)).
**Produces:** `cli/src/cmd/dump.cpp`, `cli/src/output/y4m_writer.cpp`, `cli/src/output/framemd5.cpp`, `cli/tests/test_dump.cpp`.
**Interface:** `ovt_decoder_*`, `ovt_Picture` (`ovt_picture_release` after each frame), `DumpManifest`, `ovt::model::structural_diff()` (chapter 06 WI-0.085).
**Steps:** 1. Flags; 2. software decoder forced unless `--hw`; 3. Y4M header (`C420p10`, `C422p12`, `C444`, `Cmono`, interlace `Ip`, aspect `A0:0`, `XCOLORRANGE=LIMITED|FULL`); 4. bit-depth conversion; 5. framemd5-compatible digest; 6. manifest; 7. test: `ovt dump` MD5 equals `ffmpeg -f framemd5` for every mini-corpus file; 8. `--diff`: load both records (zstd, JSON via `ovt_json_to_fb`), canonicalise, call `ovt::model::structural_diff()`, print the returned lines, exit 8 when the result is non-empty.
**Acceptance tests:** 1. Given `hevc_main10.mp4`, When `ovt dump --md5`, Then md5 list equals `ffmpeg -i f -f framemd5 -`. 2. Given `--bitdepth 8` on 10-bit, When `ovt dump` runs with it, Then Y4M header has `C420jpeg`/`C420` and file size halves.
**Size:** M.
**Notes / pitfalls:** ffmpeg framemd5 of >8-bit uses little-endian 16-bit samples and cropped planes; match exactly. Y4M has no chroma-siting field; write `XCSITING=` extension.
### WI-1.508 — `ovt extract`
**Goal:** Implement §12.2.7.
**Depends on:** WI-1.505.
**Produces:** `cli/src/cmd/extract.cpp`, `core/src/index/min_cvs.cpp` (`ovt_stream_min_cvs_units`), `core/src/index/extract.cpp` (`ovt_stream_extract`), tests.
**Interface:** `ovt_stream_extract`, `ovt_stream_min_cvs_units`, `ExtractManifest`.
**Steps:** 1. Selectors; 2. min-CVS algorithm: active VPS/SPS/PPS (or sequence header) at the RAP ≤ target, all units from that RAP to target in decode order, SEI optional; 3. Annex B writer (4-byte start codes, AUD preserved), OBU writer (Section 5 with size fields), IVF writer (32-byte header, 12-byte frame headers); 4. `--temporal-id` filter; 5. tests: extracted min-CVS decodes with FFmpeg and the last frame's md5 equals the full-stream frame md5.
**Acceptance tests:** 1. Given `--frames 1234`, When extracted and decoded by `ffmpeg`, Then the last output frame md5 == frame 1234 md5 from `ovt dump --md5`. 2. Given `--units 0-20 --to ivf`, When `ovt extract` runs with it, Then the IVF header `frame_count` equals the number of temporal units written.
**Size:** M.
**Notes / pitfalls:** For HEVC, CRA with leading RASL pictures: when the target lies in RASL frames after a CRA, use the previous RAP. AV1 `show_existing_frame` targets need the referenced frame's chain.
### WI-1.509 — `ovt cache`
**Goal:** Implement §12.2.13 including `ovt cache import`.
**Depends on:** WI-1.500, WI-0.091, WI-0.092 (cache), WI-0.088 (JSON importer).
**Produces:** `cli/src/cmd/cache.cpp`, tests.
**Interface:** `ovt_cache_*`, `CacheReport`.
**Steps:** 1. `list/stats/clear/path` subcommands; 2. confirmation prompt on TTY; 3. filters; 4. `import`: detect record kind by extension/magic, JSON → FlatBuffer via `ovt_json_to_fb`, validate with `ovt_validate_*`, write through `ovt_cache_entry`/`ovt_cache_put_*` (§12.3.14).
**Acceptance tests:** 1. Given an indexed file, When `ovt cache list -f csv`, Then one `index` row. 2. Given `ovt cache clear --yes`, When it runs, Then `stats` shows 0 entries. 3. Given `tiny16_hevc.fa` exported with `ovt analyze --validate -f json`, When `ovt cache import tiny16_hevc.json` runs and the frame is read back with `ovt_cache_get_frame`, Then the bytes equal the original `.fa` (round-trip byte-identical).
**Size:** S.
**Notes / pitfalls:** `import` must validate the JSON against the schema before writing into the cache (a bad import would poison every later read); `clear` requires `--yes` or a TTY confirmation. Report sizes in GiB consistently with the D5 caps (20 GiB desktop / 2 GiB Android).
### WI-1.510 — CLI golden-test harness and determinism check
**Goal:** A CTest harness that runs each CLI example of §12.2 over the `fast` corpus subset (chapter 14 §14.2.4), compares to golden files and runs each command twice for byte identity.
**Depends on:** WI-1.502–WI-1.509.
**Produces:** `cli/tests/golden_runner.py`, `cli/tests/cases.toml` (command, inputs, expected exit, golden path), `cli/tests/CMakeLists.txt`.
**Interface:** FR-CLI-004.
**Steps:** 1. Runner with `--update` mode; 2. determinism loop; 3. CTest registration per case; 4. document in `docs/dev/cli-testing.md`.
**Acceptance tests:** 1. Given all cases, When `ctest -R cli_golden`, Then pass. 2. Given a command whose output contains a timestamp, When the determinism test runs, Then the determinism test fails (verified with a deliberately broken fixture).
**Size:** S.
**Notes / pitfalls:** Golden JSON must be compared after `(u)long`-as-string and float shortest-round-trip normalisation (§12.1.4); differences in `elapsed_ms`/timestamps are masked by the harness, nothing else is.
### WI-1.511 — C API headers, visibility, version script and `api_example.c`
**Goal:** Finalise `core/include/ovt/*.h` as specified in §12.3 with export macros, SONAME, symbol map and the compiled C example.
**Depends on:** WI-1.130, WI-1.224, WI-1.234, WI-1.512 (the C++ modules whose C API this WI finalises).
**Produces:** all headers of §12.3.1, `core/ovt_core.map`, `core/ovt_core.exports`, `core/tests/api_example.c`, `core/tests/test_c_api_abi.cpp` (static asserts: `sizeof(ovt_Buffer)==16`, `offsetof(ovt_PlaneView, struct_size)==0` for every rule-7 struct, recorded sizes of `ovt_PlaneView`, `ovt_Picture`, `ovt_ColorDescription`), `cmake/OvtExport.cmake`.
**Interface:** §12.3.
**Steps:** 1. Write headers verbatim from §12.3 with `OVT_API` macro (`__attribute__((visibility("default")))`); 2. `-fvisibility=hidden` default for `ovt_core`; 3. version script listing all `ovt_*` symbols; 4. implement thread-local last-error; 5. compile `api_example.c` with `-std=c11 -Wall -Werror -pedantic` as a CTest; 6. ABI check test with `libabigail` (`abidiff --harmless` on a RelWithDebInfo build) against the previous tag in CI (chapter 13 §13.7.1); 7. every exported function body is a `noexcept` wrapper (rule 8) — a clang-tidy check (`ovt-c-api-noexcept`, chapter 14 §14.8) enforces it.
**Acceptance tests:** 1. Given the build, When `nm -D libovt_core.so | grep ' T '`, Then every symbol begins with `ovt_`. 2. Given `api_example.c`, When compiled with a C11 compiler, Then no warnings. 3. Given headers, When included from C++ with `-Wall -Wextra -Werror`, Then no warnings.
**Size:** M.
**Notes / pitfalls:** `ovt_last_error` is thread-local; the GUI calls from many threads. Do not expose `ovt_Picture.opaque` type.
### WI-1.512 — `ovt_Job` implementation and cancellation plumbing
**Goal:** Implement `ovt/jobs.h` on top of `ovt::jobs::Pool`, with the cancellation-check helper that the later index-building, decoding and container-expansion loops call every ≤ 50 ms (NFR-007).
**Overlap:** this WI also implements `ovt::jobs::Pool` and the `log` module (`core/src/log/`, `ovt/log.h`, `ovt/status.h` strings) because no other WI owns them; scheduled in phase 0 (milestone M0.2). See chapter 16 §16.3.
**Depends on:** WI-0.000 (this WI also implements `ovt::jobs::Pool` and the `log` module; no other WI owns them).
**Produces:** C++ helper headers (D22, §12.3.1 rule 1): `core/include/ovt/cxx/result.hpp` (`ovt::Result<T>`), `core/include/ovt/cxx/jobs.hpp` (`ovt::jobs::{Pool,Cancel,JobHandle}`; `CancelToken` alias; includes `cancel.hpp`), `core/include/ovt/cxx/cancel.hpp` (`struct ovt::Cancelled : std::runtime_error` thrown by `Cancel::throw_if_requested()`, chapter 07 §7.1.6; `jobs.hpp` includes this header), `core/include/ovt/cxx/log.hpp`; C headers `core/include/ovt/jobs.h` and `core/include/ovt/log.h` (text of §12.3.4/§12.3.5); `core/src/jobs/pool.cpp`, `core/src/api/jobs_api.cpp` (§12.3.1 rule 8 location), `core/src/log/{log.cpp,status_strings.cpp}`, `core/tests/jobs/mock_job.{h,cpp}` (`MockJob`: a job function that sleeps in 10 ms slices, polls `is_cancelled()` and reports progress), `core/tests/jobs/test_jobs_cancel.cpp`, `core/tests/jobs/test_jobs_pool.cpp`.
**Interface:** §12.3.4, §12.3.5; chapter 07 §7.1.6 (`jobs::Pool`, `Cancel`, `Cancelled`).
**Steps:** 1. Job struct (atomics, progress snapshot under a mutex, callback throttling 100 ms); 2. `Cancel::check()`/`throw_if_requested()` helper that later WIs (WI-1.121 index builder, WI-1.204 container expansion, decode loops) insert into their loops — this WI inserts nothing into other modules; 3. `MockJob` test fixture; 4. tests: cancel the mock job after 50 ms and assert the return time (NFR-007) and the progress snapshot.
**Acceptance tests:** 1. Given a `MockJob` (`core/tests/jobs/mock_job.cpp`) submitted through `ovt_Job` that sleeps in 10 ms slices and polls `is_cancelled()`, When `ovt_job_cancel` is called from another thread after 50 ms, Then the blocking call returns `OVT_E_CANCELLED` within 50 ms of the cancel (NFR-007), `ovt_job_progress` reports `fraction < 1.0`, and `ovt_job_reset` followed by a second submission succeeds. 2. Given a progress callback and a `MockJob` reporting progress every 1 ms, When it runs for 2 s, Then the callback is invoked at most 21 times (≤ 1 per 100 ms, §12.3.5). 3. Given `Cancel::throw_if_requested()` on a cancelled job inside a `Pool` task, When the task throws `ovt::Cancelled`, Then `Pool` maps it to `OVT_E_CANCELLED` at the job boundary and no exception crosses the C ABI (§12.3.1 rule 8).
**Size:** S.
**Notes / pitfalls:** Cancellation checks every ≤ 50 ms must not take locks held by the worker (deadlock on cancel); `ovt_job_cancel` is callable from any thread and from signal handlers only sets an atomic flag. `Pool` threads are joined at process exit, never detached. The acceptance tests use the synthetic `MockJob`, not the index builder (WI-1.121, M1.2), so this WI is complete inside M0.2 (D25).
### WI-2.520 — `ovt plugins` and plugin search-path implementation
**Goal:** Implement `ovt/plugins.h` (§12.3.11) and `ovt plugins` (§12.2.14).
**Depends on:** WI-1.511, WI-2.150 (plugin registry).
**Produces:** `core/src/analysis/plugin_registry.cpp`, `cli/src/cmd/plugins.cpp`, `core/tests/test_plugin_registry.cpp` (uses a test plugin `core/tests/plugins/libovt_plugin_fake.so` with wrong ABI version).
**Interface:** §12.3.11, `PluginList`.
**Steps:** 1. Search path list per platform; 2. `dlopen`/`dlsym` with `RTLD_LOCAL|RTLD_NOW`, SHA-256 of the file; 3. duplicate/ABI rejection diagnostics; 4. static registration; 5. defaults per codec; 6. CLI table and `--check`.
**Acceptance tests:** 1. Given the fake plugin in `--plugin-dir`, When `ovt plugins --check`, Then exit 6 and diagnostic `ANA-PLG-0001`. 2. Given `OVT_PLUGIN_PATH` set, When `ovt plugins -f json` runs, Then `search_paths[1]` equals it.
**Size:** M.
**Notes / pitfalls:** On macOS, `dlopen` of an unsigned dylib fails under hardened runtime without the `disable-library-validation` entitlement (chapter 13 §13.5.4). On Android, only `nativeLibraryDir` and app-private `filesDir` are allowed (W^X policy; chapter 13 §13.4.6).
### WI-2.521 — `ovt analyze` and `ovt dump` non-recon stages
**Goal:** Implement §12.2.8 for all formats and enable `dump --stage` ≠ `recon`.
**Depends on:** WI-2.520, WI-1.507, WI-2.151, WI-2.523 (analysis path), WI-0.089 (YUView CSV exporter), WI-0.090 (aom-inspect exporter).
**Produces:** `cli/src/cmd/analyze.cpp` (including the wiring of `--format yuview` with `--yuview-types` and `--yuview-poc-column` to the YUView CSV exporter of WI-0.089 and of `--format aom-inspect` to the exporter of WI-0.090), `cli/src/output/cell_csv.cpp`, `cli/src/output/planes_dir.cpp`, tests, golden `cli/tests/golden/analyze/*.{json,csv}`.
**Interface:** `ovt_analysis_*`, `FrameAnalysis`, `--planes`, `--csv-rows`, `--verify-with` (second `ovt_Analysis` with the oracle plugin; comparison rules chapter 07 §7.6.4).
**Steps:** 1. Flags incl. selectors and `--fields`; 2. `ovt_analysis_range` with streaming writers; 3. per-cell CSV; 4. YUView export via chapter 06 exporter; 5. `aom-inspect` writer; 6. planes dir with sidecar JSON; 7. `dump --stage pred/...` through `ovt_analysis_plane`; 8. golden tests vs `aom inspect` JSON on AV1 corpus (chapter 14 normalisation).
**Acceptance tests:** 1. Given AV1 stream, When `--frame 0 --format aom-inspect`, Then every `blockSize` entry equals `aomdec`'s `inspect` output. 2. Given `--format yuview`, When `ovt analyze` runs with it, Then YUView 2.14 opens it (manual, recorded in chapter 14) and `tools/yuview_csv_lint.py` passes. 3. Given `--fields qp`, When `ovt analyze` runs with it, Then `grid` has only `cells_w, cells_h, qp`.
**Size:** L.
**Notes / pitfalls:** Emitting `--all` for a long stream must not keep `FrameAnalysis` buffers (stream to disk as they arrive). `blob_id` strings must be filenames relative to `--planes-dir`, never absolute (determinism).
### WI-2.522 — "Copy as command" in the GUI
**Goal:** Implement §12.5 item 1 for the container, syntax, frames and analysis panels.
**Depends on:** WI-2.481, WI-2.482 (GUI panels), WI-2.521.
**Produces:** `app/src/CommandBuilder.h/.cpp`, QML context-menu items, `app/tests/test_command_builder.cpp`.
**Interface:** §12.2 grammar.
**Steps:** 1. A single builder mapping panel state → argv; 2. shell quoting (POSIX); 3. clipboard; 4. tests that the produced command parses with the CLI11 app tree (link `cli` parsing library as `ovt_cli_lib`).
**Acceptance tests:** 1. Given QP overlay on frame 10, When "Copy as command", Then clipboard equals `ovt analyze '<path>' -t 0 --frame 10 --fields qp --format json`.
**Size:** S.
**Notes / pitfalls:** Quote paths with spaces per shell (POSIX single quotes; PowerShell not targeted); omit options that equal their defaults so the copied command stays short; include `--format` only when the panel uses a non-default format.
### WI-2.523 — `ovt_analysis_range` callback API and LRU plane store exposure
**Goal:** Implement the range API and `ovt_analysis_plane(_copy)` with the 4-frame plane LRU of §0.7 rule 3.
**Depends on:** WI-2.151, WI-2.152 (analysis host).
**Produces:** `core/src/analysis/c_api.cpp`, tests.
**Interface:** §12.3.10.
**Steps:** 1. `ovt_analysis_range`: iterate decode indices through the analysis host, invoking the callback inside a `noexcept` wrapper (rule 8) and translating a non-zero return into `OVT_E_CANCELLED`; 2. `blob_id` → `PictureStore` lookup (chapter 06 §6.6) for `ovt_analysis_plane`, tightly packed copy for `_plane_copy`; 3. LRU of 4 frames with `ANA-LRU-0001` on a miss; 4. tests with the 60-frame synthetic GOP and RSS measurement.
**Acceptance tests:** 1. Given a 60-frame GOP, When `ovt_analysis_range(0,59)`, Then callback invoked 60 times in decode order and RSS stays < 1 GB at 4K. 2. Given a `blob_id` of a frame evicted from the LRU, When `ovt_analysis_plane` is called with it, Then `ovt_analysis_plane` returns `OVT_E_INVALID_ARG` with diagnostic `ANA-LRU-0001`.
**Size:** S.
**Notes / pitfalls:** The LRU holds decoded planes, not FlatBuffers: at 4K 4:2:0 10-bit a frame is ~25 MB per stage, so 4 frames × 3 stages must stay under the RSS budget (D5); evict by frame, never by single plane, so the `blob_id`s of one frame are all valid or all `ANA-LRU-0001`.
### WI-3.530 — `ovt metrics` and `ovt compare`
**Goal:** Implement §12.2.9 including the libvmaf-compatible `--vmaf-json <path>` / `-f vmaf-json` export, `--maps`, `--fail-below`, path templates, and `ovt compare` of §12.2.19 (first-mismatch finder, identity report, difference image; chapter 04 FR-CLI-016, FR-PLN-009).
**Depends on:** WI-1.507, WI-3.301, WI-3.302, WI-3.309, WI-3.311, WI-3.312 (core exporters), WI-3.316, WI-1.500.
**Produces:** `cli/src/cmd/metrics.cpp`, `cli/src/cmd/compare.cpp` (§12.2.19), `cli/src/output/vmaf_json.cpp` (uses `ovt_metrics_export_vmaf_json`), `cli/src/output/pgm_writer.cpp`, `tests/oracles/normalize/vmaf_json_compare.py` (`vmaf_json_compare.py A.json B.json --tol <t>`: compares every common key of `pooled_metrics.*` and per-frame `frames[].metrics.*`, ignoring `fps` and `version`; exit 0 equal within tolerance, 1 otherwise, printing the first differing key; also used by chapter 14 WI-3.591 and the chapter 16 §16.1.4 #2 exit demo), tests `cli/tests/test_metrics_cli.py`, `cli/tests/test_compare_cli.py`, golden JSON vs `vmaf` CLI.
**Interface:** `ovt_metrics_*`, `MetricsResult`.
**Steps:** 1. Flags → `MetricsConfig` JSON; 2. multi-distorted loop with exit 9 partial semantics; 3. alignment report; 4. per-frame CSV and pooled rows; 5. vmaf JSON export; 6. PGM maps; 7. thresholds; 8. golden tests on Netflix vectors (pooled and per-frame to 1e-4); 9. `ovt compare` (`--first-diff`, `--identity`, `--diff-image`, `--tolerance`) on the same normalisation front-end with normalisation off.
**Acceptance tests:** 1. Given the Netflix pair `src01_hrc00_576x324.yuv` / `src01_hrc01_576x324.yuv` (chapter 14 `netflix` source), When `ovt metrics -r src01_hrc00_576x324.yuv -d src01_hrc01_576x324.yuv --raw-format 576x324:yuv420p -m vmaf --align none -f vmaf-json -o o.json` runs (§12.2.9; `--json <path>` does not exist, `--json` is the `--format json` alias), Then `o.json` `pooled_metrics.vmaf.mean` equals `vmaf --json` output to 1e-4 (compared with `tests/oracles/normalize/vmaf_json_compare.py o.json <vmaf CLI json> --tol 1e-4`, produced by this WI). 2. Given `--fail-below vmaf=99`, When `ovt metrics` runs with it, Then exit 8. 3. Given one missing distorted among two, When `ovt metrics` runs with it, Then exit 9 and `result[1].status == "IO"`. 4. Given `syn_metrics_1080p_pair` reference and a copy of it with one sample of frame 37 flipped, When `ovt compare --first-diff A B -f json` runs, Then exit 8 and `result` is `{frame: 37, …}` with the flipped coordinates; and `ovt compare --identity A A` exits 0 with `identical_frames == total_frames`.
**Size:** L.
**Notes / pitfalls:** The `vmaf` CLI's `--json` prints per-frame `frameNum` starting at 0 and uses `"integer_motion"` feature names; copy the key set from chapter 09, not from memory.
### WI-3.531 — `ovt bdrate` and `ovt_bdrate`
**Goal:** Implement §12.2.10 and the C function.
**Overlap:** this WI delivers only `cli/src/cmd/bdrate.cpp`, the `ovt_bdrate` C function and its tests; the maths is `core/src/metrics/bd/` of WI-3.313 (no `core/src/metrics/bdrate.cpp`). See chapter 16 §16.3.
**Depends on:** WI-3.530, WI-3.313 (BD-rate maths).
**Produces:** `cli/src/cmd/bdrate.cpp`, `core/src/api/bdrate_api.cpp` (the `ovt_bdrate` C wrapper over `core/src/metrics/bd/`), `core/tests/test_bdrate.cpp` with reference values from libaom `tools/` and JVET Excel template vectors (chapter 14).
**Interface:** §12.3.13 `ovt_bdrate`, `ovt_metrics_bd`, `BdRateResult`.
**Steps:** 1. Flags of §12.2.10 incl. `--interp` alias, `--hull`/`--targets`, `--collect`; 2. RD-point readers (`ovt metrics` JSON/CSV, inline `--rd`); 3. call `ovt_metrics_bd` (JSON form) and, for the two-curve C form, `ovt_bdrate` filling `ovt_BdResult`; 4. CSV/text renderers; 5. tests against libaom and JVET vectors.
**Acceptance tests:** 1. Given the libaom example curves, When `--interpolator pchip`, Then `bd_rate_percent` matches libaom's script to 0.01 %. 2. Given a non-monotonic curve, When `ovt bdrate` runs, Then `diagnostics[]` contains `VQ-BD-0003` (chapter 09 §9.11.4 registry) and exit 0.
**Size:** M.
**Notes / pitfalls:** `ovt_BdResult.diagnostics` is caller-owned (rule 3a); free it even when the status is not `OVT_OK`.
### WI-3.532 — `ovt hdr`
**Goal:** Implement §12.2.11 using `ovt_hdr_measure` and container/bitstream metadata.
**Overlap:** this WI also owns sequence measurement (`--frames <sel>`, per-frame and sequence MaxCLL/MaxFALL JSON) formerly the `hdr-scan` subcommand proposal of WI-4.457 (not a subcommand; §12.2.0). See chapter 16 §16.3.
**Depends on:** WI-1.507, WI-1.402 (H.273 colour mapping), WI-1.223 (container HDR views); the CPU measurement `core/src/color/hdr_measure.cpp` is produced by this WI.
**Produces:** `core/src/color/hdr_measure.cpp`, `cli/src/cmd/hdr.cpp`, tests with synthetic PQ ramps (known nits).
**Interface:** §12.3.12, `HdrReport`.
**Steps:** 1. Flags; 2. declared-metadata collection via `ovt_container_hdr_metadata` and `ovt_stream_color`; 3. `--measure` loop: software decode, `ovt_hdr_measure` per frame, CTA-861.3 sequence MaxCLL/MaxFALL; 4. `--dynamic` records from SEI/OBU metadata units; 5. `--compare-declared`; 6. tests with synthetic PQ ramps.
**Acceptance tests:** 1. Given a synthetic PQ frame with code value 769 (≈ 1000 nits) everywhere, When `--measure`, Then `max_nits` within 1 % of 1000. 2. Given `--compare-declared` with declared MaxCLL 1000 and measured 1200, When `ovt hdr` runs, Then diagnostic `VQ-HDR-0002` Warning (§12.2.11; chapter 09 §9.11.4 registry).
**Size:** M.
**Notes / pitfalls:** `--measure` must decode in software with film grain applied (AV1) and in the coded bit depth; MaxFALL is the per-frame average over the active picture area (crop applied), MaxCLL the maximum (CTA-861.3) unless `--percentile` is set.
### WI-3.533 — `ovt_metrics_push` in-memory path
**Goal:** Allow scoring of frames supplied as `ovt_Picture` without a stream (Python `score_frames`, unit tests).
**Depends on:** WI-3.530.
**Produces:** `core/src/metrics/push_source.cpp`, tests.
**Interface:** §12.3.13 `ovt_metrics_push`, `ovt_metrics_finish`; `spec_json` with `"reference":"push"`.
**Steps:** 1. A `FrameSource` implementation fed by pushed `ovt_Picture`s (reference counted; the job holds a reference until scored); 2. normalisation path identical to stream sources; 3. `finish()` = drain + pooling + report.
**Acceptance tests:** 1. Given identical ref/dist planes, When pushed and finished with `psnr`, Then value is `inf` encoded as `1e9` (chapter 09 rule) and `ssim == 1.0`.
**Size:** S.
**Notes / pitfalls:** Pushed planes are borrowed for the duration of the call only (D4): the host copies or scores synchronously; a caller that frees the buffer after `ovt_metrics_push` returns is correct. Mixed push/stream use on one job is `OVT_E_INVALID_ARG`.
### WI-4.540 — Python package skeleton, pybind11 bindings, wheels
**Goal:** Build `ovt._core` over the C API with the object model of §12.4.2 for `open/Container/Stream/index/frames/unit/picture`.
**Depends on:** WI-1.511, WI-1.512.
**Produces:** `python/pyproject.toml`, `python/CMakeLists.txt` (target `ovt_py`), `python/src/_core.cpp`, `python/ovt/{__init__,_errors,_fb,_container,_stream,_picture}.py`, `python/ovt/_core.pyi`, `python/ovt/py.typed`, `python/tests/test_open.py`, `.github/workflows/wheels.yml` (chapter 13).
**Interface:** §12.4.2.
**Steps:** 1. scikit-build-core config, one extension per interpreter (`CIBW_BUILD="cp311-* cp312-* cp313-*"`); 2. holder classes with buffer protocol; 3. exception mapping; 4. generated FlatBuffers Python comes from WI-0.083/WI-0.095 under `python/ovt/fb/` (chapter 06; no `ovt/_fb/` copy, chapter 16 §16.3.13); 5. dataclass views; 6. GIL release; 7. cibuildwheel matrix; 8. `auditwheel`/`delocate` bundling of `libovt_core` + deps; 9. stubs + mypy.
**Acceptance tests:** 1. Given the FR-API-001 acceptance line (chapter 04), When it is run against the wheel, Then it passes. 2. Given `s.frames[0]`, When read, Then `.type` is a `str` from `FrameType`. 3. Given `Picture.y`, When its flags and base are inspected, Then `y.flags.writeable is False` and `y.base` keeps the picture alive (`del pic; next(decoder); y.sum()` works — the capsule's `ovt_picture_release` runs only when the last view dies).
**Size:** L.
**Notes / pitfalls:** pybind11 does not support `Py_LIMITED_API`, so there is no abi3 wheel: build one extension per supported CPython (3.11–3.13) and add a new `cp3xx-*` entry when a new CPython is released. `auditwheel` rejects a wheel that links the system `libstdc++` newer than `GLIBCXX_3.4.25` — link it statically (§12.4.1). Record both in `python/README.md`. **Overlap:** step 4 reuses `python/ovt/fb/` produced by WI-0.083/WI-0.095; do not vendor a second generated copy under `ovt/_fb/`. See chapter 16 §16.3.
### WI-4.541 — Python analysis bindings (`FrameAnalysis`, zero-copy grids, planes, YUView export)
**Goal:** §12.4.2 `Stream.analyze/analyze_range`, `FrameAnalysis`, `plane()`, `to_yuview_csv`, `block_at`.
**Depends on:** WI-4.540, WI-2.523.
**Produces:** `python/ovt/_analysis.py`, `python/src/_analysis.cpp`, `python/tests/test_analysis.py`, `python/examples/plot_qp_map.py`, `python/examples/export_yuview.py`.
**Interface:** §12.4.2 `FrameAnalysis`, `Stream.analyze/analyze_range`; C API §12.3.10.
**Steps:** 1. `ovt_analysis_frame`/`_range` wrappers releasing the GIL; 2. `CellGrid` vectors as read-only numpy views over the FlatBuffer (holder keeps the `ovt_Buffer`); 3. `plane()` over `ovt_analysis_plane` with `keep_alive`; 4. `to_yuview_csv` via the chapter 06 exporter; 5. `block_at` via the model helper library (chapter 06 WI-0.084); 6. examples and tests.
**Acceptance tests:** 1. Given the FR-API-002 acceptance line (chapter 04), When it is run against the wheel, Then it passes. 2. Given a `FrameAnalysis` `fa`, When `fa.grid.qp.shape` is read, Then it equals `(cells_h, cells_w)`. 3. Given `export_yuview.py` on the `fast` AV1 corpus, When its output is checked, Then `tools/yuview_csv_lint.py` (chapter 14 WI-0.567) passes.
**Size:** M.
**Notes / pitfalls:** numpy views over a FlatBuffer vector must keep the owning `ovt_Buffer` alive (capsule as base object); `writeable=False`. Release the GIL only around the C calls, never while touching Python objects.
### WI-4.542 — Python metrics and BD-rate bindings
**Goal:** `ovt.metrics.run`, `score_frames`, `MetricsResult.to_pandas/to_vmaf_json/map`, `ovt.bdrate`.
**Depends on:** WI-4.540, WI-3.530–WI-3.533.
**Produces:** `python/ovt/metrics.py`, `python/src/_metrics.cpp`, `python/tests/test_metrics.py`, `python/examples/batch_vmaf.py`.
**Interface:** §12.4.2 `ovt.metrics.*`, `ovt.bdrate`; C API §12.3.13.
**Steps:** 1. Build `spec_json` from keyword arguments (validated against `core/schema/normalization_config.schema.json`, chapter 09 §9.2.4); 2. `ovt_metrics_job_*` wrappers with `JobEvent` → `Progress` callbacks; 3. `MetricsResult` views over the `MetricsReport` buffer, `to_pandas`, `to_vmaf_json` (via `ovt_metrics_export_vmaf_json`), `map()` via `ovt_metrics_map`; 4. `score_frames` over `ovt_metrics_push/finish`; 5. `ovt.bdrate` over `ovt_bdrate`; 6. tests against the chapter 14 Netflix vectors.
**Acceptance tests:** 1. Given the FR-API-004 acceptance line (chapter 04), When it is run against the wheel, Then it passes. 2. Given identical planes `a`, When `score_frames(a, a, metrics=["ssim"])` is called, Then it returns `{"ssim": 1.0}`.
**Size:** M.
**Notes / pitfalls:** Progress callbacks arrive on a worker thread: marshal to Python with the GIL re-acquired and swallow exceptions into a cancellation (never propagate across the ABI, rule 8). BD-rate helpers must accept numpy arrays and plain lists.
### WI-4.543 — `ovt verify` (and the phase-4 `ovt hrd` wrapper)
**Goal:** Implement §12.2.12 (hash SEI, `--levels` and against-oracle modes) and the `ovt hrd` subcommand of §12.2.21 as a thin wrapper over `ovt_index_hrd_trace`.
**Depends on:** WI-2.521, WI-4.105, WI-4.106 (HM oracle plugin and verify comparator), WI-4.112 (HRD trace for `ovt hrd`, §12.2.21).
**Produces:** `core/src/decode/picture_hash.cpp` (MD5/CRC/checksum per H.265 D.3.19), `core/src/index/level_limits.cpp` (`--levels` tables for H.264 A.3, H.265 A.4, AV1 Annex A), `cli/src/cmd/verify.cpp`, `cli/src/cmd/hrd.cpp`, tests on HEVC conformance streams (all carry hash SEI).
**Interface:** `VerifyReport`, `HrdTrace`; C API `ovt_decoder_*`, `ovt_analysis_plane`, `ovt_index_hrd_trace` (§12.3.8).
**Steps:** 1. Flags incl. aliases (`--hash`, `--picture-hash`, `--plugin-compare`); 2. hash-SEI mode: decode with the software decoder, compute the three hash kinds over cropped planes, compare per plane; 3. `--levels`: read profile/tier/level and the limit tables, check picture size, sample rate, DPB size, tiles/slices, HRD bit rate; 4. against-oracle mode via two `ovt_Analysis` handles (reconstruction planes) with `--tolerance`/`--stop-on-first`/`--report-planes`; 5. `ovt hrd`: map flags onto `ovt_HrdOptions`, print `HrdTrace` (json/csv/text); 6. tests.
**Acceptance tests:** 1. Given every HEVC conformance stream in the corpus, When `ovt verify --hash-sei`, Then exit 0. 2. Given a stream with one flipped sample in `mdat`, When `ovt verify` runs on it, Then exit 8 and `first_mismatch_xy` set. 3. Given corpus file `syn_h264_hrd_cbr` (chapter 07 WI-4.112), When `ovt hrd --type nal -f json`, Then `overflow_count == 0` and `underflow_count == 0`. 4. Given a stream whose SPS signals level 4.0 but whose picture size exceeds `MaxLumaPs` of that level, When `--levels`, Then `checks.levels == "fail"` and exit 8.
**Size:** M.
**Notes / pitfalls:** H.265 D.3.19 checksum: per sample `xorMask = (x & 0xFF) ^ (y & 0xFF) ^ (x >> 8) ^ (y >> 8)`, sum of `(sample & 0xFF) ^ xorMask` and, for > 8 bit, `(sample >> 8) ^ xorMask`, modulo 2^32; CRC is the CRC-16 polynomial 0x1021 over the samples in raster order with the bit order of D.3.19 (MSB first, 8-bit samples one byte, > 8-bit two bytes MSB first); MD5 for > 8 bit uses little-endian 2-byte samples. Copy the three procedures from the HM `SEIDigest`/`TComPicYuvMD5.cpp` behaviour and test against HM's own hash SEIs, not from memory.
### WI-4.544 — `ovt doctor`
**Goal:** Implement §12.2.16.
**Depends on:** WI-2.520, WI-1.407 (display capability probe), WI-1.400 (headless `pl_gpu` enumeration), WI-2.171 (HW decoder probe).
**Produces:** `cli/src/cmd/doctor.cpp`, `core/src/decode/hw_probe.cpp`, `render/src/render_probe.cpp` (exposed as `ovt_render_probe_json` in `render/include/ovt/render/probe.h`; CLI links `ovt_render` only for this command and only if `OVT_CLI_WITH_RENDER_PROBE=ON`, default ON on desktop, OFF on Android), tests.
**Interface:** `DoctorReport`; `ovt_plugins_list`, `ovt_build_info_json`, `ovt_render_probe_json`, `ovt_cache_report`.
**Steps:** 1. Collect build info, env vars, plugin search paths and `PluginList`; 2. HW decoder probe per codec at 1080p/4K through `ovt_decoder_open(..., 2 /*hw required*/)` on embedded 2-frame test streams; 3. GPU/display probe through `ovt_render_probe_json` unless `--no-gpu`; 4. CPU features via Highway; 5. `--decode-test`; 6. text renderer and tests.
**Acceptance tests:** 1. Given `--no-gpu` in CI, When `ovt doctor --no-gpu -f json` runs, Then exit 0 and `gpu_json == "{}"`. 2. Given a machine without HEVC plugin, When `ovt doctor` runs, Then a Warning diagnostic `DEC-PLUGIN-0001` (chapter 07 §7.9) naming `h265`.
**Size:** M.
**Notes / pitfalls:** `doctor` must never fail hard: every probe is wrapped so a crashing probe (e.g. Vulkan loader) yields a `fail` row, exit 0 unless `--strict`. Do not load plugins from user paths before the signature check (chapter 13 §13.4.6 rules apply on Android).
### WI-4.545 — Python docs, examples CI and typing gate
**Goal:** Sphinx docs, the `python-test` CI job (pytest + every example on the `fast` corpus), `python-typecheck` (`mypy --strict`) and `python-docs` (`sphinx-build -W`) — job names as in chapter 13 §13.7.1.
**Depends on:** WI-4.541, WI-4.542.
**Produces:** `python/docs/conf.py`, `python/docs/*.rst`, the jobs `python-test`, `python-typecheck`, `python-docs` in `.github/workflows/ci.yml` (chapter 13 §13.7.1 rows; no separate `python.yml`).
**Interface:** none (docs/CI only); consumes the public package of §12.4.2.
**Steps:** 1. Sphinx project with `autodoc`/`napoleon` over `ovt` and the `.pyi` stubs; 2. `python-test` job running `pytest` and every `python/examples/*.py` against the `fast` corpus subset; 3. `mypy --strict python/ python/examples/`; 4. publish `docs/python/` from the release job.
**Acceptance tests:** 1. Given a PR touching `python/`, When the `python-test` and `python-typecheck` jobs run, Then `pytest`, every `python/examples/*.py` (on the `fast` corpus) and `mypy --strict python/ovt` exit 0. 2. Given the docs tree, When the `python-docs` job runs `sphinx-build -W`, Then it exits 0 (warnings are errors).
**Size:** S.
**Notes / pitfalls:** Run the examples against the `fast` corpus subset only (CI time); `mypy --strict` on the stubs must pass with the pinned numpy version — numpy typing changes between minor releases, so pin it in `python/requirements-dev.txt`.
### WI-4.546 — Optional: Python console panel and "Open in terminal" (MAY)
**Goal:** §12.5 items 2–3 behind `OVT_BUILD_GUI_PYTHON`.
**Depends on:** WI-4.540, WI-1.451 (dock manager).
**Produces:** `app/src/python/EmbeddedPython.h/.cpp`, `app/qml/panels/PythonConsole.qml`, `app/src/OpenInTerminal.cpp`.
**Interface:** §12.5 items 2–3 (`app.container`, `app.stream`, `app.frame`, `app.selection`, `app.show`); chapter 11 setting `gui.terminal_command`.
**Steps:** 1. CMake option `OVT_BUILD_GUI_PYTHON` (chapter 13 §13.2.2) linking `libpython3` only when ON; 2. interpreter on a dedicated thread, `ovt` imported in-process against the open handles via the jobs pool; 3. console panel (input line, history, output view) as a dock; 4. "Open in terminal" per platform; 5. tests behind the option.
**Acceptance tests:** 1. Given the option ON on macOS, When `app.frame` is typed, Then the current display index prints. 2. Given OFF (default), When the app bundle is built, Then no `libpython` dependency in the bundle (`otool -L`).
**Size:** L.
**Notes / pitfalls:** the embedded interpreter must never run on the GUI thread (chapter 11 §11.7: ≤ 50 ms rule); `Py_Initialize` once per process and keep the GIL released while the console is idle; the console shares the open handles, so a script that closes a file must go through the jobs model, not the raw C API.

---

### Glossary additions (chapter 12)

Every term this chapter uses is defined in chapter 2 (Glossary). The entries below add chapter-specific detail that chapter 2 does not carry; entries that merely repeated chapter 2 were removed.

- **min-CVS** — the minimal set of units (parameter sets + decode chain from the preceding RAP) required to decode a given picture; `ovt extract --frames` emits it.
- **selector grammar** — the single range syntax of §12.1.5 (`N`, `A-B`, `A-`, `A-B/S`, `A,B,C`, `rap`, `every:N`, `all`) used by every range-valued CLI option.
**Notes / pitfalls:** The embedded interpreter shares the process with Qt: never call into the GUI from the console thread without `QMetaObject::invokeMethod`; a blocking console call must not stall the render loop (run it on the pool, `ovt_Job` cancellable from the GUI).
