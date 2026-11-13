# 4. Requirements

> **Status:** normative. This chapter is the contract between the owner and the implementation team. Every requirement is one testable sentence using RFC 2119 words (MUST / MUST NOT / SHOULD / MAY), carries a **Phase** tag from chapter 0 §0.8 (0, 1, 2, 3, 4 or A) and at least one **Acceptance** line in Given / When / Then form that names a concrete command, UI action or test. Work items in chapter 16 reference these IDs; a work item is complete only when every acceptance line of the requirements it claims passes.
>
> **How to read.** Requirements are grouped by area (chapter 0 §0.1 item 1). Each area opens with a short rationale that cites chapter 3 (competitive reference) and the backbone. Names in `code font` are the exact identifiers from chapter 0 §0.5 and §0.6 (`StreamIndex`, `FrameAnalysis`, `CellGrid`, `SyntaxNode`, `ContainerTree`, `VideoSurface`, `ovt` CLI, `ovt-studio`). Codec facts cite specification sections: H.264 = ITU-T H.264 (08/2021); H.265 = ITU-T H.265 (09/2023); AV1 = AOM AV1 Bitstream & Decoding Process Specification v1.0.0 with Errata 1; ISOBMFF = ISO/IEC 14496-12:2022.
>
> **Precedence.** Where this chapter and a design chapter disagree, this chapter wins on *what must be true*; chapter 12 wins on CLI grammar and exit codes (§12.1.3, §12.2.0); chapter 11 wins on key bindings (§11.4.3) and the settings file (§11.2.5); chapter 6 wins on the FlatBuffers schema; chapter 16 wins on scheduling. Requirement ids (`FR-*`, `NFR-*`) are defined only here (D1); ids `FR-<AREA>-1nn` were added in the wave-2 revision from placeholders raised by chapters 08, 10, 11, 12, 13 and 15.
>
> **Conventions for acceptance lines.** `corpus/<id>` refers to a fixture by its corpus id `<source>_<name>` (chapter 14 §14.2.3): a manifest entry of `tests/corpora/manifest.json` (`jvt_*`, `jctvc_*`, `aom_*`, `argon_*`, `mpegff_*`, `netflix_*`, `ytugc_*`, `hdr_*`, `dashif_*`, `ts_*`, …) or a synthetic recipe of chapter 14 §14.3.3 (`syn_*`); every id named in this chapter MUST appear in chapter 14 §14.3 (D26). Brace patterns (`corpus/syn_{h264,h265,av1}_gop60_1080p`) expand per codec; a trailing `*` means every manifest entry with that prefix. `ovt …` is the CLI (chapter 12). `test:<Name>` is a Catch2 test name in `core/tests/`. "M1 reference" is the performance reference machine `m1-air-8gb` (MacBook Air M1, 8 GB, macOS 15) of chapter 14 §14.9.2 (D29; chapter 0 §0.7.4 mirrors it); the Android devices are `pixel-tablet` (SDR), `android-hdr` and `chromebook-hdr` of the same table. Performance numbers quoted in acceptance lines are the budgets of chapter 14 §14.9.3 / `tests/perf/budgets.json` (D19), repeated here for readability only. Where a requirement applies to all three codecs, the acceptance line names one stream per codec with the pattern `{h264,h265,av1}`.

---

## 4.1 GEN — General

**Rationale.** These requirements fix what the product is (chapter 0 §0.0), how it is built (ADR-001, ADR-002, ADR-012) and the behaviours that cut across every module: codec neutrality of the data model, the single-engine rule (chapter 3 UX-11), and the file formats accepted at the front door. They exist so that no later area can accidentally narrow the scope (for example by hard-coding three codecs).

**FR-GEN-001** (Phase 0) — The product MUST consist of exactly the layered components named in ADR-003: `libovt-core` (C++20, C API in `core/include/ovt/ovt.h`), `libovt-render`, the `ovt` CLI, the `ovt-studio` Qt Quick GUI, the Android shell and the Python module, with no analysis logic implemented outside `libovt-core`.
- Acceptance: Given the repository, when `tools/check_layering.py` runs in CI, then it reports zero source files under `app/`, `cli/`, `android/` or `python/` that include headers from `core/src/` (only `core/include/` is allowed).

**FR-GEN-002** (Phase 0) — All native code MUST compile as C++20 with `-Wall -Wextra -Werror` on Clang ≥ 16 (macOS Xcode ≥ 16, Android NDK r27c — chapter 13 §13.1) and GCC ≥ 13 (Linux).
- Acceptance: Given a clean checkout, when `cmake --preset macos-arm64-release && cmake --build --preset macos-arm64-release` runs, then the build exits 0 with zero warnings.

**FR-GEN-003** (Phase 0) — The data model MUST represent the codec of a stream by the `ovt.fb.Codec` enum and MUST NOT contain any `switch` or table that fails to compile or returns an error when a value outside {H264, H265, AV1} is added.
- Acceptance: Given `test:ModelCodecNeutral` constructs a `StreamIndex` with `codec = VP9`, when it is serialised and re-read, then no exception is thrown and `ovt_status_string` is never `OVT_E_INTERNAL`.

**FR-GEN-004** (Phase 1) — OVT MUST open the following inputs by content sniffing, not by file extension: ISOBMFF (`ftyp`/`moov`/`moof`/`styp` at offset 0 or after a `wide`/`free` box), IVF (`DKIF` signature), H.264/HEVC Annex B byte streams (start code `0x000001`), AV1 low-overhead OBU streams (OBU header with `obu_has_size_field = 1`, AV1 §5.3.1) and AV1 Annex B length-delimited streams, Y4M (`YUV4MPEG2` magic) and headerless raw YUV (requires a format description).
- Acceptance: Given each file of the `fast` corpus subset renamed to `.bin`, when `ovt probe <file>` runs, then the `format` field of the JSON output equals the expected value in `tests/golden/container/sniff.expected.json` for all files.

**FR-GEN-005** (Phase 1) — When a file contains more than one video track, OVT MUST list every track and MUST default to the first video track whose codec is in {H264, H265, AV1}.
- Acceptance: Given `corpus/mpegff_hevc_dual_track` (track 1 = AAC, track 2 = HEVC, track 3 = AV1), when `ovt probe` runs, then `tracks[]` has three entries and `default_video_track` equals 2.

**FR-GEN-006** (Phase 1) — Every user-visible error MUST carry a stable diagnostic code of the form `<FAMILY>(-<QUALIFIER>)?-<nnnn>` with `FAMILY` ∈ {`CNT`, `SYN`, `DEC`, `VQ`, `MDL`, `IO`, `BR`, `VER`, `ANA`, `RND`, `GUI`, `CLI`} (for example `SYN-H265-0007`, `CNT-0040` — a bare `CNT-nnnn` is an ISOBMFF code; chapter 5 §5.7 defines the format, each family's registry lives in its owning chapter) collected into the generated union `docs/spec/diagnostic-codes.md`, and the same code MUST appear in CLI JSON and in the GUI Diagnostics panel.
- Acceptance: Given `corpus/syn_broken_h265_truncated_sps`, when `ovt syntax <file> --format json` runs and when the file is opened in `ovt-studio`, then both show diagnostic code `SYN-H265-0003` with the same `message`.

**FR-GEN-007** (Phase 1) — OVT MUST treat all input files as untrusted and MUST NOT execute, `dlopen` or `eval` any content derived from an input file.
- Acceptance: Given the fuzz corpus `core/tests/fuzz/corpus/*`, when `ovt probe` and `ovt index` run over every file under ASan/UBSan, then no process crashes and no sanitizer report is produced (CI job `fuzz-smoke`).

**FR-GEN-008** (Phase 0) — Official binaries MUST be produced by `cmake --preset <name>` for the presets `macos-arm64-release`, `macos-x86_64-release`, `linux-x64-release`, `linux-arm64-release`, `android-arm64-release` and `android-x86_64-release` with no manual steps other than installing the documented toolchain.
- Acceptance: Given a `v*` tag, when the `release` workflow (`.github/workflows/release.yml`, chapter 13 §13.7.1) runs, then all six presets produce their artefacts (DMG, AppImage/Flatpak, APK) and upload them.

**FR-GEN-009** (Phase 1) — Byte-identical input files MUST produce byte-identical `StreamIndex`, `ContainerTree`, `FrameAnalysis` and `MetricsResult` FlatBuffers on every supported platform and on every run (chapter 0 §0.7.5).
- Acceptance: Given `corpus/syn_{h264,h265,av1}_gop60_1080p`, when `ovt index --flatbuffers` runs twice on macOS and once on Linux, then the SHA-256 of the three outputs are equal (`test:DeterminismCrossPlatform`).

**FR-GEN-010** (Phase 1) — The CLI and GUI MUST report the same version string (`<major>.<minor>.<patch>+<git-sha>`), the list of loaded plugins with their `id`, `version` and `license`, and the versions of FFmpeg, libaom, dav1d, libde265, libvmaf, libplacebo and Qt they were built against.
- Acceptance: Given a build, when `ovt --version --format json` runs and the About dialog is opened in `ovt-studio`, then the two `plugins[]` and `third_party[]` lists are identical.

**FR-GEN-011** (Phase 1) — OVT MUST accept files of at least 64 GiB and MUST NOT read the entire file into memory to open it.
- Acceptance: Given a sparse 64 GiB ISOBMFF file generated by `tools/make_sparse_mp4.py` with a 1000-frame HEVC track, when `ovt probe` runs, then it exits 0 within 5 s on the M1 reference and peak RSS stays below 512 MiB.

**FR-GEN-012** (Phase 4) — Adding a new codec MUST be possible by adding a `Codec` enum value, a header parser in `core/src/syntax/<codec>/`, an analysis plugin and codec extension tables, without modifying `libovt-render`, `ovt-studio` panels or the CLI subcommand grammar.
- Acceptance: Given the stub plugin `plugins/ovt-plugin-stub` (codec = `VP9`, returns a fixed `FrameAnalysis`), when it is placed in the plugin directory, then `ovt analyze --plugin stub corpus/syn_av1_gop60_1080p` emits a `FrameAnalysis` and `ovt-studio` draws its partition overlay without code changes to those components.

---

**FR-GEN-101** (Phase 0) — A clean clone MUST build with exactly `tools/bootstrap.sh` followed by `cmake --preset <preset> && cmake --build --preset <preset>` on each platform, with no undocumented manual step (chapter 13 §13.9.1).
- Acceptance: Given a fresh Ubuntu 22.04 VM, when the two commands run, then `build/linux-x64-release/cli/ovt --version` prints the version.

**FR-GEN-102** (Phase 0) — An incremental rebuild after touching one `.cpp` in `core/` MUST take ≤ 10 s (ninja + ccache) on the M1 reference (chapter 13 §13.2.4).
- Acceptance: Given a warm build, when `touch core/src/syntax/h265_sps.cpp && time cmake --build --preset macos-arm64-debug` runs, then wall time is ≤ 10 s.

**FR-GEN-103** (Phase 0) — A full CI build and test run per platform MUST finish in ≤ 40 min with warm caches and ≤ 120 min cold (chapter 13 §13.7).
- Acceptance: Given the GitHub Actions job durations of one week, when they are read from the Actions API, then every required job's median is ≤ 40 min and no cold run exceeds 120 min.

**FR-GEN-104** (Phase 0) — Release binaries MUST be reproducible given the same toolchain pins: two builds of the same tag differ only in signatures (`SOURCE_DATE_EPOCH`, `-ffile-prefix-map`, deterministic archives; chapter 13 §13.8).
- Acceptance: Given a tag, when `tools/repro_check.sh <tag>` builds it twice, then `diffoscope` reports no difference outside the signature blocks.

**FR-GEN-105** (Phase 0) — Every source file (`.c .h .cpp .hpp .fbs .py .kt .qml .js .cmake .sh .toml .yml .yaml .rb`, `CMakeLists.txt`) MUST carry an SPDX header whose licence equals its directory's licence per chapter 15 §15.1.1.
- Acceptance: Given the repository, when `reuse lint` runs in the `lint` CI job, then it reports compliance; when `tools/check_spdx_dirs.py` runs, then no file's SPDX id differs from its directory's policy.

**FR-GEN-106** (Phase 0) — No file under `core/`, `render/`, `core/include/ovt/abi/` or `core/schema/` MAY include, link or copy code whose licence is GPL (any version), LGPL-3.0-only, or any licence not in the LGPL-2.1 allow-list of `tools/license_policy.toml` (chapter 15 §15.3, §15.4.2).
- Acceptance: Given the `license-gate` job, when it runs on a release preset, then the `ovt_core` link graph contains only `LGPL-2.1-or-later`-compatible libraries and the job passes; given a fixture where `ovt_core` links a GPL-3.0-only library, then the job fails with `E-LINK`.

**FR-GEN-107** (Phase 0) — Binary files that cannot carry headers (icons, test vectors, fonts) MUST be covered by a `.license` sidecar file or a `REUSE.toml` entry.
- Acceptance: Given the repository, when `reuse lint` runs, then no file is reported as missing licensing information.

**FR-GEN-108** (Phase 0) — A contribution that changes a file's licence or moves code between directories with different licences MUST be a separate PR titled `licence:` and reviewed by two maintainers listed in `MAINTAINERS.md` as licence reviewers (chapter 15 §15.7.3).
- Acceptance: Given a PR that changes an `SPDX-License-Identifier` line, when the `lint` job runs, then it fails unless the title starts with `licence:`; given such a PR, when review is requested, then CODEOWNERS requires two approvals from the licence-reviewer team.

## 4.2 CNT — Container

**Rationale.** Owner capability (5): container inspection, MP4 required. chapter 3 rows 5–8 show that every primary benchmark has a byte-offset-linked box tree. ADR-006 fixes an own ISOBMFF reader with field-level spans; libavformat only demuxes. Requirements here define what the `ContainerTree` must contain and how fast it must be built.

**FR-CNT-001** (Phase 1) — The ISOBMFF reader MUST parse every box in a file into a `BoxNode` with `fourcc`, `file_span` (whole box including header), `header_length` (8, 16 for `largesize`, +16 for `uuid`) and `children`, including boxes whose type is unknown to the registry (emitted with empty `fields` and a non-empty `payload_span`).
- Acceptance: Given the `fast` ISOBMFF files (`corpus/{mpegff,gpac,bento4,cta,apple}_*` and `corpus/syn_mp4_*`, ≥ 40 files), when `ovt probe --boxes` runs, then for every file the sum of top-level `file_span.length` equals the file size and the tree matches the box list of `MP4Box -diso` (box type and offset) in `test:IsobmffTreeOracle`.

**FR-CNT-002** (Phase 1) — The reader MUST decode the payload fields of at least these boxes into `Field` nodes with `BitSpan` relative to the box start and `spec_ref`: `ftyp`, `mvhd`, `tkhd`, `mdhd`, `hdlr`, `elst`, `stsd` and its sample entries, `avcC` (ISO/IEC 14496-15 §5.3.3.1), `hvcC` (§8.3.3.1), `av1C` (AV1 ISOBMFF binding §2.3), `colr`, `pasp`, `clap`, `mdcv`, `clli`, `btrt`, `stts`, `ctts`, `stss`, `stsc`, `stsz`/`stz2`, `stco`/`co64`, `sdtp`, `sgpd`, `sbgp`, `mehd`, `trex`, `mfhd`, `tfhd`, `tfdt`, `trun`, `sidx`, `ssix`, `emsg`, `pssh`, `tenc`, `senc`, `saiz`, `saio`, `schm`, `frma`.
- Acceptance: Given `corpus/syn_h264_paff`, when `ovt probe --boxes --format json` runs, then every field value listed in `tests/golden/container/syn_h264_paff.fields.expected.json` (generated from `MP4Box -diso` and `mp4ff-info`) matches by name and value (`test:IsobmffFieldsOracle`).

**FR-CNT-003** (Phase 1) — Field names MUST be the exact identifiers used in ISO/IEC 14496-12 syntax (for example `creation_time`, `timescale`, `sample_delta`, `default_sample_flags`), and `Field.spec_ref` MUST name the section (for example `ISO/IEC 14496-12 8.6.1.2`).
- Acceptance: Given the generated table `core/src/container/isobmff_fields.inc`, when `tools/check_field_names.py` compares it against `tools/registry/14496-12-syntax.json`, then zero mismatches are reported.

**FR-CNT-004** (Phase 1) — The reader MUST never read the payload of `mdat` or `idat` boxes while building the tree, and MUST expand the children of `moov`, `trak`, `moof` and `meta` lazily through `ovt_container_expand` so that opening a file reads at most the box headers plus `moov`/`moof` payloads.
- Acceptance: Given the 64 GiB sparse file of FR-GEN-011, when `ovt_container_open` and `ovt_container_tree` are called under `strace -e pread64` (Linux), then the total bytes read is less than 16 MiB (`test:ContainerLazyRead`).

**FR-CNT-005** (Phase 1) — For each video track the reader MUST produce a `TrackInfo` with `track_id`, `handler`/`handler_fourcc`, `codec` (from the sample entry: `avc1`/`avc3` → H264, `hvc1`/`hev1` → H265, `av01` → AV1), `timescale`, `duration`, `language`, `width`/`height` and `coded_width`/`coded_height` (from `tkhd` and the sample entry), `edit_list_present`, `sample_count`, `codec_private` bytes (`avcC`/`hvcC`/`av1C` payload) and the `ovt_ColorDescription` derived from `colr` (`nclx`), `mdcv` and `clli`.
- Acceptance: Given `corpus/syn_h265_main10_pq_hdr`, when `ovt probe --format json` runs, then `tracks[0].color.transfer == 16`, `tracks[0].color.primaries == 9`, `tracks[0].color.max_cll == 1000` and `tracks[0].color.mdcv_max_luminance == 10000000`.

**FR-CNT-006** (Phase 1) — The reader MUST build a `SampleTable` view for each track giving, for every sample, `offset`, `size`, `dts`, `cts_offset` (CTS = `dts + cts_offset`, after `ctts`), `is_sync` (from `stss` or `trun` sample flags), `chunk_index` and `fragment_index`, for both non-fragmented (`moov`-only) and fragmented (`moof`-based, including `tfdt` and `default_base_is_moof`) files.
- Acceptance: Given `corpus/{syn_h264_gop60_1080p,syn_mp4_frag_cmaf}`, when `ovt probe --samples --format json` runs, then the per-sample `file_offset`, `size`, `dts`, `cts` match `ffprobe -show_packets` (`pos`, `size`, `dts`, `pts`) for every sample (`test:SampleTableOracle`).

**FR-CNT-007** (Phase 1) — The reader MUST split each sample of an `avc1`/`hvc1`/`hev1`/`avc3` track into NAL units using the `lengthSizeMinusOne` value from `avcC`/`hvcC` (ISO/IEC 14496-15 §5.3.4.2) and each sample of an `av01` track into OBUs (AV1 ISOBMFF binding §2.4), producing `Unit` records with `file_span`, `prefix_span` and `sample_index`.
- Acceptance: Given `corpus/syn_{h264,h265,av1}_gop60_1080p`, when `ovt index --format json` runs, then the count and byte lengths of `units[]` equal those produced by `ffmpeg -c:v copy -bsf:v trace_headers -f null -` for every sample.

**FR-CNT-008** (Phase 1) — When a track is encrypted (`encv` sample entry with `sinf`/`schm`/`tenc`), the reader MUST report `EncryptionInfo` (`scheme_type` = `cenc`/`cbc1`/`cens`/`cbcs`, `default_kid`, `default_is_protected`, `pssh` system IDs) and MUST mark the stream as not decodable with diagnostic `CNT-0040` rather than attempting to parse encrypted slice data.
- Acceptance: Given `corpus/syn_mp4_cenc_cbcs`, when `ovt probe --format json` runs, then `tracks[0].encryption.scheme == "cbcs"` and `diagnostics[]` contains `CNT-0040`; when `ovt index` runs, then it exits with status `OVT_E_UNSUPPORTED` (exit code 4) without a crash.

**FR-CNT-009** (Phase 1) — The reader MUST validate structural constraints and emit `Diagnostic` entries for at least: box extends past parent (`CNT-0108`), `stsz` sample count ≠ `stts` total (`CNT-2005`), `stco` offset beyond EOF (`CNT-2007`), missing `stsd` (`CNT-1011`), `trun` without `tfhd` (`CNT-1011`), `sidx` referenced range mismatch (`CNT-4030`), `ftyp` not first box (`CNT-0002`, Warning) — codes per the registry in chapter 8 §8.5.
- Acceptance: Given `corpus/syn_mp4_broken_*` (one file per code), when `ovt probe --format json` runs, then each file's `diagnostics[0].code` equals the code encoded in its filename.

**FR-CNT-010** (Phase 1) — The IVF reader MUST parse the 32-byte file header (`DKIF`, version, header size, fourcc, width, height, frame rate numerator/denominator, frame count) and the 12-byte frame headers (frame size, 64-bit pts) into a `ContainerTree` with `format = "ivf"`, and MUST hand each frame payload to the OBU splitter.
- Acceptance: Given `corpus/aom_*` (AV1 conformance set), when `ovt probe --format json` runs, then `frame_count` and per-frame `pts` match `aomdec --summary` output.

**FR-CNT-011** (Phase 1) — The Annex B reader MUST split H.264 and HEVC byte streams at 3- and 4-byte start codes (H.264 Annex B §B.1.1; H.265 Annex B §B.2), MUST attribute `zero_byte` and `trailing_zero_8bits` to `prefix_span`, and MUST exclude `cabac_zero_words` from the NAL payload.
- Acceptance: Given the `corpus/syn_h264_*` and `corpus/syn_h265_*` Annex B elementary streams (`.264`/`.265`), when `ovt index --format json` runs, then `units[].file_span.offset` equals the NAL offsets printed by `ffmpeg -bsf:v trace_headers` for all units.

**FR-CNT-012** (Phase 1) — The raw AV1 OBU reader MUST support both the low-overhead format (AV1 §5.2, `obu_has_size_field = 1`) and Annex B (`temporal_unit_size`, `frame_unit_size`, `obu_length` leb128 prefixes), MUST group OBUs into temporal units at each `OBU_TEMPORAL_DELIMITER`, and MUST reject `obu_has_size_field = 0` outside Annex B with `CNT-OBU-0001`.
- Acceptance: Given `corpus/syn_av1_{lowoverhead,annexb}`, when `ovt index --format json` runs, then the temporal-unit count equals the count reported by `aomdec --annexb`/`aomdec --obu` and `tests/corpora/av1_obu_expected.json`.

**FR-CNT-013** (Phase 1) — The Y4M reader MUST parse the stream header tags `W`, `H`, `F`, `I`, `A`, `C` (including `C420jpeg`, `C420paldv`, `C420mpeg2`, `C420p10`, `C422p10`, `C444p12`, `Cmono`) and `X` comments, MUST map `C` to `chroma_format`, `bit_depth` and `chroma_siting`, and MUST expose each `FRAME` as a raw sample.
- Acceptance: Given `corpus/syn_raw_*` (one per `C` tag), when `ovt probe --format json` runs, then `tracks[0].chroma_format`, `bit_depth` and `frame_count` match `ffprobe -show_streams`.

**FR-CNT-014** (Phase 1) — For headerless raw YUV input, OVT MUST require an explicit format description in the grammar `--raw-format WxH:pix_fmt[:fps]` of chapter 12 §12.1.5 (D21; `fps` as `num/den` or a decimal) (`pix_fmt` is an FFmpeg pixel-format name from the accepted set of chapter 8 §8.6.6, for example `yuv420p`, `yuv420p10le`, `nv12`, `p010le`, `yuyv422`, `uyvy422`, `v210`; frame rate optional; `--raw-offset <bytes>` gives the start offset) and MUST compute `frame_count = floor((file_size − start_offset) / frame_bytes)`.
- Acceptance: Given `corpus/syn_raw_1080p_p010` (10 frames), when `ovt probe --raw-format 1920x1080:p010le <file> --format json` runs, then `frame_count == 10` and `bit_depth == 10`.

**FR-CNT-015** (Phase 4) — OVT MUST parse Matroska/WebM (EBML) element trees with byte spans for every element and MUST produce `TrackInfo` and `SampleTable` views for `V_MPEG4/ISO/AVC`, `V_MPEGH/ISO/HEVC` and `V_AV1` tracks including `CodecPrivate`, `BlockGroup`/`SimpleBlock` timestamps and `Cues`.
- Acceptance: Given `corpus/syn_mkv_*`, when `ovt probe --boxes --format json` runs, then the element list (ID, offset, size) matches `mkvinfo -v` and sample offsets match `ffprobe -show_packets`.

**FR-CNT-016** (Phase 4) — OVT MUST parse MPEG-2 Transport Streams (ISO/IEC 13818-1) with 188- and 192-byte packets, PAT/PMT, PES headers (PTS/DTS), and MUST produce a `SampleTable` per video PID with access-unit boundaries, plus a bitrate graph source with 10, 100, 250 and 500 ms windows.
- Acceptance: Given `corpus/syn_ts_*` and `corpus/ts_h264_dvb`, when `ovt probe --boxes --format json` runs, then PID list and PES PTS/DTS match `ffprobe -show_packets`, and `ovt probe --bitrate 100ms` emits one row per 100 ms window with `bits` summing to the file's video payload bits.

**FR-CNT-017** (Phase 4) — OVT MUST open a DASH MPD or HLS master/media playlist as a **virtual file** that concatenates the initialisation segment and media segments of one selected representation/rendition in order, exposing the result through the same `ovt_File` interface so that the ISOBMFF reader and `StreamIndex` work unchanged.
- Acceptance: Given `corpus/dashif_testcase_1a` (its `manifest.mpd`; local HTTP server started by `tests/serve_corpus.py`), when `ovt probe http://localhost:8080/manifest.mpd --representation 1 --format json` runs, then the output `tracks[0].sample_count` equals the sum of samples in all segments of representation 1.

**FR-CNT-018** (Phase 1) — The container tree build for a 1-hour, 1080p, 30 fps progressive MP4 (108 000 samples, `moov` ≈ 2 MiB) MUST complete within the `bench_container_open_moov_1h` budget of chapter 14 §14.9.3 (`tests/perf/budgets.json`, D19) on the M1 reference, excluding lazy children.
- Acceptance: Given `corpus/syn_h264_1h_1080p`, when `bench_container_open_moov_1h` and `bench_container_sample_table_1h` run on `m1-air-8gb` (chapter 14 §14.9.3), then their medians meet the budgets in `tests/perf/budgets.json`, and `ovt probe --time` reports `container_ms` within the same budget.

---

**FR-CNT-101** (Phase 1) — The ISOBMFF reader MUST survive truncated, size-0, size-1, overlapping and negative-size boxes, emitting diagnostics instead of throwing out of the module (chapter 8 §8.2.3).
- Acceptance: Given every file in `tests/corpora/synthetic/isobmff-broken/`, when it is opened, then `ovt_container_open` returns `OVT_OK` or `OVT_E_CORRUPT` (never crashes; ASan clean) and `ContainerTree.diagnostics` is non-empty.

**FR-CNT-102** (Phase 1) — The reader MUST compute an `HdrMetadata` summary combining box-level and bitstream-level signalling and MUST flag disagreements with `CNT-5001`…`CNT-5006` (chapter 8 §8.4.6).
- Acceptance: Given `corpus/syn_h265_main10_pq_hdr` (MP4-wrapped), when `ovt probe --hdr --format json` runs, then `HdrMetadata.kind == HDR10`, `max_cll == 1000`, `max_fall == 400` and `color.transfer == 16`; given `corpus/syn_h265_main10_pq_hdr_mismatch` (`colr` BT.709 over a BT.2020 SPS), then `CNT-5001` is reported.

**FR-CNT-103** (Phase 1) — `Demuxer` MUST map every libavformat stream to a `TrackInfo` or report `CNT-0901` (chapter 8 §8.1.4).
- Acceptance: Given every corpus MP4, when `ovt probe --demux-check` runs, then every track with `handler_type` in {`vide`, `soun`, `subt`, `text`} has `avstream_index ≥ 0`, or a `CNT-0901` diagnostic names it.

**FR-CNT-104** (Phase 1) — The GUI and the Python package MUST obtain file bytes only through `ovt_container_bytes` / `ovt_stream_unit_bytes` and MUST NOT map or open the file themselves (chapter 8 §8.8).
- Acceptance: Given the `app/` and `python/` source trees, when the CI lint `no_direct_file_access` (`grep -rn "mmap\|QFile::map" app/ python/`) runs, then it returns no match.

**FR-CNT-105** (Phase 1) — When a raw YUV file is opened without a format description, OVT MUST propose a format when the file size is an exact multiple of a candidate frame size (filename and size heuristics of chapter 8 §8.6.6) and MUST pre-fill the GUI dialog with it.
- Acceptance: Given `foo_1920x1080_yuv420p10le.yuv`, when it is opened without a sidecar, then the proposed format is `1920x1080:yuv420p10le` and the Open Raw dialog is pre-filled with it.

**FR-CNT-106** (Phase 1) — Every container parser MUST have a libFuzzer target that runs 10 minutes in the nightly fuzz lane without a crash, timeout or leak (chapter 8 §8.9.4).
- Acceptance: Given `cmake --preset linux-x64-fuzz`, when `ctest -L fuzz-smoke` runs, then all `fuzz_container_*` targets exit 0.

## 4.3 SYN — Syntax / bitstream

**Rationale.** Owner capability (1) and persona commitment A1 (chapter 3 §3.4.1). ADR-007 fixes own position-recording parsers for header-level syntax; the value of OVT's syntax view over FFmpeg's `trace_headers` is the `BitSpan` on every `SyntaxNode` that makes the hex linkage (UX-5) possible. The requirements name every header the parser must cover for the three codecs and the oracle each is validated against.

**FR-SYN-001** (Phase 0) — All bitstream reads MUST go through `ovt::BitReader`, which MUST bounds-check every read, MUST record the bit position of every read so that `SyntaxNode.span` can be filled, and MUST throw `ovt::ParseError` (converted to `OVT_E_CORRUPT` plus a `Diagnostic` at the module boundary) on any read past the end of the unit.
- Acceptance: Given `test:BitReaderBounds`, when a 3-byte buffer is read with `u(32)`, then `ovt::ParseError` is thrown and `position()` is unchanged; when `fuzz_bitreader` runs for 60 s in CI, then no crash occurs.

**FR-SYN-002** (Phase 1) — `BitReader` MUST implement the descriptors `u(n)`, `f(n)`, `b(8)`, `i(n)`, `ue(v)`, `se(v)`, `te(v)`, `me(v)` (H.264 §7.2 and §9.1), `st(v)`, and the AV1 descriptors `leb128()`, `su(n)`, `ns(n)`, `uvlc()`, `le(n)`, `L(n)` and `NS(n)` (AV1 §4.10), and MUST set `SyntaxNode.descriptor` to the descriptor string used.
- Acceptance: Given `test:BitReaderDescriptors`, when each descriptor is applied to the test vectors in `core/tests/syntax/descriptor_vectors.json`, then every decoded value and consumed bit count matches.

**FR-SYN-003** (Phase 1) — The H.26x unit layer MUST remove emulation prevention bytes (H.264 §7.4.1.1; H.265 §7.4.2) to form the RBSP, MUST record each removed byte's offset in `Unit.epb_positions`, and MUST provide a function `rbsp_bit_to_raw_byte(unit, bit_offset)` that maps an RBSP bit offset to the raw file byte and bit using `epb_positions`.
- Acceptance: Given `corpus/syn_h264_epb_dense` (≥ 50 EPBs in SPS/PPS), when `test:EpbMapping` maps every `SyntaxNode.span` of the SPS back to file bytes and re-reads the bits from the raw file, then the re-read value equals the parsed value for every node.

**FR-SYN-004** (Phase 1) — The H.264 parser MUST produce a `SyntaxNode` tree for `nal_unit_header` (§7.3.1), `seq_parameter_set_rbsp` including `vui_parameters` and `hrd_parameters` (§7.3.2.1.1, §E.1.1, §E.1.2), `seq_parameter_set_extension_rbsp` (§7.3.2.1.2), `pic_parameter_set_rbsp` (§7.3.2.2), `sei_rbsp` with each `sei_message` (§7.3.2.3), `access_unit_delimiter_rbsp` (§7.3.2.4), `slice_header` with `ref_pic_list_modification`, `pred_weight_table` and `dec_ref_pic_marking` (§7.3.3, §7.3.3.1–§7.3.3.3), and `filler_data_rbsp` (§7.3.2.7), for Baseline, Main, High, High 10, High 4:2:2 and High 4:4:4 Predictive profiles.
- Acceptance: Given `corpus/jvt_*` (JVT conformance suite, professional profiles subset), when `ovt syntax <file> --format json` runs, then every leaf value equals the value printed by `ffmpeg -bsf:v trace_headers` for the same element name (`test:H264SyntaxOracle`, allowing only the element names listed in `core/tests/syntax/h264_oracle_skip.txt`).

**FR-SYN-005** (Phase 1) — The H.265 parser MUST produce a `SyntaxNode` tree for `nal_unit_header` (§7.3.1.2), `video_parameter_set_rbsp` (§7.3.2.1), `seq_parameter_set_rbsp` with `profile_tier_level`, `scaling_list_data`, `st_ref_pic_set`, `vui_parameters`, `hrd_parameters` and `sps_range_extension` (§7.3.2.2, §7.3.3, §7.3.4, §7.3.7, §E.2.1, §E.2.2, §7.3.2.2.2), `pic_parameter_set_rbsp` with `pps_range_extension` (§7.3.2.3), `sei_rbsp` (§7.3.2.4), `access_unit_delimiter_rbsp`, `end_of_seq_rbsp`, `end_of_bitstream_rbsp`, `filler_data_rbsp` (§7.3.2.5–§7.3.2.8), and `slice_segment_header` with `short_term_ref_pic_set`, `pred_weight_table` and `slice_segment_header_extension` (§7.3.6, §7.3.6.1–§7.3.6.3).
- Acceptance: Given `corpus/jctvc_*` (HEVC conformance suite, Main/Main10), when `ovt syntax <file> --format json` runs, then every leaf value equals the `trace_headers` value for the same element (`test:H265SyntaxOracle`).

**FR-SYN-006** (Phase 1) — The AV1 parser MUST produce a `SyntaxNode` tree for `obu_header` and `obu_extension_header` (§5.3.2, §5.3.3), `sequence_header_obu` with `color_config`, `timing_info`, `decoder_model_info` and `operating_parameters_info` (§5.5), `temporal_delimiter_obu` (§5.6), `frame_header_obu`/`frame_obu` with `uncompressed_header` and all its sub-syntax including `frame_size`, `render_size`, `superres_params`, `interpolation_filter`, `tile_info`, `quantization_params`, `segmentation_params`, `delta_q_params`, `delta_lf_params`, `loop_filter_params`, `cdef_params`, `lr_params`, `read_tx_mode`, `frame_reference_mode`, `skip_mode_params`, `global_motion_params` and `film_grain_params` (§5.9.1–§5.9.30), `tile_group_obu` header fields `tile_start_and_end_present_flag`, `tg_start`, `tg_end` and per-tile `tile_size_minus_1` (§5.11.1), `metadata_obu` (§5.8, including `metadata_hdr_cll` and `metadata_hdr_mdcv`), `tile_list_obu` (§5.12) and `padding_obu` (§5.7).
- Acceptance: Given `corpus/argon_*` (Argon conformance subset) and `corpus/aom_*`, when `ovt syntax <file> --format json` runs, then every leaf value equals the value in `aomdec --all-layers` debug trace or the GStreamer `av1parse` debug log listed in `core/tests/syntax/av1_oracle.json` (`test:Av1SyntaxOracle`).

**FR-SYN-007** (Phase 1) — Every `SyntaxNode.name` MUST be the exact syntax element name from the governing specification table (for example `log2_max_pic_order_cnt_lsb_minus4`, `sps_temporal_mvp_enabled_flag`, `base_q_idx`), every `SyntaxNode.spec_ref` MUST name the syntax table section (for example `H.265 7.3.2.2.1`, `AV1 5.9.12`), and container-like syntax structures MUST be non-leaf nodes named after the syntax function (`vui_parameters`, `tile_info`).
- Acceptance: Given the parser source, when `tools/check_syntax_names.py` compares every string literal passed to `BitReader::read_*` against `tools/registry/{h264,h265,av1}-syntax-elements.txt`, then zero unknown names are reported.

**FR-SYN-008** (Phase 1) — For every syntax element with an enumerated meaning, the parser MUST fill `SyntaxNode.enum_name` with the specification's symbolic name (for example H.264 `slice_type` 2 → `I`, H.265 `nal_unit_type` 19 → `IDR_W_RADL`, AV1 `frame_type` 0 → `KEY_FRAME`, `interpolation_filter` 4 → `SWITCHABLE`, `color_primaries` 9 → `CP_BT_2020`).
- Acceptance: Given `corpus/syn_{h264,h265,av1}_gop60_1080p`, when `ovt syntax --format json` runs, then every `enum_name` equals the entry in `core/tests/syntax/enum_expected.json`.

**FR-SYN-009** (Phase 1) — The parser MUST fill `SyntaxNode.derived` for at least these derived variables, formatted as `<Name> = <value>`: H.264 `PicWidthInMbs`, `FrameHeightInMbs`, `MaxFrameNum`, `MaxPicOrderCntLsb`, `ChromaArrayType`, `BitDepthY`, `BitDepthC`, `SliceQPY`; H.265 `MinCbLog2SizeY`, `CtbLog2SizeY`, `CtbSizeY`, `PicWidthInCtbsY`, `PicHeightInCtbsY`, `MaxPicOrderCntLsb`, `SliceQpY`, `NumPicTotalCurr`; AV1 `FrameWidth`, `FrameHeight`, `UpscaledWidth`, `MiCols`, `MiRows`, `TileCols`, `TileRows`, `NumPlanes`, `BitDepth`, `OrderHint`, `qindex`.
- Acceptance: Given `corpus/syn_{h264,h265,av1}_gop60_1080p`, when `ovt syntax --format json` runs, then the `derived` strings match `core/tests/syntax/derived_expected.json`.

**FR-SYN-010** (Phase 1) — The parser MUST resolve parameter-set activation: each slice/frame header node MUST reference the `Unit.index` of the active SPS/PPS (and VPS) or AV1 sequence header, using the most recent parameter set with the same id in decode order, and MUST emit `SYN-<codec>-0010` when a slice references a parameter-set id that has not been received.
- Acceptance: Given `corpus/syn_h265_multi_pps` (3 PPS ids, re-sent mid-stream), when `ovt syntax --format json` runs, then each slice's `active_pps_unit` equals the expected unit index in `tests/golden/syntax/syn_h265_multi_pps.expected.json`; given `corpus/syn_broken_h264_missing_pps`, then `diagnostics[]` contains `SYN-H264-0010`.

**FR-SYN-011** (Phase 1) — The parser MUST decode these SEI messages and metadata OBUs into named fields: H.264/H.265 `buffering_period`, `pic_timing`, `user_data_registered_itu_t_t35` (with ITU-T T.35 country/provider/… header and HDR10+ ST 2094-40 payload fields when `terminal_provider_code == 0x003C`), `user_data_unregistered` (with UUID and x264/x265 encoder string when present), `recovery_point`, `mastering_display_colour_volume` (H.265 §D.2.28), `content_light_level_info` (§D.2.35), `alternative_transfer_characteristics` (§D.2.38), `decoded_picture_hash` (§D.2.20; MD5, CRC, checksum), `active_parameter_sets`, `time_code`; AV1 `METADATA_TYPE_HDR_CLL`, `METADATA_TYPE_HDR_MDCV`, `METADATA_TYPE_ITUT_T35`, `METADATA_TYPE_SCALABILITY`, `METADATA_TYPE_TIMECODE` (§5.8.2–§5.8.7).
- Acceptance: Given `corpus/syn_sei_*` (one file per message type), when `ovt syntax --format json` runs, then every field matches `trace_headers` or `tests/corpora/sei_expected.json`.

**FR-SYN-012** (Phase 1) — For SEI messages and metadata whose payload type is unknown, the parser MUST emit a node named `reserved_sei_message` / `metadata_unknown` with `payload_type`, `payload_size` and the raw `bytes`, and MUST NOT emit an error.
- Acceptance: Given `corpus/syn_h265_sei_unknown_200`, when `ovt syntax --format json` runs, then the SEI node has `payload_type == 200`, `bytes.length == payload_size`, and `diagnostics[]` is empty.

**FR-SYN-013** (Phase 1) — The parser MUST emit a `Diagnostic` with `severity ∈ {Warning, Error}`, `code`, `span` and `spec_ref` for at least these conditions: value outside the range stated in the semantics (for example H.265 `log2_max_pic_order_cnt_lsb_minus4 > 12`, §7.4.3.2.1), `forbidden_zero_bit != 0`, missing `rbsp_trailing_bits`, trailing non-zero bytes after `rbsp_stop_one_bit`, `nuh_layer_id`/`nuh_temporal_id_plus1 == 0` (H.265 §7.4.2.2), AV1 `obu_forbidden_bit != 0`, OBU size extending past the temporal unit, unknown `nal_unit_type`/`obu_type` (Warning).
- Acceptance: Given `corpus/syn_broken_syntax_*` (one file per condition, filename carries the code), when `ovt syntax --format json` runs, then `diagnostics[].code` contains the filename's code and `span.bit_offset` points inside the offending element.

**FR-SYN-014** (Phase 1) — The CLI and GUI MUST be able to show the raw bytes and the RBSP bytes of any `Unit` (via `ovt_stream_unit_bytes`) and the GUI MUST highlight the bits covered by the selected `SyntaxNode.span` in the raw view after EPB mapping, in both hex and binary display.
- Acceptance: Given `corpus/syn_h264_epb_dense` opened in `ovt-studio`, when the user clicks `pic_order_cnt_lsb` in the first slice header, then the Hex panel scrolls to and highlights exactly the bytes containing that element's bits, and pressing `Ctrl+B` switches to binary with the same bits highlighted (`ui-test:HexHighlightEpb`).

**FR-SYN-015** (Phase 1) — The syntax panel MUST support filtering nodes by element-name substring and by value (`name == value`, `name > value`), MUST search across all units of the stream, and MUST return results within 500 ms for a `StreamIndex` with 100 000 units on the M1 reference.
- Acceptance: Given `corpus/syn_h264_1h_1080p` opened in `ovt-studio`, when the user types `slice_qp_delta > 10` in the Syntax filter box, then the result list appears in ≤ 500 ms and contains exactly the units listed by `ovt syntax --filter "slice_qp_delta > 10" --json`.

**FR-SYN-016** (Phase 1) — The header-only parse of all non-VCL units plus slice/frame headers for a 1-hour 1080p H.264 stream (≈ 86 400 frames) MUST complete in ≤ 2 s on the M1 reference (chapter 0 §0.7.4).
- Acceptance: Given `corpus/syn_h264_1h_1080p`, when `ovt index --time` runs, then `index_ms ≤ 2000` (`test:PerfIndexH264`, 20 % tolerance against baseline).

**FR-SYN-017** (Phase 2) — The syntax panel MUST show a tooltip on every node containing `spec_ref`, the descriptor, and the one-sentence glossary definition of the element when one exists in `docs/spec/02-glossary.md`, and MUST NOT reproduce specification text verbatim.
- Acceptance: Given `ovt-studio` with `corpus/syn_h265_gop60_1080p`, when the user hovers `sps_temporal_mvp_enabled_flag`, then a tooltip shows `H.265 7.3.2.2.1 · u(1)` and the glossary sentence within 700 ms (`ui-test:SyntaxTooltip`).

**FR-SYN-018** (Phase 1) — OVT MUST extract units: `ovt extract` MUST write (a) a single unit by index, (b) all units of a frame, (c) the minimum coded video sequence needed to decode a given frame (from the preceding RAP, including active parameter sets), and (d) all RAP frames, as Annex B (`.264`/`.hevc`), low-overhead OBU (`.obu`) or IVF, and the output MUST decode with the reference decoder.
- Acceptance: Given `corpus/syn_{h264,h265,av1}_gop60_1080p`, when `ovt extract --min-cvs --frames 37 -o out.<ext>` runs and `ffmpeg -i out.<ext> -f md5 -` (or `aomdec --md5`) runs, then the decoder exits 0 and the MD5 of the last output frame equals the MD5 of display frame 37 from the full stream.

**FR-SYN-019** (Phase 4) — OVT MUST verify `decoded_picture_hash` SEI (H.265 §D.3.20) and H.264 user-data MD5 conventions against the output of the playback decoder and MUST emit `SYN-H265-0050` (hash mismatch) with the frame id and plane index on failure.
- Acceptance: Given every `corpus/jctvc_*` stream that carries a decoded picture hash SEI (not all do: RPS_A_docomo_5, RPS_F_docomo_2, POC_A_Bossen_3, AMP_A_Samsung_7, AMP_D_Hisilicon_3, CONFWIN_A_Sony_1, MAXBINS_A_TI_5, STRUCT_B_Samsung_7 and TILES_B_Cisco_1 verify through `*_yuv.md5` instead; issue #255), when `ovt verify --hash-sei <file>` runs, then it reports `0 mismatches`, and a stream without one reports `absent` for every picture; given `corpus/syn_broken_h265_hash_mismatch`, then it reports exactly 1 mismatch at the frame recorded in the filename.

**FR-SYN-020** (Phase 4) — OVT MUST check profile/tier/level limits (H.264 Annex A Table A-1 and A-3 to A-7; H.265 Annex A Tables A.8–A.9 and §A.4; AV1 Annex A §A.3 level limits) on `MaxLumaPs`, `MaxDpbSize`, `MaxLumaSr`, `MaxBR`, `MaxCpbSize`, tile counts and `MaxTileCols`/`MaxTileRows`, and MUST emit `SYN-<codec>-006x` diagnostics naming the exceeded limit and the signalled level.
- Acceptance: Given `corpus/syn_level_violation_{h264,h265,av1}`, when `ovt verify --levels <file> --format json` runs, then `diagnostics[]` contains the expected `SYN-*-006x` code with the `limit_name` recorded in `tests/corpora/level_expected.json`.

---

## 4.4 DEC — Decoding and playback

**Rationale.** ADR-009 fixes two decode paths: hardware-accelerated playback for scrubbing and an instrumented software analysis path that is GOP-local. Persona commitment A6 (reference structure) is met from header-derived data in the `StreamIndex` so that it is available before any block analysis. These requirements define frame identity, ordering, seeking and the correctness oracle for decoded pixels.

**FR-DEC-001** (Phase 1) — The `StreamIndex` MUST contain one `FrameInfo` per coded frame in decode order, assigning `FrameId.decode_index` to every coded frame including AV1 frames with `show_frame = 0` and `FrameId.display_index = −1` for frames never output, and MUST fill `display_order[]` so that `display_order[display_index]` yields the `decode_index` of the shown frame, with AV1 `show_existing_frame` frames producing a `FrameInfo` whose `show_existing_of` names the shown frame.
- Acceptance: Given `corpus/syn_av1_hidden_altref_showexisting` (contains ALTREF hidden frames and `show_existing_frame`), when `ovt index --format json` runs, then `frames.length` equals the number of frame headers, `display_order.length` equals the frame count from `aomdec --summary`, and every `show_existing_of` matches `tests/corpora/av1_show_existing_expected.json`.

**FR-DEC-002** (Phase 1) — For H.264 the index MUST derive `FrameInfo.poc` by the POC decoding process of §8.2.1 for `pic_order_cnt_type` 0, 1 and 2, MUST set `is_rap` for IDR pictures and for pictures with a `recovery_point` SEI, MUST set `structure` from `field_pic_flag`/`bottom_field_flag`/`mb_adaptive_frame_field_flag` (§7.4.3), and MUST pair complementary field pairs into one `FrameInfo` with `structure = TopField`/`BottomField` children listed in the Data Model chapter.
- Acceptance: Given `corpus/jvt_{CVPCMNL1_SVA_C,CVSE2_Sony_B,MR9_BT_B}` (POC types 0, 1, 2 and field coding), when `ovt index --format json` runs, then `frames[].poc` and `frames[].structure` equal the values from `ffmpeg -debug pict` (`test:H264PocOracle`).

**FR-DEC-003** (Phase 1) — For H.265 the index MUST derive `PicOrderCntVal` (§8.3.1), MUST set `is_rap` for `nal_unit_type` in {BLA_W_LP, BLA_W_RADL, BLA_N_LP, IDR_W_RADL, IDR_N_LP, CRA_NUT} (§7.4.2.2), MUST mark RASL pictures associated with a CRA that starts the stream as not decodable (`FrameInfo` flag `skipped_rasl`), and MUST fill `declared_refs` from the `st_ref_pic_set` and long-term entries of the slice header (§8.3.2) and `dpb_after` from the RPS marking.
- Acceptance: Given `corpus/jctvc_{RAP_A_docomo_6,RAP_B_Bossen_2,POC_A_Bossen_3}`, when `ovt index --format json` runs, then `poc`, `is_rap`, `skipped_rasl` and `declared_refs` equal `tests/corpora/h265_rps_expected.json` (generated from the HM decoder trace).

**FR-DEC-004** (Phase 1) — For AV1 the index MUST derive `OrderHint`, `frame_type` (`KEY_FRAME`, `INTER_FRAME`, `INTRA_ONLY_FRAME`, `SWITCH_FRAME` → `FrameType.Key/Inter/IntraOnly/Switch`), `is_rap` for key frames with `show_frame = 1`, MUST fill `declared_refs` from `ref_frame_idx[0..6]` resolved through the reference frame update process (AV1 §7.20) into decode indices, and MUST fill `dpb_after` with the eight reference slots after `refresh_frame_flags` is applied.
- Acceptance: Given `corpus/aom_*`, when `ovt index --format json` runs, then `declared_refs` and `dpb_after` for every frame equal the reference-slot trace produced by `tools/aom_ref_trace.py` (libaom inspection callback) (`test:Av1RefsOracle`).

**FR-DEC-005** (Phase 1) — `FrameInfo.type` for H.26x MUST be the slice-majority type (I if all slices are I, else P if no slice is B, else B), and `FrameInfo.qp_base` MUST be `26 + pic_init_qp_minus26 + slice_qp_delta` (H.264 §7.4.3) / `SliceQpY` (H.265 §7.4.7.1) of the first slice, or `base_q_idx` (AV1 §5.9.12).
- Acceptance: Given `corpus/syn_{h264,h265,av1}_gop60_1080p`, when `ovt index --format json` runs, then `frames[].type` and `frames[].qp_base` match `tests/corpora/gop_expected.json`.

**FR-DEC-006** (Phase 1) — The playback decoder MUST use hardware acceleration when `prefer_hw = 1` (VideoToolbox on macOS, MediaCodec on Android, VA-API on Linux) and MUST fall back to libavcodec software decoding (dav1d for AV1) within the same `ovt_Decoder` without caller intervention when the hardware decoder rejects the stream.
- Acceptance: Given `corpus/syn_h265_422_10bit` (unsupported by VideoToolbox on M1), when `ovt dump --hw --frames 10 --md5 <file>` runs on macOS, then the log contains `decoder: hw rejected, fallback to software` and the MD5 equals that of `ovt dump --sw`.

**FR-DEC-007** (Phase 1) — Software-decoded pictures from the playback path MUST be bit-exact with the reference decoders (JM for H.264, HM for H.265, libaom for AV1) for every stream in the conformance corpora.
- Acceptance: Given `corpus/{jvt,jctvc,argon}_*`, when `ovt dump --sw --md5 <file>` runs, then the per-frame MD5 list equals the `*.md5` file shipped with each conformance stream (`test:DecodeConformanceMd5`).

**FR-DEC-008** (Phase 1) — `ovt_decoder_seek(display_index)` MUST decode from the nearest preceding RAP and MUST return the requested frame, and the first frame of a freshly opened 1080p stream MUST be displayed in `ovt-studio` within 500 ms of the open command on the M1 reference (chapter 0 §0.7.4).
- Acceptance: Given `corpus/syn_h264_1h_1080p`, when `ovt-studio --open <file> --measure-first-frame` runs, then the logged `first_frame_ms ≤ 500`; when `ovt dump --seek 5000 --frames 1 --md5` runs, then the MD5 equals frame 5000 of a full sequential decode.

**FR-DEC-009** (Phase 1) — Playback MUST support play/pause, step forward/backward by one frame in the current order (decode or display), step to the next/previous RAP, step to the next/previous frame of a chosen type (I, P, B, Key), and a speed of 1× at the container frame rate with frame drops permitted only when the hardware decoder cannot keep up.
- Acceptance: Given `corpus/syn_h264_gop60_1080p` in `ovt-studio`, when the user presses `Ctrl+→` (next RAP, chapter 11 §11.4.3) three times from frame 0, then the current frame is the third RAP as listed by `ovt index --format json | jq '[.frames[] | select(.is_rap)][2]'`.

**FR-DEC-010** (Phase 2) — The analysis path MUST decode only the frames from the nearest preceding RAP up to and including the requested `decode_index` (GOP-local, ADR-009), MUST run on a `jobs::Pool` thread, MUST be cancellable via `ovt_Job` with cancellation observed within 50 ms, and MUST cache every produced `FrameAnalysis` so that the second request for the same frame returns from cache in ≤ 20 ms.
- Acceptance: Given `corpus/syn_h265_gop60_1080p`, when `ovt analyze --frames 45 --time` runs twice, then the first run reports `analysis_ms ≤ 1500` (libde265) and the second `cache_hit = true, analysis_ms ≤ 20`; when `test:AnalysisCancel` cancels after 100 ms, then the job returns `OVT_E_CANCELLED` within 150 ms.

**FR-DEC-011** (Phase 2) — The GOP/reference graph MUST be derivable from `StreamIndex` alone (`declared_refs`, `dpb_after`, `temporal_id`, `poc`) and MUST be refined with actual reference usage (`RefSlot.usage_cells`) once a `FrameAnalysis` is available.
- Acceptance: Given `corpus/syn_h265_bpyramid`, when `ovt index --refgraph --format json` runs, then the edge list equals `tests/corpora/bpyramid_edges.json`; when `ovt analyze --frames 8` is added, then `refs[].usage_cells` sums to `grid.cells_w * grid.cells_h` for inter cells.

**FR-DEC-012** (Phase 4) — The HM oracle plugin (`ovt-plugin-hm`) MUST decode HEVC RExt (4:2:2, 4:4:4, 12-bit) and SCC streams, MUST be selectable with `--plugin hm`, and `ovt verify --plugin-compare de265,hm` MUST report any pixel or `CellGrid` field difference between the two plugins for Main/Main10 streams.
- Acceptance: Given `corpus/jctvc_rext_*`, when `ovt dump --plugin hm --frames 0 --md5` runs, then the reconstruction MD5 equals the conformance MD5; given `corpus/jctvc_*`, when `ovt verify --plugin-compare de265,hm` runs, then it reports `0 differences`.

**FR-DEC-013** (Phase 4) — OVT MUST compute the HRD/CPB buffer fullness curve (H.264 Annex C §C.1; H.265 Annex C §C.2; AV1 §7.3 decoder model) from `hrd_parameters`/`decoder_model_info`, `buffering_period` and `pic_timing` SEI (or constant-rate arrival when absent), and MUST report underflow and overflow events as `Diagnostic` entries `DEC-<codec>-0070/0071`.
- Acceptance: Given `corpus/syn_{h264_hrd_cbr,h265_hrd_vbr}` with known fullness traces, when `ovt hrd <file> --format json` runs, then per-frame `fullness_bits` equals `tests/corpora/hrd_expected.json` within ±1 bit and the underflow frame list matches.

**FR-DEC-014** (Phase 1) — Decoding MUST honour the conformance window / `render_size` for display (`FrameInfo.crop`) and MUST expose the full coded picture when the user selects "show coded area" so that overlays in coded-picture coordinates (chapter 0 §0.1.9) align with pixels.
- Acceptance: Given `corpus/syn_h265_crop_1080_in_1088`, when `ovt dump --frames 1 --info --format json` runs, then `coded_size.h == 1088` and `crop.h == 1080`; in `ovt-studio`, when "show coded area" is toggled, then the picture height changes from 1080 to 1088 pixels at 1× zoom (`ui-test:CodedArea`).

---

## 4.5 BLK — Block-level analysis

**Rationale.** Owner capability (3) and persona commitments A2–A5, A11 (chapter 3 §3.4.1). ADR-008 fixes that block data comes from instrumented decoders behind `ovt_analysis_decoder_v1`; the neutral `CellGrid` (4×4 luma cells) and `Block` tree (chapter 0 §0.5.4) carry the data to overlays and panels. Requirements state which fields each codec plugin must fill, with the spec element each field comes from, so that the implementer of a plugin has a checklist, and the oracle that proves the values right.

**FR-BLK-001** (Phase 2) — Every analysis plugin MUST implement `ovt_analysis_decoder_v1` exactly as declared in `core/include/ovt/abi/analysis_decoder_v1.h`, MUST export only the symbol `ovt_analysis_decoder_v1`, and MUST report `abi_version == 1` and a complete `ovt_AnalysisCaps`.
- Acceptance: Given each built plugin library, when `ovt plugins --format json` runs, then each is listed with `abi_version == 1`, non-empty `id`, `version`, `license`, and `nm -gU libovt_plugin_*.{dylib,so} | grep ' T '` lists exactly one exported function.

**FR-BLK-002** (Phase 2) — Every `FrameAnalysis` MUST contain a `CellGrid` with `cells_w = ceil(coded_w / 4)` and `cells_h = ceil(coded_h / 4)`, and every non-empty vector MUST have exactly `cells_w * cells_h` elements, with `block_id` filled for every cell.
- Acceptance: Given `corpus/syn_{h264,h265,av1}_gop60_1080p`, when `test:CellGridShape` loads `ovt analyze --frame N` output for N in {0, 1, 7}, then every vector length check passes and every `block_id` indexes an existing leaf `Block` whose `rect` contains the cell.

**FR-BLK-003** (Phase 2) — The `Block` tree MUST have `blocks[0]` as a synthetic root whose `children` are the top-level units in raster order (H.264 macroblocks, HEVC CTUs, AV1 superblocks), MUST have each child's `rect` fully inside its parent's `rect`, and sibling `rect`s MUST NOT overlap.
- Acceptance: Given the same outputs as FR-BLK-002, when `test:BlockTreeInvariants` runs, then containment and non-overlap hold for every node and `blocks[0].children.length == PicWidthInCtbsY * PicHeightInCtbsY` (HEVC) / `PicSizeInMbs` (H.264) / `ceil(MiCols/sbCols) * ceil(MiRows/sbRows)` (AV1).

**FR-BLK-004** (Phase 2) — The HEVC plugin (`ovt-plugin-de265`) MUST fill, for every CU/PU/TU, the `HevcCu`, `HevcPu` and `HevcTu` extension tables with every syntax element of `coding_unit` (H.265 §7.3.8.5: `cu_transquant_bypass_flag`, `cu_skip_flag`, `pred_mode_flag`, `palette_mode_flag`, `part_mode`, `pcm_flag`, `prev_intra_luma_pred_flag`, `mpm_idx`, `rem_intra_luma_pred_mode`, `intra_chroma_pred_mode`, `rqt_root_cbf`), `prediction_unit` (§7.3.8.6: `merge_flag`, `merge_idx`, `inter_pred_idc`, `ref_idx_l0/l1`, `mvd_l0/l1`, `mvp_l0_flag/l1_flag`), `transform_tree`/`transform_unit` (§7.3.8.8, §7.3.8.10: `split_transform_flag`, `cbf_luma/cb/cr`, `cu_qp_delta_abs/sign`, `cu_chroma_qp_offset_flag/idx`), `residual_coding` summary (§7.3.8.11: `transform_skip_flag`, `explicit_rdpcm_flag`, number of non-zero coefficients, `last_sig_coeff_x/y`), and `sao` per CTU (§7.3.8.3), plus derived `IntraPredModeY`, `IntraPredModeC`, `QpY`, `Qp'Y`, `MvL0/MvL1`, `MergeCandIdx`.
- Acceptance: Given `corpus/jctvc_*`, when `ovt analyze --frames 0..3 --format json` runs and `tools/hm_trace_compare.py` compares against the HM decoder trace (`--TraceFile`), then every listed element matches for every CU (`test:HevcBlockOracle`).

**FR-BLK-005** (Phase 2) — The HEVC plugin MUST fill `CellGrid.pred`, `qp` (final `QpY` per CU), `intra_mode_y`, `intra_mode_c`, `mv0_*`, `mv1_*` (converted from quarter-sample to 1/16 units by `<< 2`), `ref0`, `ref1`, `tx_log2_w/h`, `tx_type` (0 = DCT-II, 1 = DST-VII for 4×4 intra luma, 2 = transform skip, 3 = bypass), `cbf`, `partition_id` (slice segment index), `deblock_bs_v/h` (boundary strength 0/1/2 per §8.7.2.4), `filter_idx` (`SaoTypeIdx` luma per CTU), and `bits_x8` from the plugin's per-CU bit accounting.
- Acceptance: Given `corpus/jctvc_{DBLK_A_SONY_4,SAO_A_MediaTek_4,TSKIP_A_MS_3}`, when `ovt analyze --frames 0 --format json` runs, then `deblock_bs_*`, `filter_idx`, `tx_type` match the HM trace for every cell (`test:HevcGridOracle`), and `caps.grid_fields` declares every filled vector.

**FR-BLK-006** (Phase 2) — The AV1 plugin (`ovt-plugin-aom`) MUST fill the `Av1Block` and `Av1Txb` extension tables from libaom's `CONFIG_INSPECTION` `insp_mi_data` with every per-block syntax element of `intra_frame_mode_info`/`inter_frame_mode_info` (AV1 §5.11.7, §5.11.17–§5.11.30: `skip`, `skip_mode`, `segment_id`, `is_inter`, `use_intrabc`, `y_mode`/`uv_mode`, `angle_delta_y/uv`, `use_filter_intra`, `filter_intra_mode`, `cfl_alpha_u/v`, `palette_size_y/uv`, `ref_frame[0..1]`, `compound_mode`/`comp_group_idx`/`compound_idx`, `interintra`, `motion_mode`, `interp_filter[0..1]`, `mv[0..1]`, `delta_qindex`, `delta_lf`), partition type (`PARTITION_*`, §5.11.4), `tx_size`, `tx_type` (§5.11.47, §5.11.48), `cdef_idx` (§5.11.56) and `use_wiener`/`use_sgrproj` per LR unit (§5.11.57).
- Acceptance: Given `corpus/aom_*`, when `ovt analyze --frames 0..3 --format json` runs and `tools/aom_inspect_compare.py` compares against `aomdec`'s `inspect` JSON (`examples/inspect.c` build), then every listed element matches per mode-info unit (`test:Av1BlockOracle`).

**FR-BLK-007** (Phase 2) — The AV1 plugin MUST fill `CellGrid.qp` with the per-block `qindex` after `delta_q` and segmentation (`get_qindex`, AV1 §7.12.2), `pred` (Intra, Inter, Skip when `skip && is_inter`, IntraBC, Palette), `intra_mode_y/c`, `mv0/mv1` (AV1 MVs are in 1/8 sample units; converted by `<< 1`), `ref0/ref1` as indices into `refs[]` named `LAST`…`ALTREF`, `tx_log2_w/h`, `tx_type` (`DCT_DCT` … `H_FLIPADST`, 16 values), `filter_idx` = `cdef_idx` of the 64×64 unit, `partition_id` = tile index, and `bits_x8` from `CONFIG_ACCOUNTING` symbol costs summed per block.
- Acceptance: Given `corpus/syn_av1_{tiles_cdef_lr,deltaq,palette}`, when `ovt analyze --frames 0 --format json` runs, then `qp`, `filter_idx` and `pred` equal the `inspect` JSON for every 4×4 cell (`test:Av1GridOracle`).

**FR-BLK-008** (Phase 2) — The AV1 plugin MUST fill `FrameAnalysis.symbols` with one `SymbolCost` per (4×4 position, symbol id) from libaom accounting, MUST list symbol names in `symbol_names`, and the sum of `bits_x8` over all symbols of a tile MUST be within 1 % of `tile_size_minus_1 + 1` bytes × 64.
- Acceptance: Given `corpus/aom_*`, when `test:Av1AccountingSum` runs over frame 0 of each file, then the per-tile deviation is ≤ 1 %.

**FR-BLK-009** (Phase 1) — The H.264 plugin (`ovt-plugin-ffmpeg`) MUST, using only the FFmpeg public API (`AV_CODEC_EXPORT_DATA_MVS`, `AV_CODEC_EXPORT_DATA_VIDEO_ENC_PARAMS`), fill `CellGrid.mv0_x/mv0_y/mv1_x/mv1_y` (from `AVMotionVector`, quarter-sample `<< 2`), `ref0/ref1`, `qp` (per-MB `qp` from `AVVideoEncParams` block `delta_qp` added to `qp`), `pred` ∈ {Intra, Inter, Skip} inferred from the presence of MVs and the block being coded, and `Block` nodes of kind `MB` with `rect` 16×16 and `SubMB` nodes for each `AVMotionVector` of smaller size.
- Acceptance: Given `corpus/jvt_{BA1_Sony_D,CVWP1_TOSHIBA_E}`, when `ovt analyze --frames 1 --format json` runs, then `mv0_x/y` per cell equal `ffmpeg -flags2 +export_mvs -vf codecview … -f null` side-data dumped by `tools/ffmpeg_mv_dump.py`, and `qp` equals the `-debug qp` printout.

**FR-BLK-010** (Phase 4) — The H.264 plugin MUST, through the upstream-submitted side-data extension patch (`plugins/ovt-plugin-ffmpeg/patches/0001-h264-export-mb-params.patch`, applied by WI-4.102 to the plugin FFmpeg flavour built by `third_party/ffmpeg.cmake` whatever `OVT_FFMPEG_GPL` is set to, never to the core flavour, chapter 7 §7.6.3 and chapter 13 §13.2.2; issue #331), fill the `AvcMb` extension table with `mb_type` (and its Table 7-11/7-13/7-14 name), `sub_mb_type[4]`, `transform_size_8x8_flag`, `coded_block_pattern`, `mb_qp_delta`, `intra_chroma_pred_mode`, `prev_intra4x4_pred_mode_flag`/`rem_intra4x4_pred_mode` (or 8×8 equivalents), `Intra16x16PredMode`, `ref_idx_l0/l1[4]`, `mvd_l0/l1`, `mb_skip_flag`, `mb_field_decoding_flag` (H.264 §7.3.5, §7.3.5.1, §7.3.5.2), and MUST fill `intra_mode_y`, `tx_log2_w/h`, `cbf` in `CellGrid`.
- Acceptance: Given `corpus/jvt_*`, when `ovt analyze --frames 0..3 --format json` runs with the patched FFmpeg and `tools/jm_trace_compare.py` compares against the JM decoder `-p TraceFile`, then every listed element matches per macroblock (`test:AvcMbOracle`); given the unpatched build, then `caps.grid_fields` omits `intra_mode_y` and the GUI greys out the intra-mode overlay.

**FR-BLK-011** (Phase 2) — Every plugin MUST fill `FrameAnalysis.mvs` with one `MotionVector` per prediction block per list, including `mvd_x/y` when the plugin has them, `predictor` as the spec's candidate name (HEVC `merge[k]`/`amvp[k]`; AV1 `NEARESTMV`/`NEARMV`/`GLOBALMV`/`NEWMV`/`NEAREST_NEARESTMV` …; H.264 `p_skip`/`b_direct_spatial`/`b_direct_temporal`/`mvd`), and `flags` bits (bi-pred, weighted, global/warped, sub-block, field) as defined in chapter 0 §0.5.4.
- Acceptance: Given `corpus/syn_{h265,av1}_gop60_1080p`, when `test:MotionVectorFlags` loads frame 2, then every `MotionVector.rect` equals its block's `rect` (or a sub-block for `flags & 8`), `predictor` is non-empty, and the count of `flags & 1` entries equals the count of bi-predicted PUs/blocks in the oracle trace.

**FR-BLK-012** (Phase 2) — Every plugin MUST fill `FrameAnalysis.refs` with one `RefSlot` per active reference (`L0[i]`/`L1[i]` for H.26x per H.265 §8.3.4 / H.264 §8.2.4; `LAST`, `LAST2`, `LAST3`, `GOLDEN`, `BWDREF`, `ALTREF2`, `ALTREF` for AV1), each with the referenced `FrameId`, `is_long_term`, and `usage_cells` equal to the number of cells whose `ref0` or `ref1` equals that slot.
- Acceptance: Given `corpus/syn_{h264,h265,av1}_gop60_1080p`, when `test:RefSlots` loads frame 3, then `refs[].frame.decode_index` ⊆ `FrameInfo.declared_refs` and `Σ usage_cells == count(ref0 ≥ 0) + count(ref1 ≥ 0)`.

**FR-BLK-013** (Phase 2) — Every plugin MUST fill `FrameAnalysis.partitions` with one `PartitionInfo` per slice (H.26x) or tile (AV1) containing the partition index, its `Rect` (slice: bounding box of its CTUs/MBs; tile: tile rectangle), `ByteSpan` of its data in the file, and the `Unit.index` of its header unit.
- Acceptance: Given `corpus/syn_h265_tiles_2x2` and `corpus/syn_av1_tiles_cdef_lr` (chapter 14 §14.3.3 recipes 7 and 13), when `ovt analyze --frames 0 --format json` runs, then `partitions.length` equals the signalled tile count — 4 for the HEVC stream (2×2 uniform tiles) and `TileCols × TileRows` of the AV1 frame header for the AV1 stream (8 with the recipe's log2 options `--tile-columns=2 --tile-rows=1`) and the `Rect`s tile the coded picture without gaps or overlap.

**FR-BLK-014** (Phase 2) — Every plugin whose `caps.plane_stages` declares them (the HEVC and AV1 plugins; the public-API H.264 plugin reports n/a) MUST export, on request via `plane_stage_mask`, the planes `Reconstruction`, `Prediction`, `Residual`, `PreDeblock` and `PreSaoOrCdef` (HEVC: pre-SAO; AV1: pre-CDEF, plus `PreLoopRestoration`) as `PlaneRef` blobs at native bit depth, with `Residual` stored as signed 16-bit, and `Reconstruction + 0 == Prediction + Residual` sample-wise before in-loop filtering.
- Acceptance: Given `corpus/syn_{h265,av1}_gop60_1080p`, when `test:PlaneStageIdentity` requests all stages for frame 1, then `|PreDeblock − (Prediction + Residual)| == 0` for every sample, and the `Reconstruction` plane MD5 equals the conformance MD5 for that frame.

**FR-BLK-015** (Phase 2) — `Block.bits_x8` (and the per-cell `CellGrid.bits_x8` distribution) MUST sum, over all top-level blocks of a slice/tile, to within 2 % of the exact slice/tile data size in bits × 8 (the exact size being available only at slice/tile granularity, see Glossary "bits per block (estimated)").
- Acceptance: Given `corpus/syn_{h264,h265,av1}_gop60_1080p`, when `test:BitsAccounting` runs over frames 0–7, then the per-partition deviation is ≤ 2 % (H.264 phase 1 build exempt until FR-BLK-010 is in place).

**FR-BLK-016** (Phase 2) — `FrameAnalysis.qp_hist` MUST contain 256 bins where bin `q` counts the cells with `CellGrid.qp == q` (AV1 qindex 0–255; H.26x QP offset by +128 so negative QPs for high bit depth fit), and `FrameAnalysis.filter_params` MUST contain the codec's frame-level filter parameters (`HevcFilterParams`: deblocking offsets, `pps_loop_filter_across_*`, SAO enable; `Av1FilterParams`: `loop_filter_level[4]`, `cdef_damping`, `cdef_y/uv_strengths`, `lr_type`, `lr_unit_shift`; `AvcFilterParams`: `disable_deblocking_filter_idc`, `slice_alpha_c0_offset_div2`, `slice_beta_offset_div2`).
- Acceptance: Given `corpus/syn_{h264,h265,av1}_gop60_1080p`, when `test:QpHistAndFilters` runs on frame 0, then `Σ qp_hist == cells_w * cells_h` and the decoded `filter_params` table equals the frame header values parsed by the syntax layer.

**FR-BLK-017** (Phase 2) — `ovt-studio` MUST render these overlays from `FrameAnalysis` on the picture region, each toggleable independently and selectable as an F-key mode (chapter 3 UX-2): partition boundaries by level (distinct line widths for CTU/SB, CU/block, PU, TU/TXB), slice/tile boundaries, prediction kind fill (intra/inter/skip/intrabc/palette), intra direction glyphs, MV arrows (L0 and L1 in distinct colours, scaled by zoom with a user gain 0.25–8×), QP/qindex heat map, bits heat map, reference-index map, transform type/size, deblocking boundary strength, SAO/CDEF index, and block labels (`I`/`P`/`B`/`S`, mode names) when zoom ≥ 4×.
- Acceptance: Given `corpus/syn_h265_gop60_1080p` opened in `ovt-studio` at frame 2, when the tester presses F1, F2, F3, F5, F6, F9, F10, F12 in turn, then each overlay appears within 16 ms and the screenshot matches `tests/golden/overlays/h265_f<N>.png` with ≤ 0.5 % differing pixels (`ui-test:OverlayGolden`).

**FR-BLK-018** (Phase 2) — Overlay redraw of a 4K frame with MV arrows for every 4×4 cell (≈ 518 400 arrows) MUST complete in ≤ 16 ms on the M1 reference, and panning/zooming MUST not trigger a rebuild of `OverlaySet` GPU buffers.
- Acceptance: Given `corpus/syn_av1_4k_2min` in `ovt-studio` with F2 active, when `--measure-render 300` runs while scripted panning, then the 99th-percentile frame time is ≤ 16 ms and `overlay_rebuilds == 1`.

**FR-BLK-019** (Phase 2) — Clicking a block MUST select it and populate the Selection Info panel with: block kind, position (x, y) and size in luma samples, cell index, `partition_id`, `pred`, `qp`, `bits_x8 / 8` formatted as bits, all `MotionVector`s of the block (list, ref name, mv in 1/16 units and in luma samples, mvd, predictor), and every field of the codec extension table in spec order with `SyntaxNode.name`-style labels.
- Acceptance: Given `corpus/syn_av1_gop60_1080p` at frame 2 in `ovt-studio`, when the tester clicks at (200, 120), then Selection Info shows the block whose `rect` contains (200, 120) and its `y_mode`, `ref_frame[0]`, `interp_filter[0]`, `tx_size` values equal those in `ovt analyze --frames 2 --format json` (`ui-test:SelectionInfoAv1`).

**FR-BLK-020** (Phase 2) — The Statistics panel MUST compute, for the current frame and for a selectable frame range, histograms of block size by kind, prediction kind share, intra mode (luma, chroma), inter reference usage, MV magnitude (log2 bins) and direction (16 sectors), transform size and type, QP, and bits per prediction kind, each displayable as raw count, area-normalised (% of luma samples) and bits-normalised (% of frame bits), and exportable as CSV and JSON.
- Acceptance: Given `corpus/syn_h265_gop60_1080p`, when `ovt stats --frames 0-15 --format json` runs and the Statistics panel exports CSV for the same range, then the two agree on every histogram bin, and the `area_pct` column of each histogram sums to 100 ± 0.01.

**FR-BLK-021** (Phase 2) — OVT MUST produce an **encoding toolset summary** per stream listing which coding tools were used at least once (for example HEVC: AMP, SAO, transform skip, PCM, weighted prediction, TMVP, sign data hiding, scaling lists, tiles, WPP, long-term refs; AV1: superres, intrabc, palette, CfL, filter intra, OBMC, warped motion, global motion, compound modes, switchable interp, CDEF, loop restoration, film grain, delta-q, delta-lf, segmentation, reference scaling, screen content tools; H.264: CABAC/CAVLC, 8×8 transform, MBAFF/PAFF, weighted prediction, multiple slices, FMO/ASO, B-direct spatial/temporal) with the count of frames and blocks that used each.
- Acceptance: Given `corpus/syn_av1_tools_all`, when `ovt stats --toolset --format json` runs over all frames, then `toolset.palette.blocks > 0`, `toolset.obmc.blocks > 0` and `toolset.superres.frames > 0`, matching `tests/corpora/toolset_expected.json`.

**FR-BLK-022** (Phase 2) — `ovt analyze` MUST export `FrameAnalysis` as FlatBuffers, JSON and YUView statistics CSV (`%;syntax-version;v1.2` header; one item type per overlay: partition, pred, qp, bits, mv L0/L1, ref idx), and YUView MUST load the CSV against the same stream without errors.
- Acceptance: Given `corpus/syn_h265_gop60_1080p`, when `ovt analyze --frames 0-7 --format yuview -o stats.csv` runs and `tools/yuview_csv_lint.py stats.csv` runs, then the linter reports 0 errors and the per-cell `qp` values re-read from CSV equal the FlatBuffer values.

**FR-BLK-023** (Phase 2) — The Details view for a TU/TXB MUST show the three coefficient stages — quantised levels as parsed (`residual_coding`/`coeffs`), dequantised coefficients after scaling (H.265 §8.6.3; AV1 §7.12.3) and residual samples after the inverse transform — as three aligned grids in dec or hex, and the Details view for a deblocking edge MUST show `bS`, `β`, `tC` (H.265 §8.7.2.5.3), the filter decision (strong/normal/none) and the sample values before and after filtering.
- Acceptance: Given `corpus/jctvc_DBLK_A_SONY_4` at frame 0 in `ovt-studio`, when the tester right-clicks the TU at (64, 0) in F4 (Transform/residual) mode, then the three grids match `tests/golden/details/dblk_a_tu_64_0.json` exactly; when the tester right-clicks the vertical edge at x = 64 in F8 (Loop filter) mode, then `bS == 2` and the shown `β`, `tC` match the HM trace.

**FR-BLK-024** (Phase 2) — OVT MUST export the 4×4 motion field of a frame as CSV with columns `x4,y4,list,ref,mv_x,mv_y,mvd_x,mvd_y,predictor` (MVs in 1/16 units) via `ovt analyze --format mv-csv`.
- Acceptance: Given `corpus/syn_h265_gop60_1080p`, when `ovt analyze --frames 2 --format mv-csv -o mv.csv` runs, then `mv.csv` has one row per inter cell per list and the row count equals `count(mv0 valid) + count(mv1 valid)` from the FlatBuffer.

---

## 4.6 PLN — Planes and pixel views

**Rationale.** Owner capability (4): inspection of separate Y/U/V planes. chapter 3 row 24 and commitment B2. Appendix B.4 records the plane view as shader modes over uploaded native-depth planes; these requirements make the views, the sample read-outs and the magnification exact.

**FR-PLN-001** (Phase 1) — The picture region MUST offer plane modes `YUV` (colour-converted), `Y`, `U`, `V` and `Grey` (Y with the content transfer applied), selected by keys `1`–`4` and `5`, where `U`/`V` modes show the chroma plane upsampled to luma resolution with nearest-neighbour sampling and a neutral mid-grey mapping of the achromatic value (`128 << (bit_depth − 8)`).
- Acceptance: Given `corpus/syn_h265_main10_pq_hdr` in `ovt-studio`, when the tester presses `3`, then the frame grab's pixel at (0, 0) equals `U[0][0]` mapped to grey and the status bar reads `Plane: U` (`ui-test:PlaneModes`).

**FR-PLN-002** (Phase 1) — Plane views MUST operate on the decoded samples at native bit depth (8, 10 or 12) uploaded as `R8`/`R16` textures (ADR-005), MUST NOT round-trip through an 8-bit RGB conversion, and the GUI MUST show the sample value under the cursor in the status bar as `Y=<v> U=<v> V=<v>` (decimal or hex per the `H`/`D` toggle) with the 10/12-bit value, not a scaled 8-bit value.
- Acceptance: Given `corpus/syn_h265_main10_pq_hdr`, when the tester hovers (100, 100) and `ovt dump --frames 1 --pixel 100,100 --format json` runs, then the status bar values equal the CLI's `y`, `u`, `v` (values up to 1023).

**FR-PLN-003** (Phase 1) — When zoom ≥ 24 screen pixels per luma sample, the picture region MUST draw each sample's numeric value inside its square for the current plane (luma for `YUV`/`Y`/`Grey`; chroma at chroma resolution for `U`/`V`), in dec or hex per the `H`/`D` toggle, and MUST use nearest-neighbour magnification (no interpolation) at any zoom > 1×.
- Acceptance: Given `corpus/syn_h264_gop60_1080p` in `ovt-studio`, when the tester zooms to 32× on (8, 8), then the screenshot shows the digit string for `Y[8][8]` inside the corresponding square and no intermediate colour values exist between adjacent sample squares (`ui-test:SampleValuesAtZoom`).

**FR-PLN-004** (Phase 1) — The pixel inspector MUST allow pinning a sample position (`Ctrl+click`, or long-press on touch; chapter 11 §11.3.2) so that the position is kept while panning and across frame changes, and MUST show, for the locked position, the current frame's Y/U/V values and the difference to the previous frame.
- Acceptance: Given `corpus/syn_h264_gop60_1080p`, when the tester `Ctrl+click`s at (50, 50) and steps 3 frames with `→`, then the inspector still shows position (50, 50) and `ΔY` equals `Y_t[50][50] − Y_{t−1}[50][50]` per `ovt dump --pixel`.

**FR-PLN-005** (Phase 2) — The picture region MUST display any `PlaneStage` present in the current `FrameAnalysis` (`Reconstruction`, `Prediction`, `Residual`, `PreDeblock`, `PreSaoOrCdef`, `PreLoopRestoration`) with the same plane modes, where `Residual` is shown with a signed-to-unsigned offset of `1 << (bit_depth − 1)` and a user gain of 1–16×.
- Acceptance: Given `corpus/syn_h265_gop60_1080p` at frame 1, when the tester selects stage `Residual` and plane `Y` with gain 4, then the pixel (0, 0) grey equals `clamp(512 + 4 * residual_Y[0][0], 0, 1023)` for `bit_depth = 10` (`ui-test:ResidualStageView`).

**FR-PLN-006** (Phase 1) — The picture region MUST provide a per-plane histogram (256 or 1024/4096 bins per bit depth) of the visible region of the current plane, updated within 100 ms after a pan/zoom or frame change, with limited-range markers at `16 << (bd−8)` and `235 << (bd−8)` (luma) / `240 << (bd−8)` (chroma).
- Acceptance: Given `corpus/syn_h265_main10_pq_hdr`, when the Histogram panel is open and `ovt dump --frames 1 --histogram y --format json` runs, then the 1024 bin counts are equal for the full frame at fit zoom.

**FR-PLN-007** (Phase 2) — The picture region MUST support the compare modes of chapter 10 §10.2.4 between surface slots A and B (`VideoSurface::set_compare`): `Single`, `SideBySide`, `Split` (`H`/`V`, draggable divider), `Overlay`, `Difference` (|A−B| with gain 1–64× via `[`/`]`, optional per-plane selection and false-colour mapping) and `Flicker`; `C`/`Shift+C` cycle the mode and `X` swaps A and B (chapter 11 §11.4.3).
- Acceptance: Given `corpus/syn_metrics_1080p_pair` (reference) as A and `corpus/syn_metrics_1080p_dist` as B, when the tester selects `Difference` with gain 8 on frame 10, then the grab's pixel (x, y) equals `clamp(8 * |A_Y − B_Y|, 0, 255)` for ten sampled positions compared with `ovt compare --diff-image --frames 10 --gain 8`.

**FR-PLN-008** (Phase 1 dialog / Phase 3 live preview) — Raw YUV / Y4M files MUST be loadable as either slot with a format dialog that shows a live preview of the first frame at the chosen parameters and updates the preview within 200 ms of any parameter change (chapter 3 §3.6 item 5).
- Acceptance: Given `corpus/syn_raw_1080p_p010` opened via File → Open Raw…, when the tester changes packing from `I420` to `P010`, then the preview re-renders in ≤ 200 ms and shows a recognisable picture matching `tests/golden/raw_dialog_p010.png` within 1 % pixels.

**FR-PLN-009** (Phase 3) — The **first-mismatch finder** MUST compare slots A and B frame by frame (after alignment) and report the first frame, plane, and (x, y) at which samples differ, with the two values, and MUST run as a cancellable job on a worker thread at ≥ 300 fps for 1080p 8-bit on the M1 reference.
- Acceptance: Given `corpus/syn_metrics_1080p_{pair,bitflip_f37}`, when `ovt compare --first-diff A B --format json` runs, then the output is `{frame: 37, plane: 0, x: 611, y: 203, a, b}` with `a` and `b` equal to the values recorded in `tests/corpora/compare_expected.json` (chapter 14 recipe 52; `b == a + 1`) and the run time for 300 frames is ≤ 1 s.

**FR-PLN-010** (Phase 1) — Frame grabs MUST be exportable as PNG (8-bit, display-referred as rendered) and as 16-bit PNG or raw plane dumps (`.yuv`, planar, native bit depth, little-endian) of the current frame and plane stage, and `ovt dump --frames N --output frame.yuv` MUST produce byte-identical plane dumps.
- Acceptance: Given `corpus/syn_h265_main10_pq_hdr`, when the tester exports frame 0 as raw planes and `ovt dump --frames 0 --raw -o cli.yuv` runs, then `cmp gui.yuv cli.yuv` exits 0.

---

**FR-PLN-101** (Phase 1) — Rendering MUST be deterministic: the same inputs and `ViewState` MUST produce byte-identical offscreen output on the same GPU and driver, so peak detection and any frame-history-dependent tone mapping MUST be off for offscreen, export and test renders (chapter 10 §10.3.4 rule R-TM-1; replaces the retired `NFR-41x` ids of earlier chapter-10 drafts).
- Acceptance: Given any fixture, when it is rendered twice to an offscreen RGBA16F target, then the two downloads are byte-identical (`test:RenderDeterminism`, chapter 10 §10.9.2 item 5).

**FR-PLN-102** (Phase 4 — open and **deferred**: no work item is scheduled in phases 0–4; owner decision in chapter 17 §17.4 OI-08) — Interlaced H.264 content (field pictures, PAFF, MBAFF) MUST be displayable as weave, top field, bottom field or line-doubled single field (`ViewState.field_mode`, chapter 10 §10.2.3) with the field parity shown in the status bar.
- Acceptance: Given `corpus/jvt_CVPA1_TOSHIBA_B`, when the tester selects "Top field", then the grab contains only the even source lines and the status bar reads `Field: top`.

## 4.7 HDR — HDR and colour

**Rationale.** The owner's one-sentence goal requires "correct HDR (PQ and HLG) display"; commitment B8 and matrix rows 33–35 (chapter 3). ADR-005 fixes libplacebo for the colour pipeline, ADR-014 the Android output path, and chapter 0 §0.5.6 the `ovt_ColorDescription` shared by decode, metrics and render. Appendix B.4 supplies the platform facts used in the acceptance lines.

**FR-HDR-001** (Phase 1) — OVT MUST derive one `ovt_ColorDescription` per stream by precedence: (1) bitstream VUI/`color_config` (`colour_primaries`, `transfer_characteristics`, `matrix_coeffs`, `video_full_range_flag`, `chroma_sample_loc_type_*` per H.264 §E.2.1, H.265 §E.3.1, AV1 §6.4.2), (2) container `colr` (`nclx`) when the bitstream signals unspecified (code 2), (3) a user override; and MUST attach static metadata from MDCV/CLL SEI or AV1 metadata OBUs, falling back to `mdcv`/`clli` boxes.
- Acceptance: Given `corpus/syn_h265_pq_sei_only`, `corpus/syn_h265_pq_colr_only` and `corpus/syn_av1_hlg_metadata`, when `ovt probe --color --format json` runs, then `transfer` is 16, 16, 18 respectively and `max_cll`/`mdcv_max_luminance` equal `tests/corpora/hdr_expected.json`.

**FR-HDR-002** (Phase 1) — The HDR panel MUST display: primaries, transfer, matrix, range, chroma siting (names and H.273 codes), MDCV primaries and white point as CIE xy (converted from 0.00002 units), MDCV min/max luminance in cd/m², MaxCLL/MaxFALL, presence and version of HDR10+ (ST 2094-40 `application_version`, number of windows), presence of Dolby Vision RPU (`dvcC`/`dvvC` profile and level, or NAL type 62 for HEVC) with the note "RPU displayed, DV reshaping to PQ via libplacebo, no licensed DV display mapping", and per-frame HDR10+ values when present.
- Acceptance: Given `corpus/hdr_hdr10plus_dv81`, when the HDR panel is opened at frame 5, then it shows `HDR10+ v1, windows=1, targeted_system_display_maximum_luminance=…` equal to `ovt syntax --frames 5 --sei t35 --format json`; given `corpus/hdr_hdr10plus_dv81`, then `dv_profile == 8` is shown with the note.

**FR-HDR-003** (Phase 1) — On an HDR-capable display, `VideoSurface` MUST output PQ content without tone mapping when the display headroom allows (content MaxCLL ≤ display peak) and MUST output HLG via the BT.2100 OOTF with system gamma `1.2 + 0.42 · log10(L_W / 1000)` for the display peak `L_W`, with `ToneMap::Auto` selecting BT.2390 when the display peak is below content peak.
- Acceptance: Given `corpus/syn_h265_main10_pq_hdr` (a luminance ramp frame with known code values) on a Mac with a Pro Display XDR or MacBook Pro XDR in "HDR Video (P3-ST 2084)" preset, when the frame is displayed and measured with a colorimeter at the 100, 203, 500 and 1000 nit patches, then the measured luminance *ratios* between patches match the PQ targets within ±10 % (for example 1000-nit / 203-nit ≥ 4.4 when the reported headroom is ≥ 5; macOS EDR scales absolute output with the display brightness slider, so absolute nits are not asserted) (manual test `hw-test:PqRampXdr`); given the same stream on an SDR display, then the 203-nit patch renders at the SDR white level and no patch clips at 1000 nits (`ui-test:PqToneMapSdr` compares against `tests/golden/hdr/pq_ramp_sdr_bt2390.png`).

**FR-HDR-004** (Phase 1) — `VideoSurface` MUST offer the tone-mapping functions `Auto`, `BT2390`, `BT2446A`, `Spline`, `Clip` and `None` (`ovt::render::ToneMap`), the gamut-mapping modes `perceptual`, `relative`, `saturation`, `clip`, and MUST expose the selected function and the display peak in the status bar.
- Acceptance: Given `corpus/syn_h265_pq_4000nit` on an SDR display, when the tester switches tone mapping from `BT2390` to `Clip` with `clip_nits = 1000`, then all samples above 1000 nits render as white (`R=G=B=255`) in the grab and the status bar reads `TM: Clip 1000 nit`.

**FR-HDR-005** (Phase 1) — The user MUST be able to override matrix (BT.601/BT.709/BT.2020-NCL), range (limited/full), primaries and transfer for display and the override MUST apply to the picture region, the plane views and the measurement functions without re-decoding.
- Acceptance: Given `corpus/syn_h265_vui_unspecified` (VUI absent, content BT.709 limited), when the tester sets range to `Full`, then the grab's black level at (0, 0) changes from `16/255`-equivalent to `0` and `ovt dump --range full --png` produces the same image.

**FR-HDR-006** (Phase 3 — CPU measurement path of `ovt hdr --measure`, WI-3.532; the GPU `measure()` texture and the live HDR-panel read-out are phase 4, WI-4.452; re-phased, see chapter 17 §17.4 OI-15) — `VideoSurface::measure()` MUST compute per frame in linear light: MaxCLL (max over samples of max(R,G,B) in cd/m²), FALL (mean of max(R,G,B)), the 99.99th percentile luminance, and MUST accumulate MaxFALL over the frames viewed, using the PQ EOTF (ST 2084) or HLG inverse OETF + OOTF, with the result shown in the HDR panel and exported by `ovt hdr --measure` (chapter 12 §12.2.11).
- Acceptance: Given `corpus/syn_h265_main10_pq_hdr`, when `ovt hdr --measure --frames 0 -f json` runs, then `max_cll_measured` is 1000 ± 1 and `measures[0].fall_nits` equals the analytically computed ramp mean ± 1 cd/m².

**FR-HDR-007** (Phase 1) — When the display is SDR, OVT MUST still accept HDR input and MUST label the picture region with a persistent, non-modal badge `Tone-mapped (BT.2390, 203 → 100 nit)` naming the function and the mapping so that users never mistake tone-mapped output for true HDR.
- Acceptance: Given `corpus/syn_h265_main10_pq_hdr` on an SDR display, when the file opens, then the badge is visible in the grab (`ui-test:ToneMapBadge`); on an XDR display with headroom ≥ content peak, then no badge is shown.

**FR-HDR-008** (Phase 1) — Dithering to the output bit depth MUST be applied in `VideoSurface` (blue-noise, libplacebo `pl_dither_params`) for every output, and the output format MUST be 10-bit or FP16 on HDR surfaces (macOS: `MTLPixelFormatRGBA16Float` with `extendedLinearSRGB` or `bgr10a2Unorm` with ITU-R 2100 PQ; Android: `RGBA_1010102` / `RGBA_FP16` with `ADATASPACE_BT2020_PQ`/`BT2020_HLG`).
- Acceptance: Given the running `ovt-studio` on macOS with an HDR display, when `ovt-studio --dump-swapchain` runs, then the log reports `pixelFormat=RGBA16Float colorspace=kCGColorSpaceExtendedLinearSRGB wantsEDR=1 headroom=<value ≥ 1.0>`.

**FR-HDR-009** (Phase 3) — The metrics normalisation stage MUST support working domains `CodeValue`, `LinearLight`, `PU21`, `ICtCp` and `ModelRGB` (chapter 0 §0.5.7) and MUST convert via zimg with 32-bit float intermediates, honouring `ovt_ColorDescription` of the reference as the target, with display-model presets `pq_1000`, `pq_4000`, `pq_10000`, `hlg_1000`, `bt1886_100` (chapter 9 §9.4) for `LinearLight`/`PU21`.
- Acceptance: Given `corpus/syn_metrics_pq_1080p_pair` (PQ 10-bit), when `ovt metrics --metric psnr --domain pu21 --display-model pq_1000 -f json` runs, then the per-frame PU21-PSNR equals the values in `tests/corpora/pu21_expected.json` (computed with the reference MATLAB/Python PU21 implementation) within 0.01 dB.

**FR-HDR-010** (Phase 4) — The picture region MUST offer HDR scopes as toggleable panels rendered from the linear-light texture: false colour with the bands `<1`, `1–100`, `100–203`, `203–1000`, `1000–4000`, `>4000` cd/m² and a legend; luminance waveform with a logarithmic nit axis from 0.0001 to 10 000; vectorscope in BT.2020 with BT.709 and P3 gamut triangles; gamut-clip overlay marking samples outside BT.709 (or P3, selectable); histogram in PQ code values.
- Acceptance: Given `corpus/syn_h265_main10_pq_hdr`, when false colour is enabled, then the 500-nit patch is rendered in the `203–1000` band colour from the legend and the waveform shows a monotonic ramp reaching the 1000-nit grid line (`ui-test:HdrScopes` golden images).

**FR-HDR-011** (Phase 1) — Static HDR metadata (ST 2086 primaries/white/luminance, CTA-861.3 MaxCLL/MaxFALL) MUST be passed to the display surface when it is HDR (macOS `CAEDRMetadata.hdr10`; Android `ANativeWindow` SMPTE2086/CTA861.3 metadata via `AHardwareBuffer`/EGL `EGL_EXT_surface_SMPTE2086_metadata`; Linux Wayland `image_description` in phase 4), and MUST be re-sent when the stream changes.
- Acceptance: Given `corpus/syn_h265_main10_pq_hdr` on macOS, when `ovt-studio --dump-swapchain` runs, then the log shows `edrMetadata: hdr10 min=0.0001 max=1000 opticalOutputScale=…`.

**FR-HDR-012** (Phase 4 — WI-4.455; re-phased, chapter 17 §17.4 OI-15; the *presence* of HDR10+ metadata is shown from phase 1 by FR-HDR-002) — HDR10+ (ST 2094-40) per-frame metadata, when present, MUST be passed to libplacebo as `pl_hdr_metadata.scene_max/scene_avg/ootf` so that tone mapping follows the signalled per-scene values, and the HDR panel MUST indicate `dynamic metadata in use`.
- Acceptance: Given `corpus/hdr_hdr10plus_dv81` on an SDR display, when frames 5 and 60 (different scene_max) are grabbed, then the tone-mapping curve differs (a 500-nit patch present in both frames renders at different grey levels) and the panel shows `dynamic metadata in use`.

**FR-HDR-013** (Phase 4 — WI-4.456; re-phased, chapter 17 §17.4 OI-15; RPU *presence* is shown from phase 1 by FR-HDR-002, and the base layer renders from phase 1) — Dolby Vision profile 5 and 8 RPUs MUST be parsed by the playback path (FFmpeg `AV_FRAME_DATA_DOVI_METADATA`) and applied by libplacebo to produce PQ output, and when the RPU cannot be applied the stream MUST still render as the base layer with diagnostic `DEC-HDR-0002` (Info; registry chapter 7 §7.9.4).
- Acceptance: Given `corpus/hdr_dovi_p5`, when the file opens, then the picture is not magenta/green-tinted (grab mean chroma within ±8 code values of `tests/golden/hdr/dovi_p5_f0.png`) and the HDR panel shows `DV profile 5 → PQ (libplacebo)`.

**FR-HDR-014** (Phase 2) — Overlays (grids, arrows, labels, heat maps) MUST be composited at the SDR reference white (203 cd/m² on PQ/HLG surfaces, `sdr_white_nits` from `DisplayTarget`) so that overlay colours do not change brightness when the display switches between SDR and HDR modes.
- Acceptance: Given `corpus/syn_h265_main10_pq_hdr` with the F1 (Coding flow) overlay enabled on an XDR display, when measured with a colorimeter on a white grid line, then luminance is 203 ± 20 cd/m² (`hw-test:OverlayWhite`).

---

**FR-HDR-101** (Phase 1) — All UI chrome MUST stay SDR; only the video surface is HDR (chapter 10 §10.1.2 rules R-ARCH-1 and R-ARCH-2).
- Acceptance: Given HDR mode on, when a screenshot of the window is taken, then no pixel outside the picture panel exceeds SDR white (`ui-test:ChromeStaysSdr`).

## 4.8 VQ — Quality metrics

**Rationale.** Owner capability (2) and the starter metrics PSNR, SSIM, VMAF, UVQ with a mandatory plugin layer; persona commitments B1, B3–B7 and the should-haves (chapter 3 §3.4.2). ADR-011 fixes the engines (own kernels, libvmaf, ONNX Runtime for UVQ, sidecar protocol) and the shared normalisation stage. Appendix B.3 lists the pipeline pitfalls (alignment, range, bit depth, frame-rate mismatch) that each become a requirement here so that scores are reproducible against the `vmaf` CLI.

**FR-VQ-001** (Phase 3) — Every metric MUST be implemented behind `ovt_metric_v1` (`core/include/ovt/abi/metric_v1.h`) with a `MetricDescriptor` declaring `id`, `kind`, `domain`, `per_plane`, `higher_is_better`, `range_min/max`, `temporal_unit_frames`, `outputs`, `license` and `version`, and the core MUST discover metrics only through this ABI (built-in metrics included).
- Acceptance: Given a build, when `ovt plugins --metrics --verbose --format json` runs, then it lists at least `psnr`, `ssim`, `ssim_x264`, `ms_ssim`, `vmaf`, `vmaf_neg`, `vmaf_4k`, `vmaf_phone`, `psnr_hvs`, `ciede2000`, `cambi`, `uvq`, each with all descriptor fields non-empty.

**FR-VQ-002** (Phase 3) — The normalisation stage MUST, before any `score()` call, (a) align frames temporally by index after an offset search, (b) crop both inputs by container crop and by user crop rectangles, (c) scale the distorted frame to the reference resolution with a declared kernel ∈ {`bicubic` (default), `lanczos`, `bilinear`, `spline36`} (chapter 9 §9.3.2), (d) convert matrix/primaries/transfer/range/chroma siting to the reference's `ovt_ColorDescription` via zimg, (e) promote bit depth to `max(ref, dist)` by left shift, and MUST record every step in `MetricsResult.normalization` as JSON.
- Acceptance: Given `corpus/syn_metrics_scale_range_pair`, when `ovt metrics --metric psnr --format json` runs, then `normalization` contains `{"scale":{"kernel":"bicubic","from":[1280,720],"to":[1920,1080]},"range":{"from":"full","to":"limited"},"bit_depth":{"from":[8,8],"to":8}}` and the Y-PSNR equals `ffmpeg -i dist -i ref -lavfi "[0:v]scale=1920:1080:flags=bicubic,scale=in_range=full:out_range=limited[d];[d][1:v]psnr" -f null -` within 0.02 dB.

**FR-VQ-003** (Phase 3) — Temporal alignment MUST search offsets in the range ±`K` frames (`K` default 30, user-settable) by maximising the mean normalised cross-correlation `S(o)` of downscaled luma signatures over the first `N` frames exactly as chapter 9 §9.3.1 specifies, MUST report `alignment_offset_frames` (sign convention D9: distorted frame *i* corresponds to reference frame *i + o*, so a distorted stream that drops 7 leading frames yields `+7`) and `alignment_confidence` (= `S(o*) − S(o₂)`, the margin between the best and second-best offset, range 0–2; below `min_confidence` = 0.05 the result is flagged `low_confidence` with `VQ-ALIGN-0001`), MUST allow a manual override (`--offset`), MUST emit `VQ-ALIGN-0002` (Warning) when a non-zero offset was applied and `VQ-ALIGN-0003` (Warning) when the two inputs have different frame counts after alignment (chapter 9 §9.3.1).
- Acceptance: Given `corpus/syn_align_drop7` (chapter 14 §14.3.3; the distorted stream drops 7 leading frames), when `ovt metrics --metric psnr --format json` runs, then `alignment_offset_frames == +7`, `alignment_confidence ≥ 0.5` (no `low_confidence` flag, no `VQ-ALIGN-0001`), and `VQ-ALIGN-0003` (frame counts differ by the applied offset; chapter 9 §9.11.4) is present.

**FR-VQ-004** (Phase 3) — When frame rates differ, the default policy MUST be to pair only frames whose timestamps match within ε = half the shorter frame duration, MUST report `frames_skipped`, and MUST NOT duplicate or interpolate frames; the user MAY select `--fps-policy refuse|nearest`.
- Acceptance: Given `corpus/syn_metrics_fps_mismatch_pair`, when `ovt metrics --metric psnr --format json` runs, then `frames.length == 300` for a 10 s clip and `frames_skipped == 300`.

**FR-VQ-005** (Phase 3) — PSNR MUST be computed per plane as `10·log10(peak² / MSE)` with `peak = 2^bit_depth − 1` of the working depth, MUST report Y, U, V separately, MUST offer the named combinations `jvet_weighted` ((6·Y + U + V)/8 on PSNR), `ffmpeg_avg` (MSE weighted by sample count), and `y_only`, and MUST cap infinite values at `psnr_max` = 100 dB with a `lossless` flag.
- Acceptance: Given `corpus/syn_metrics_1080p_pair`, when `ovt metrics --metric psnr --format json` runs, then per-frame Y/U/V equal `ffmpeg … psnr -f null -` `psnr_y/u/v` within 0.001 dB and `ffmpeg_avg` equals `psnr_avg` within 0.001 dB (`test:PsnrOracle`).

**FR-VQ-006** (Phase 3) — SSIM MUST be offered in two variants: `ssim` (Wang et al. 11×11 Gaussian window σ = 1.5, C1 = (0.01·L)², C2 = (0.03·L)², L = 2^bd − 1) and `ssim_x264` (8×8 blocks of 4×4 sums as in x264/libaom `ssim.c`), each per plane with a per-pixel/per-block map output.
- Acceptance: Given `corpus/syn_metrics_1080p_pair`, when `ovt metrics --metric ssim,ssim_x264 --format json` runs, then `ssim` Y equals libvmaf `float_ssim` within 0.0005 and `ssim_x264` Y equals `ffmpeg … ssim` within 0.0005 (`test:SsimOracle`).

**FR-VQ-007** (Phase 3) — VMAF MUST be computed by calling libvmaf (≥ 3.0, fixed-point build) directly on the normalised planes with the bundled models addressed by the ids `vmaf` (= `vmaf_v0.6.1`), `vmaf_neg` (= `vmaf_v0.6.1neg`), `vmaf_4k` (= `vmaf_4k_v0.6.1`), `vmaf_b` (= `vmaf_b_v0.6.3`, bootstrap CI) and `vmaf_<name>` for any other model file shipped with libvmaf ≥ 3.0.0 (chapter 9 §9.6.2), and the per-frame and pooled scores MUST be bit-identical to the `vmaf` CLI on the Netflix test vectors.
- Acceptance: Given `corpus/netflix_{src01_hrc00_576x324,src01_hrc01_576x324}`, when `ovt metrics --metric vmaf --model vmaf --format json` runs and `vmaf --reference … --distorted … --model version=vmaf_v0.6.1 --format json` runs, then every `frames[i].values[0]` and `pooled_mean[0]` are equal to 6 decimal places (`test:VmafParity`).

**FR-VQ-008** (Phase 3) — The libvmaf host MUST also expose `ms_ssim`, `psnr_hvs`, `ciede2000`, `cambi` (with its heat-map output) and `vif` as metric ids through the same ABI, with results identical to the `vmaf --feature <name>` output.
- Acceptance: Given the Netflix vectors, when `ovt metrics --metric cambi,psnr_hvs --format json` runs, then values equal `vmaf --feature cambi --feature psnr_hvs --format json` to 6 decimal places and the CAMBI map blob has `map_block_size` and dimensions matching libvmaf's heatmap.

**FR-VQ-009** (Phase 3) — UVQ MUST be implemented as a `NoReference` metric running the google/uvq 1.5 (default) and 1.0 models exported to ONNX (ContentNet, CompressionNet, DistortionNet, AggregationNet) (TensorFlow SavedModels exported via `tf2onnx`, D11) through ONNX Runtime with the CoreML EP on macOS, the XNNPACK EP on Android (NNAPI is deprecated from API 35) and the CPU EP on Linux, with preprocessing (1 fps sampling default, resize to the model input geometry with bilinear interpolation, RGB float normalisation, 1-second chunks) reproducing the reference CLI, and the `compression_content_distortion` output as the MOS score.
- Acceptance: Given the three `corpus/ytugc_*` clips of the `fast` subset (chapter 14 §14.3.2), when `ovt metrics --metric uvq --format json` runs and `python uvq_inference.py <clip> --model_version 1.5 --fps 1` runs, then the two MOS values differ by ≤ 0.01 for each clip (`test:UvqParity`).

**FR-VQ-010** (Phase 3) — UVQ MUST also emit its per-chunk 4×4 compression-level map and 2×2 distortion-type labels as map outputs so that the GUI can overlay them, and model weights (≈ 100 MB) MUST be downloaded on first use into the user cache directory, never bundled in the Android APK.
- Acceptance: Given a fresh install, when the user selects `uvq` for the first time, then a download prompt names the size and licence (Apache-2.0) and after download `<cache_root>/models/uvq-1.5/*.onnx` (`~/Library/Caches/org.openvideotools.ovt/` on macOS, chapter 6 §6.7) exists; the APK size stays below 120 MB.

**FR-VQ-011** (Phase 3) — Every full-reference metric that can be computed locally (PSNR, SSIM variants, ΔE-ITP in phase 4) MUST emit a per-block map at a user-selectable block size `N` ∈ {4, 8, 16, 32, 64} (default: the codec's largest block size when known, else 16) as a `map16` blob, which the GUI MUST render as a heat-map overlay with a selectable colour map including `viridis` and `cividis` and a legend.
- Acceptance: Given `corpus/syn_metrics_1080p_pair`, when `ovt metrics --metric psnr --map 16 --frames 10 --format json --maps-dir out/` runs, then `frames[0].map_block_size == 16`, the map is written as `out/psnr/000010.pgm` (chapter 12 §12.2.9; the cache form is chapter 9 §9.9.4) and the map value at block (0, 0) equals the PSNR of that 16×16 block computed independently by `tests/vq/block_psnr.py` within 0.01 dB.

**FR-VQ-012** (Phase 3) — Pooled statistics `pooled_mean`, `pooled_harmonic`, `pooled_min`, `pooled_p1`, `pooled_p5`, `pooled_std` MUST be computed per plane/output for every metric, and per-scene means MUST be available when a scene list is supplied (`--scenes file.json`) or detected (phase 4).
- Acceptance: Given the Netflix vectors, when `ovt metrics --metric vmaf --pool all --format json` runs, then `pooled_harmonic[0]` equals `vmaf --pool harmonic_mean` output to 6 decimals and `pooled_p5` equals the 5th percentile of `frames[].values[0]` computed by `numpy.percentile(…, 5, method="linear")` within 1e-6.

**FR-VQ-013** (Phase 4 — open and **deferred**: no work item is scheduled in phases 0–4; owner decision in chapter 17 §17.4 OI-07) — ROI-restricted metrics MUST be supported by a list of rectangles (coded-picture coordinates) in a `rois` vector to be added to `NormalizationConfig` (chapter 6 §6.5) when this requirement is scheduled, where each metric is computed on the union of the rectangles, and results MUST state `roi_area_pct`.
- Acceptance: Given `corpus/syn_metrics_1080p_pair`, when `ovt metrics --metric psnr --roi 0,0,960,540 --format json` runs, then Y-PSNR equals the PSNR of the top-left quadrant computed by `tests/vq/roi_psnr.py` within 0.001 dB and `roi_area_pct == 25.0`.

**FR-VQ-014** (Phase 3) — Metric results MUST be exportable as CSV in the layout of chapter 9 §9.9.1 (`<stem>.csv` with one row per frame and columns `frame,display_index,<metric>_<plane>…`, plus `<stem>_summary.csv` with the pooled values; `.` decimal separator, `,` field separator, UTF-8, LF line endings, regardless of system locale), as OVT JSON (`--format json`), as libvmaf-compatible JSON (`--format vmaf-json`: `frames[].metrics.{vmaf,psnr_y,…}`, `pooled_metrics`), and as OVT `MetricsResult` FlatBuffers (`--format fb`).
- Acceptance: Given `LANG=de_DE.UTF-8`, when `ovt metrics --metric psnr,vmaf --format csv --output out` runs, then `out.csv` contains no `;` and no `,` inside numbers, and `python -c "import csv; …"` parses every numeric cell as a float; when `--format vmaf-json` is used, then `jq .pooled_metrics.vmaf.mean` succeeds.

**FR-VQ-015** (Phase 3) — PSNR + SSIM together MUST process ≥ 200 fps at 1080p 8-bit on 4 threads of the M1 reference, and VMAF (`vmaf_v0.6.1`) MUST process ≥ 60 fps at 1080p on 4 threads (budget values owned by chapter 14 §14.9.3 `bench_metrics_psnr_ssim_1080p` / `bench_metrics_vmaf_1080p` and `tests/perf/budgets.json`, D19).
- Acceptance: Given `corpus/syn_metrics_1080p_pair`, when `ovt metrics --metric psnr,ssim --threads 4 --time` and `ovt metrics --metric vmaf --threads 4 --time` run, then `timing.metrics_fps ≥ 200` and `timing.metrics_fps ≥ 60` respectively (`test:PerfMetrics`, 20 % tolerance).

**FR-VQ-016** (Phase 3) — The Metrics panel MUST plot per-frame scores for every selected metric and plane as line charts over the timeline, MUST overlay frame size from `StreamIndex` on a secondary axis, MUST show a cumulative mean line, and clicking a point MUST seek the picture region to that frame (UX-7).
- Acceptance: Given `corpus/syn_metrics_1080p_dist` with the `corpus/syn_metrics_1080p_pair` reference loaded and PSNR computed, when the tester clicks the plotted point at frame 120, then the current frame becomes display index 120 within 16 ms and the Selection/Status shows `PSNR-Y = <value>` equal to the CSV row for frame 120 (`ui-test:MetricsClickSeek`).

**FR-VQ-017** (Phase 3) — OVT MUST compute BD-rate and BD-PSNR (Bjøntegaard) between two rate–distortion sets of ≥ 4 points with interpolators `cubic` (default, piecewise cubic as in JCTVC-L1100 / VCEG-M33), `pchip` and `akima`, for any metric id and the default pooling `mean` (`--pool` selects another), and results MUST match the FAU-LMS `bjontegaard` reference implementation.
- Acceptance: Given `tests/vq/bd/{anchor,test}.csv` (rate, psnr_y columns), when `ovt bdrate --anchor anchor.csv --test test.csv --metric psnr_y --interp cubic,pchip,akima --format json` runs, then each `bd_rate_pct` equals `bjontegaard.bd_rate(..., method=…)` within 0.01 percentage points.

**FR-VQ-018** (Phase 3) — The GUI MUST support a **comparison project** of up to 16 distorted files against one reference (an encoding ladder of chapter 9 §9.8.4 commonly has 10–14 rungs), run metrics for all of them as one cancellable job set with a progress bar per file, and show RD points (bit rate from `StreamIndex.frames[].size_bytes` and container duration vs pooled metric) in an RD plot with BD-rate between any two selected files.
- Acceptance: Given `corpus/syn_metrics_ladder_{ref,q22,q27,q32,q37}`, when the tester adds the four encodes and runs PSNR, then the RD plot shows 4 points whose bit rates equal `ovt probe --format json | jq .tracks[0].bitrate_bps` and BD-rate between the two selected files equals `ovt bdrate` output.

**FR-VQ-019** (Phase 4) — OVT MUST implement the sidecar metric protocol: a child process started from a sidecar manifest (`<id>.ovt-sidecar.json` naming the executable and its `MetricDescriptor`; chapter 9 §9.7), speaking JSON Lines over stdin/stdout for control (`hello`, `configure`, `frame`, `finish`) and receiving frames through a shared-memory region (POSIX `shm_open`) or a Y4M pipe, running with a restricted environment (no inherited env except `PATH`, `HOME`, `TMPDIR`; working directory a temp dir; 2-minute inactivity timeout).
- Acceptance: Given the reference sidecar `tools/sidecar_examples/psnr_py/` (Python), when `ovt metrics --metric psnr_sidecar --format json` runs on `corpus/syn_metrics_1080p_pair`, then its scores equal the native `psnr` within 0.001 dB and `ps` shows the child's environment contains only the three allowed variables.

**FR-VQ-020** (Phase 4) — The following metrics MUST be added through the plugin ABI without core changes: `xpsnr` (port of the FFmpeg ≥ 7.1 filter, with its patent note in `license`), `de_itp` (ΔE-ITP per BT.2124 on `ICtCp` domain with map output), `pu21_psnr`/`pu21_ssim` (PU21 encoding on `LinearLight` with display model), `wpsnr` (JVET HDR CTC), `si_ti` (ITU-T P.910 2008 and 2023 definitions, `Bitstream`-free, no-reference).
- Acceptance: Given `corpus/syn_metrics_pq_1080p_pair`, when `ovt metrics --metric de_itp,pu21_psnr,wpsnr --format json` runs, then values equal the reference implementations listed in `tests/vq/hdr_reference.json` within 0.01; given `corpus/syn_metrics_1080p_pair` (reference), when `ovt metrics --metric si_ti --format json` runs, then `si`/`ti` equal `siti-tools` output within 0.01.

**FR-VQ-021** (Phase 4) — Scene-change detection MUST be exactly the FFmpeg `scdet` filter (D31; chapter 9 §9.8.2): SAD between consecutive frames summed over **all** planes at their own dimensions, `mafd = sad · 100 / (count · 2^bitdepth)` with `count` the total sample count of all planes, score = `clip(min(mafd_t, |mafd_t − mafd_{t−1}|), 0, 100)`, threshold default 10.0 on the 0–100 scale, no downscaling, so that `-vf scdet=threshold=10` is a bit-exact oracle, plus the libvmaf motion score when VMAF is computed, MUST produce a scene list JSON, and per-scene pooling MUST use it automatically when no `--scenes` file is given.
- Acceptance: Given `corpus/syn_scenes_5cuts`, when `ovt scenes <file> --format json` runs, then it lists exactly the 5 cut frames in `tests/corpora/scenes_expected.json` (±1 frame).

**FR-VQ-022** (Phase 3) — The per-block metric heat map MUST be alignable to the codec block grid of the current `FrameAnalysis` (per-CTU/SB/MB when `--map codec` is selected) so that quality can be read per coding block in the Selection Info panel.
- Acceptance: Given `corpus/syn_metrics_1080p_dist` (HEVC, CTU 64) with analysis at frame 10 and PSNR map `codec`, when the tester clicks the CTU at (128, 64), then Selection Info shows `PSNR-Y (block) = <value>` equal to the 64×64 block PSNR from `tests/vq/block_psnr.py --block 64 --x 128 --y 64`.

**FR-VQ-023** (Phase 3) — A **lossless check** metric `identity` MUST report, per frame, whether all planes are bit-identical and the number of differing samples per plane, at ≥ 500 fps for 1080p 8-bit on the M1 reference.
- Acceptance: Given `corpus/syn_metrics_1080p_{pair,bitflip_f37}`, when `ovt metrics --metric identity --format json` runs, then `frames[37].values == [1, 0, 0]` (1 differing luma sample) and all other frames report `[0, 0, 0]`.

**FR-VQ-024** (Phase 3) — The metric subsystem MUST process inputs in all combinations of {8, 10, 12}-bit and {4:2:0, 4:2:2, 4:4:4} where the reference and distorted may differ in bit depth, and MUST refuse with `VQ-NORM-0003` when chroma formats differ unless `--chroma-policy upsample444` is given.
- Acceptance: Given `corpus/syn_metrics_10bit_8bit_pair`, when `ovt metrics --metric psnr --format json` runs, then `normalization.bit_depth.to == 10` and Y-PSNR equals `ffmpeg … format=yuv420p10le … psnr` within 0.001 dB; given `corpus/syn_metrics_444_420_pair`, then exit code is 4 and the diagnostic is `VQ-NORM-0003`.

---

**FR-VQ-101** (Phase 4) — OVT MUST compute the convex hull of a set of rate–distortion points across resolutions and emit an encoding-ladder table (per-rung resolution, bit rate, pooled metric, on-hull flag) as JSON and CSV (`ovt bdrate --hull`, chapter 9 §9.8.4).
- Acceptance: Given `tests/vq/ladder/*.csv` (four resolutions, five rates each), when `ovt bdrate --hull --format json` runs, then `hull[]` lists exactly the points of `tests/vq/ladder/expected_hull.json`.

## 4.9 GUI — ovt-studio

**Rationale.** chapter 3 §3.5 (UX-1 … UX-12) states the UX conventions OVT adopts; this area turns each into a testable requirement and adds the panel inventory, keyboard map, layout persistence and large-data behaviour (Appendix B.4). ADR-004 fixes Qt 6 Quick; ADR-003 fixes that the GUI contains no analysis logic. UI acceptance tests use the Qt Quick Test harness in `app/tests/` with the `ui-test:` prefix and golden images in `tests/golden/`.

**FR-GUI-001** (Phase 1) — `ovt-studio` MUST implement the three-pane skeleton of UX-1: a timeline region (bottom), a picture region (centre) and a tabbed side region (right) with the panels Container, Syntax, Hex, Selection Info, Statistics, Metrics, HDR, Diagnostics, Reference Graph, Histogram; every panel MUST be dockable, floatable, closable and re-openable from the View menu.
- Acceptance: Given a fresh profile, when `ovt-studio` starts, then the three regions are visible with the default panel set, and when the tester drags the Syntax panel out and back, then it docks again (`ui-test:DockingRoundTrip`).

**FR-GUI-002** (Phase 1) — Panel layout, window geometry, open panels, last overlay mode, colour map and plane mode MUST persist across restarts in the per-user JSON settings file `settings.json` in `QStandardPaths::AppConfigLocation` (`~/.config/ovt/settings.json`, `~/Library/Application Support/ovt/settings.json`, the app files directory on Android; chapter 11 §11.2.5 is the single source, no `QSettings`), and a menu item View → Reset Layout MUST restore the default.
- Acceptance: Given the tester closes the Hex panel and quits, when `ovt-studio` restarts, then the Hex panel is closed; when View → Reset Layout is chosen, then it reappears in its default position.

**FR-GUI-003** (Phase 1) — The timeline MUST draw one bar per frame from `StreamIndex` (height = `size_bytes`, colour by `FrameType` per UX-6), MUST overlay `qp_base` as a line on a secondary axis, MUST support zoom (wheel) and pan (drag) over up to 1 000 000 frames with level-of-detail aggregation (max/mean per pixel column), MUST mark RAPs with a tick, and MUST draw within 16 ms per frame at any zoom level.
- Acceptance: Given `corpus/syn_h264_1h_1080p` (86 400 frames), when the tester zooms from full range to 100 frames and back while `--measure-render` runs, then the 99th-percentile frame time is ≤ 16 ms and the screenshot at full range matches `tests/golden/timeline_1h.png` within 1 %.

**FR-GUI-004** (Phase 1) — Clicking a bar MUST set the current frame; the current frame MUST be a single application-wide property (`AppState.currentFrame` holding a `FrameId`) and every panel MUST bind to it so that a change propagates to all panels within one render frame (UX-7).
- Acceptance: Given `corpus/syn_h265_gop60_1080p`, when the tester clicks bar 17, then the picture, Syntax (first VCL unit of frame 17), Hex, Selection Info and Filmstrip all show frame 17 in the next rendered frame (`ui-test:CursorSync` checks each panel's `currentFrame` binding).

**FR-GUI-005** (Phase 1) — Decode order and display order MUST be switchable globally by the `O` key and a toolbar toggle (UX-8); in display order the timeline MUST reorder bars by `display_index`, hidden frames (`display_index = −1`) MUST be shown as hatched bars attached to the frame that shows them, and tooltips MUST show `decode #<d> · display #<p> · POC/OrderHint <n> · <type> · <size> B · <pts s>`.
- Acceptance: Given `corpus/syn_av1_hidden_altref_showexisting`, when the tester presses `O`, then bar positions change to display order and the hidden ALTREF bar is hatched; the tooltip of the bar for display index 8 reads the values from `ovt index --format json` for that frame.

**FR-GUI-006** (Phase 1) — Frame search (`Ctrl+G`) MUST accept decode index (`d123`), display index (`123` or `p123`), POC/order hint (`poc45`), time (`12.5s`, `00:01:30.250`) and file byte offset (`@0x1A2B3C`, `@1715004`) and MUST navigate to the matching frame or report "no frame" without changing the current frame.
- Acceptance: Given `corpus/syn_h264_gop60_1080p`, when the tester enters `@<offset of frame 40's first unit>`, then the current frame becomes decode index 40; when `poc9999` is entered, then a non-modal message "no frame with POC 9999" appears and the current frame is unchanged.

**FR-GUI-007** (Phase 1) — The picture region MUST support wheel/pinch zoom about the cursor from 1/32× to 64×, primary-button drag pan, `R` fit-to-window, double-click of any mouse button fits, and MUST keep the zoom centre fixed under the cursor to within 1 pixel (UX-12).
- Acceptance: Given any stream, when the tester wheels 5 notches at screen point (400, 300), then the sample that was under (400, 300) before the first notch is still under (400, 300) ± 1 pixel after the fifth (`ui-test:ZoomAnchor`).

**FR-GUI-008** (Phase 2) — Overlay modes MUST be bound to F1–F12 exactly as chapter 11 §11.4.2 lists them (F1 Coding flow, F2 Prediction, F3 Motion vectors, F4 Transform/residual, F5 QP map, F6 Bits heatmap, F7 Reference indices, F8 Loop filter, F9 In-loop filter params, F10 Metrics heatmap, F11 Plain picture, F12 HDR false colour), the letter toggles `P`, `I`, `G`, `M`, `T`, `L`, `H`, `D`, `K` (colour-blind palette), `E`, `S`, `A` (analyse), `B` (bookmark), planes `1`–`5`, `Space`, `←`/`→`, `Shift+←`/`→` (±10), `Ctrl+←`/`→` (RAP), `Tab` (block level) and `Ctrl+↓`/`↑` (next/previous error) MUST follow the canonical keyboard map of chapter 11 §11.4.3 (which chapter 3 UX-2 cites), and every binding MUST be remappable in Settings → Keyboard with conflicts rejected.
- Acceptance: Given `ovt-studio`, when the tester presses F5, then the QP overlay is active and the mode label reads `F5 · QP map`; when the tester remaps F5 to `Q` in Settings, then `Q` activates it and F5 does nothing (`ui-test:KeyRemap`).

**FR-GUI-009** (Phase 2) — Repeated left-clicks at the same canvas position MUST cycle the selection through the block hierarchy largest → smallest and wrap (UX-4): H.264 MB → sub-MB partition; HEVC CTU → CU → PU → TU; AV1 SB → block → TXB; the Selection Info header MUST show the current level and the selection outline MUST be drawn on the picture.
- Acceptance: Given `corpus/syn_h265_gop60_1080p` at frame 2, when the tester clicks (100, 100) four times, then Selection Info headers read `CTU`, `CU`, `PU`, `TU` in order and a fifth click returns to `CTU` (`ui-test:ClickCycle`).

**FR-GUI-010** (Phase 2) — Right-click (or long-press ≥ 500 ms on touch) on a block MUST open the Details view for the current overlay mode (UX-3), and the Details view MUST be a dockable panel, not a modal dialog.
- Acceptance: Given `corpus/syn_av1_gop60_1080p` in F2 mode, when the tester right-clicks a block, then the Details panel opens showing the block's `Av1Block` fields, and the main window remains interactive (`ui-test:DetailsPanel`).

**FR-GUI-011** (Phase 2) — Selecting a block MUST select the corresponding node in the Syntax panel and highlight its bits in the Hex panel (UX-5); when the plugin provides no per-block syntax position (H.264 phase 1), the slice header of the owning partition MUST be selected instead.
- Acceptance: Given `corpus/syn_h265_gop60_1080p` at frame 2, when the tester clicks a CU, then the Syntax panel's selected node is the `coding_unit` of that CU (or its slice segment header when the plugin lacks CU spans) and the Hex panel highlights the slice data range (`ui-test:SelectionToSyntax`).

**FR-GUI-012** (Phase 1) — Double-clicking a VCL unit in the Syntax panel or a row in the Diagnostics panel MUST navigate to the owning frame, and when the diagnostic has a block position it MUST select that block (UX-5).
- Acceptance: Given `corpus/syn_broken_h265_slice_error_f12`, when the tester double-clicks the error row, then the current frame becomes decode index 12 and the Hex panel highlights the diagnostic's `span`.

**FR-GUI-013** (Phase 1) — The Syntax panel MUST present the `SyntaxNode` tree with columns Name, Value (dec/hex per `H`/`D`), Descriptor, Derived, and MUST be backed by a lazily-populated `QAbstractItemModel` so that a `StreamIndex` with 1 000 000 units opens without materialising more than the visible rows plus 200.
- Acceptance: Given `corpus/syn_h264_1h_1080p`, when the Syntax panel opens, then `AppState.syntaxModelRowsMaterialised ≤ visibleRows + 200` and scrolling to the end takes ≤ 1 s (`ui-test:SyntaxLazyModel`).

**FR-GUI-014** (Phase 1) — The Hex panel MUST show the selected unit's raw bytes 16 per row with offset, hex and ASCII columns, MUST toggle to binary with `Ctrl+B`, MUST mark EPB bytes in a distinct colour, MUST highlight the selected `SyntaxNode.span` after EPB mapping, and MUST support search for a hex byte sequence across the whole file (`Ctrl+F`) returning results within 2 s for a 4 GiB file.
- Acceptance: Given `corpus/syn_h264_epb_dense`, when the tester searches `00 00 03`, then the result count equals `Σ units[].epb_positions.length` and clicking a result selects the containing unit.

**FR-GUI-015** (Phase 1) — The Container panel MUST show the `ContainerTree` as a tree with columns fourcc/Name, Offset, Size, Version/Flags, Description; expanding a node MUST call `ovt_container_expand` lazily; selecting a `Field` MUST highlight its bits in the Hex panel (which switches to file-offset mode for container fields).
- Acceptance: Given `corpus/syn_h265_main10_pq_hdr`, when the tester expands `moov → trak → mdia → minf → stbl → stsd → hvc1 → colr` and clicks `transfer_characteristics`, then the Hex panel highlights 2 bytes at the field's absolute file offset equal to `ovt probe --boxes --format json` `file_span.offset + span.bit_offset/8`.

**FR-GUI-016** (Phase 1) — The HDR panel MUST display the fields of FR-HDR-002 and, once WI-4.452 lands (phase 4), the live measurements of FR-HDR-006 for the current frame (declared values only until then), and the status bar MUST show the display's HDR capability (`SDR` / `HDR headroom 4.0×` / `PQ pass-through`), distinguishing *declared* (container/bitstream metadata) from *measured* (FR-HDR-006) values by label.
- Acceptance: Given `corpus/syn_h265_main10_pq_hdr` on an SDR display, when the frame changes, then the panel's `MaxCLL (frame)` updates within 100 ms and the status bar reads `SDR · tone-mapped BT.2390`.

**FR-GUI-017** (Phase 2) — The Filmstrip MUST show thumbnails (≤ 160 px wide) for a window of ≥ 32 frames around the current frame in the current order, with frame type colour, `POC`/`OrderHint`, `decode_index`/`display_index`, reference arrows from `declared_refs` (long-term/ALTREF in a distinct hue), and a "used as reference" dot, and MUST fetch thumbnails from the playback path on a worker thread with cancellation when the window moves.
- Acceptance: Given `corpus/syn_h265_bpyramid`, when the current frame is 8, then the Filmstrip shows arrows from frame 8 to each of its `declared_refs` and the screenshot matches `tests/golden/filmstrip_bpyramid_f8.png` within 2 %.

**FR-GUI-018** (Phase 2) — The Reference Graph panel MUST draw the GOP structure as a directed graph (nodes = frames by `temporal_id` row and decode order column; edges = references) for the GOP containing the current frame, MUST highlight the DPB contents after the current frame, and MUST support export as SVG and Graphviz DOT.
- Acceptance: Given `corpus/syn_h265_bpyramid` at frame 8, when the tester exports DOT, then `dot -Tsvg` succeeds and the edge set equals `ovt index --refgraph --format json` for that GOP.

**FR-GUI-019** (Phase 2) — Every overlay mode MUST have a legend toggled by `L` naming each colour/glyph (UX-10), and every heat-map overlay MUST use a user-selectable colour map from {`viridis`, `cividis`, `magma`, `turbo`, `grey`} with a numeric scale (min/max auto or fixed).
- Acceptance: Given F5 (QP map) active, when the tester presses `L`, then a legend showing the QP range and colour ramp appears; when the colour map is changed to `cividis`, then the overlay and legend update together (`ui-test:LegendColormap`).

**FR-GUI-020** (Phase 1) — Frame type colours MUST be exactly the values of chapter 10 §10.5.2 `kOverlayColors[]` rows 30–32 (`#E53935` I/Key/IntraOnly, `#1E88E5` P/Inter, `#43A047` B; AV1 Switch `#9E9E9E` per chapter 11 §11.4.5), exposed to QML once as theme tokens `frame.i`, `frame.p`, `frame.b`, `frame.switch` that read those constants (no hex literal is repeated in QML), and the colour-blind-safe alternative palette MUST add shape coding (I = square, P = triangle, B = circle) in the filmstrip and timeline legend (UX-6).
- Acceptance: Given the tester enables Settings → Accessibility → Colour-blind palette, when the timeline renders `corpus/syn_h264_gop60_1080p`, then the legend shows the three shapes and the grab's I-bar colour equals the alternative token value (`ui-test:ColourBlindPalette`).

**FR-GUI-021** (Phase 2) — Dual view MUST show two `VideoSurface` instances (horizontal or vertical arrangement) for slots A and B, with frame, zoom, pan, plane mode and overlay mode synchronised by default and individually unlockable, plus the compare modes of FR-PLN-007 in single-surface mode (UX-7; chapter 11 §11.10, realised by WI-2.489).
- Acceptance: Given `corpus/syn_metrics_1080p_pair` in dual view, when the tester pans the left surface by (100, 50) pixels, then the right surface pans identically; when "unlock pan" is enabled, then it does not.

**FR-GUI-022** (Phase 1) — All operations that may take longer than 50 ms (the GUI budget of chapter 11 §11.7; 100 ms in the C API rule of FR-API-004) MUST run on `jobs::Pool` threads with a progress indicator in the status bar and a cancel button, and the UI thread MUST never block on core calls other than the cheap getters listed in `docs/dev/threading.md`.
- Acceptance: Given `corpus/syn_h264_1h_1080p`, when the index builds, then the UI responds to a window resize within 50 ms (measured by the Qt event-loop latency probe `app/tests/latency_probe`) and pressing Cancel stops the job within 100 ms.

**FR-GUI-023** (Phase 1) — All user-visible strings MUST be wrapped in `qsTr()` and extracted to `app/i18n/ovt-studio_*.ts`, and the application MUST switch language from Settings without restart; the initial languages are English (source) and a pseudo-locale for testing.
- Acceptance: Given `lupdate` runs in CI, when `tools/check_qstr.py` scans `app/qml/**/*.qml`, then zero unwrapped string literals longer than 1 character are reported outside the allow-list; when the pseudo-locale is selected, then every menu label is bracketed `[…]`.

**FR-GUI-024** (Phase 1) — Every panel MUST be operable by keyboard alone (Tab order, arrow keys in trees and lists, `Enter` to activate, `Esc` to close floating panels), focus MUST be visibly indicated, and text/background contrast MUST be ≥ 4.5:1 in both light and dark themes.
- Acceptance: Given `ovt-studio` with the Syntax panel focused, when the tester navigates with arrow keys and presses `Enter` on a VCL unit, then the current frame changes; when `tools/check_contrast.py` runs on the theme tokens, then every text/background pair reports ≥ 4.5.

**FR-GUI-025** (Phase 1) — The GUI MUST provide light and dark themes following the OS setting by default, and the picture region background MUST be selectable among black, 18 % grey and checkerboard independent of the theme.
- Acceptance: Given macOS set to Dark, when `ovt-studio` starts, then the window uses the dark token set (`ui-test:ThemeFollowsOs`).

**FR-GUI-026** (Phase 1) — The Diagnostics panel MUST list every `Diagnostic` from `ContainerTree`, `StreamIndex` and the current `FrameAnalysis` with severity icon, code, message, spec reference and location, MUST be filterable by severity and code prefix, MUST turn its tab red when any Error/Fatal exists, and MUST export as JSON and CSV.
- Acceptance: Given `corpus/syn_broken_h265_truncated_sps`, when the file opens, then the Diagnostics tab is red and the exported JSON equals `ovt syntax --format json | jq .diagnostics`.

**FR-GUI-027** (Phase 1) — `ovt-studio` MUST accept a file path as its first positional argument, MUST accept drag-and-drop of files onto the window, MUST keep a recent-files list of 20 entries, and MUST open a second file in a new window (`Ctrl+N`) or as compare slot B (`Ctrl+Shift+O`).
- Acceptance: Given `ovt-studio corpus/syn_h264_gop60_1080p`, when the window appears, then the stream is loaded; when the tester drops `corpus/syn_h265_gop60_1080p`, then it replaces slot A.

**FR-GUI-028** (Phase 2) — The GUI MUST provide an Extract dialog exposing FR-SYN-018 (unit, frame, minimum CVS, RAPs) and an Export menu exposing FrameAnalysis (FlatBuffers/JSON/YUView CSV/MV CSV), Statistics (CSV/JSON), Frame grabs (PNG/raw), Diagnostics (JSON/CSV) and, from phase 3, Metrics (CSV/JSON/FlatBuffers), each writing the identical bytes to the corresponding `ovt` subcommand.
- Acceptance: Given `corpus/syn_h265_gop60_1080p`, when the tester exports FrameAnalysis JSON for frame 2 and `ovt analyze --frames 2 --format json -o cli.json` runs, then `diff gui.json cli.json` is empty.

**FR-GUI-029** (Phase 1) — Memory held by the GUI for one open stream MUST stay ≤ 2 GB RSS at 4K on desktop (chapter 0 §0.7.3), with `FrameAnalysis` grids in an LRU of 64 frames, planes in an LRU of 4 frames, and thumbnails in an LRU of 512 entries.
- Acceptance: Given `corpus/syn_av1_4k_2min`, when the tester scrubs 200 frames with F2 active, then peak RSS reported by `ovt-studio --measure-memory` is ≤ 2048 MiB.

**FR-GUI-030** (Phase 1) — `ovt-studio` MUST render the picture region through `libovt-render`'s `VideoSurface` embedded in a `QQuickRhiItem`/`QSGRenderNode` (ADR-004) using Vulkan (MoltenVK on macOS, Vulkan or GLES on Android, Vulkan on Linux), and MUST NOT draw video or overlays with Qt Quick scene-graph primitives.
- Acceptance: Given `ovt-studio --dump-renderer`, when it starts, then the log shows `VideoSurface backend=vulkan(moltenvk)` on macOS and `grep -r "Image {" app/qml/PictureRegion*.qml` returns no match.

**FR-GUI-031** (Phase 2) — Statistics panel charts (pie/bar/histogram) MUST be resizable with the panel, MUST support raw/area-normalised/bits-normalised modes (FR-BLK-020), and MUST export the visible chart as PNG and SVG.
- Acceptance: Given `corpus/syn_h265_gop60_1080p` at frame 2, when the tester resizes the Statistics panel from 400 to 900 px wide, then the chart re-lays out with no clipping and SVG export produces a valid SVG whose bar values equal the CSV export.

**FR-GUI-032** (Phase 4) — The Buffer panel MUST draw the HRD/CPB fullness curve of FR-DEC-013 with the cursor synchronised to the current frame, showing fullness at the cursor in bits and as a percentage of `CpbSize`, and marking underflow/overflow events in red.
- Acceptance: Given `corpus/syn_h264_hrd_cbr` at frame 100, when the Buffer panel is open, then the cursor is at frame 100 and the shown fullness equals `ovt hrd --format json | jq .frames[100].fullness_bits`.

---

**FR-GUI-101** (Phase 1) — Everything the GUI shows MUST be obtainable from core data (`StreamIndex`, `SyntaxTree`, `ContainerTree`, `FrameAnalysis`, `MetricsResult`, `Measurements`); the GUI MUST NOT parse or compute codec data itself.
- Acceptance: Given any panel value in `ovt-studio`, when the matching `ovt` CLI command of chapter 12 runs with `--format json`, then the value is present in the JSON and equal (`ui-test:PanelCliParity` over the fast corpus).

**FR-GUI-102** (Phase 1) — Specification terminology MUST be used in all labels (`SyntaxNode.name`, `BlockKind` names, `RefSlot.name`, untranslated); human explanations go in tooltips and derived columns (chapter 11 §11.3.4, §11.3.9, §11.6).
- Acceptance: Given the Syntax panel on `corpus/syn_h265_gop60_1080p`, when the tree is exported with Copy-as-JSON, then every `name` equals the corresponding `SyntaxNode.name` byte for byte.

**FR-GUI-103** (Phase 2) — The GUI MUST provide a Bookmarks & Comments panel: a bookmark is a (`FrameId`, optional block id, text) tuple stored in the `.ovtproj` project file; `B` adds a bookmark at the current frame, the Timeline shows bookmark markers, and bookmarks MUST be exportable as JSON and CSV (chapter 11 §11.3.17).
- Acceptance: Given `corpus/syn_h264_gop60_1080p`, when the tester presses `B` on frames 3 and 17 and saves the project, then the `.ovtproj` contains two `bookmarks[]` entries and reopening the project shows two markers on the timeline (`ui-test:Bookmarks`).

## 4.10 CLI — ovt command line

**Rationale.** UX-11 (same engine for GUI and CLI) and persona commitments A10 and B7; chapter 3 row 53. ADR-003 fixes that the CLI is a thin front-end over `libovt-core`; ADR-010 fixes FlatBuffers/JSON as output. These requirements fix the subcommand grammar, output stability, exit codes and locale independence so that the CLI is scriptable from CI pipelines.

**FR-CLI-001** (Phase 1) — The `ovt` executable MUST implement exactly the subcommands of chapter 12 §12.2.0: `info`, `probe`, `syntax`, `index`, `frames`, `dump`, `extract`, `cache`, `completion`, `render` in phase 1; `analyze`, `plugins`, `stats` in phase 2; `metrics`, `bdrate`, `hdr`, `compare`, `scenes`, `models` in phase 3; `verify`, `doctor`, `hrd` in phase 4 (aliases `decode` = `dump`, `units` = `syntax --list`), with kebab-case flags and `--help` for each subcommand generated from one declarative table per command in `cli/src/cmd/<name>.cpp`.
- Acceptance: Given a build, when `ovt --help` runs, then it lists exactly the subcommands of the current phase and `ovt <sub> --help` exits 0 for each.

**FR-CLI-002** (Phase 1) — Every subcommand MUST support `--format {text,json,fb,csv,…}` (chapter 12 §12.1.2; `--format json`, `--text` and `--flatbuffers` are aliases; `--pretty`/`--compact` select JSON layout), and `--output <path>` (`-o`, default stdout), and the JSON MUST validate against the published JSON Schemas in `docs/spec/schemas/*.schema.json`, which are generated from the FlatBuffers schemas by `tools/fbs_to_jsonschema.py`.
- Acceptance: Given `corpus/syn_{h264,h265,av1}_gop60_1080p`, when `ovt index --format json` runs, then `check-jsonschema --schemafile docs/spec/schemas/StreamIndex.schema.json` passes for each output.

**FR-CLI-003** (Phase 1) — Exit codes MUST be those of chapter 12 §12.1.3: 0 OK, 1 ERROR (internal), 2 USAGE, 3 IO (`OVT_E_IO`), 4 UNSUPPORTED (`OVT_E_UNSUPPORTED`), 5 CORRUPT (`OVT_E_CORRUPT`, output still produced when partial results exist and `--partial-ok` is set), 6 PLUGIN, 7 CANCELLED (SIGINT), 8 MISMATCH (verify/compare), 9 PARTIAL; diagnostics MUST go to stderr as JSON Lines when `--format json` is in effect and as human text otherwise.
- Acceptance: Given `corpus/syn_broken_h265_truncated_sps`, when `ovt syntax --format json --partial-ok <file> > out.json 2> err.jsonl` runs, then `$? == 5`, `out.json` is valid JSON and `err.jsonl` contains a line with `"code":"SYN-H265-0003"`.

**FR-CLI-004** (Phase 1) — `ovt probe <file>` MUST output the `ContainerTree` (with `--tree` for all boxes, default: tracks and top-level boxes), `TrackInfo` list, `ovt_ColorDescription` per track, `bitrate_bps`, duration, frame count and the sniffed `format`, within 300 ms for a 1-hour MP4 (FR-CNT-018).
- Acceptance: Given `corpus/syn_h265_main10_pq_hdr`, when `ovt probe --format json` runs, then the output contains `format`, `tracks[0].codec == "H265"`, `tracks[0].color.transfer == 16`, `tracks[0].bitrate_bps` within 1 % of `ffprobe -show_format` `bit_rate`.

**FR-CLI-005** (Phase 1) — `ovt syntax <file>` MUST output the `SyntaxNode` trees of all units (`--unit a-b`, `--frame N`, `--type SPS,PPS,SEI` filters; `--filter "<expr>"` per FR-SYN-015), and `ovt syntax --format text` MUST output an indented text form `name = value (descriptor) [derived] @bit_offset+len` compatible in spirit with `trace_headers`.
- Acceptance: Given `corpus/syn_h265_gop60_1080p`, when `ovt syntax --type SPS --text` runs, then the output contains the line `sps_temporal_mvp_enabled_flag = 1 (u(1)) @…` with the bit offset equal to the JSON `span.bit_offset`.

**FR-CLI-006** (Phase 1) — `ovt index <file>` MUST output the `StreamIndex` (with `--summary` for a one-screen table: codec, profile/level, resolution, frame count, GOP lengths min/avg/max, frame type counts, average bit rate, RAP count) and MUST write/read the on-disk cache under the cache root of chapter 6 §6.7 (D32: `$XDG_CACHE_HOME/ovt/` on Linux, `~/Library/Caches/org.openvideotools.ovt/` on macOS, the app cache dir on Android) keyed by `source_hash` unless `--no-cache`.
- Acceptance: Given `corpus/syn_h264_1h_1080p`, when `ovt index --summary` runs twice, then the second run reports `cache: hit` and completes in ≤ 200 ms.

**FR-CLI-007** (Phase 1) — `ovt dump <file>` MUST write decoded frames as raw planar YUV (`--raw`), Y4M (`--y4m`), PNG (`--png`, 8-bit tone-mapped as rendered or `--png16`), or per-frame MD5 (`--md5`, hex per plane and combined, matching the HM/libaom `--md5` convention), honouring `--frames a-b`, `--display-order|--decode-order`, `--sw|--hw`, `--nocrop`, `--plane-stage <stage>` (phase 2), `--pixel x,y` and `--histogram y|u|v`.
- Acceptance: Given `corpus/jctvc_WPP_A_ericsson_MAIN10_2`, when `ovt dump --sw --md5 --frames 0-9` runs, then the ten MD5 lines equal the first ten lines of the conformance `.md5` file.

**FR-CLI-008** (Phase 2) — `ovt analyze <file> --frame N | --frames a-b` MUST output `FrameAnalysis` as `--format json`, `--flatbuffers`, `--format yuview` (YUView statistics CSV), `--format mv-csv` or `--format aom-inspect` (subset of libaom `inspect` JSON: `frame`, `blockSize`, `mode`, `skip`, `motionVectors`, `referenceFrame`, `transformSize`, `transformType`, `cdef`, `qindex`), with `--plugin <id>` selection and `--planes <stages>` to dump plane stages as raw files.
- Acceptance: Given `corpus/syn_av1_gop60_1080p`, when `ovt analyze --frames 0 --format aom-inspect` runs and `aomdec … | inspect` is run for the same frame, then the `blockSize`, `mode` and `qindex` arrays are equal.

**FR-CLI-009** (Phase 3) — `ovt metrics [--reference R] --distorted D [--distorted D2 …] --metric m1,m2,…` MUST run the metrics for each distorted file against the reference (`--reference` is optional for no-reference metrics such as `uvq`, `cambi` and `si_ti`) with the normalisation options `--scale-kernel`, `--offset`, `--crop`, `--roi`, `--domain`, `--display`, `--fps-policy`, `--chroma-policy`, `--threads`, `--map N|codec`, `--pool`, `--model`, output `--format json|csv|vmaf-json|fb`, and `--quick` (every 5th frame, `n_subsample` for VMAF).
- Acceptance: Given `corpus/syn_metrics_1080p_pair`, when `ovt metrics --reference ref.y4m --distorted dist.mp4 --metric psnr,ssim,vmaf --format csv` runs, then the CSV header is `frame,display_index,psnr_y,psnr_u,psnr_v,ssim_y,ssim_u,ssim_v,vmaf` and the row count equals the aligned frame count.

**FR-CLI-010** (Phase 1) — All numeric output MUST use `.` as decimal separator and no thousands separator regardless of `LANG`/`LC_ALL`, and the CLI MUST set `LC_NUMERIC=C` internally before any formatting.
- Acceptance: Given `LC_ALL=de_DE.UTF-8`, when `ovt probe --format json corpus/syn_h264_gop60_1080p | jq .tracks[0].duration_s` runs, then it prints a number with a `.` decimal point.

**FR-CLI-011** (Phase 1) — The CLI MUST handle SIGINT by cancelling the running `ovt_Job`, flushing partial output when `--partial-ok` is set, and exiting with code 7 (CANCELLED) within 200 ms.
- Acceptance: Given `ovt index corpus/syn_h264_1h_1080p --format json -o out.json`, when SIGINT is sent after 500 ms, then the process exits with 7 within 200 ms and `out.json` is absent (or valid partial JSON with `--partial-ok`).

**FR-CLI-012** (Phase 1) — Reading from stdin (`ovt probe -`, `ovt index -`) MUST be supported for Annex B, OBU and Y4M inputs by spooling to a temporary file when the input is not seekable, and `--raw-format WxH:pix_fmt[:fps]` (chapter 12 §12.1.5, D21) MUST be accepted for headerless YUV from stdin.
- Acceptance: Given `cat corpus/syn_h265_gop60_1080p | ovt index - --summary`, when it runs, then the summary equals `ovt index corpus/syn_h265_gop60_1080p --summary`.

**FR-CLI-013** (Phase 1) — `ovt extract <file>` MUST implement FR-SYN-018 with the options of chapter 12 §12.2.7: `--units <sel>` (exactly these units), `--frames <sel>` (these frames plus, with `--min-cvs`, default on, the parameter sets and decode chain from the preceding RAP; `--no-min-cvs` copies only the frames' own units), `--raps` (every RAP picture with its parameter sets), `--to annexb|obu|ivf` (output framing) and `-o <path>`; the selector grammar is that of chapter 12 §12.1.2 `--frames`. (The earlier spellings `--unit`, `--frame`, `--format` survive only as aliases in chapter 12 §12.2.0.)
- Acceptance: Given `corpus/syn_h264_gop60_1080p`, when `ovt extract --raps --to annexb -o raps.264` runs, then `ffprobe -show_frames raps.264` lists only `pict_type=I` frames and the count equals the RAP count in `ovt index --summary`.

**FR-CLI-014** (Phase 4) — `ovt verify <file>` MUST run FR-SYN-019 (`--hash-sei`; D21), FR-SYN-020 (`--levels`), plugin cross-check (`--plugin a --against b`; `--plugin-compare a,b` is an alias, chapter 12 §12.2.0) and syntax range checks, MUST output a report JSON with `pass|fail` per check and all `Diagnostic`s, and MUST exit 0 only when every requested check passes.
- Acceptance: Given `corpus/jctvc_*`, when `ovt verify --hash-sei --levels <file>` runs for each, then every exit code is 0; given `corpus/syn_broken_h265_hash_mismatch`, then exit code is 8 (MISMATCH) and `checks.hash == "fail"`.

**FR-CLI-015** (Phase 1) — The CLI MUST support `--log-level trace|debug|info|warn|error` and `--log-file`, MUST default to `warn`, and MUST never print to stdout anything other than the requested output so that `ovt … --format json | jq` always succeeds.
- Acceptance: Given `ovt index corpus/syn_broken_h265_truncated_sps --format json --log-level trace 2>/dev/null | jq .`, when it runs, then `jq` exits 0.

**FR-CLI-016** (Phase 3) — `ovt compare A B` MUST implement the first-mismatch finder (FR-PLN-009, `--first-diff`), a frame-by-frame identity report (`--identity`), and a difference image export (`--diff-image --frame N --gain G -o diff.png`).
- Acceptance: Given `corpus/syn_metrics_1080p_{pair,bitflip_f37}`, when `ovt compare --identity A B --format json` runs, then `frames[37].differing_samples == [1,0,0]`.

---

**FR-CLI-101** (Phase 1) — Every capability of `ovt-studio` MUST be available as an `ovt` subcommand that reads the same inputs and emits the same FlatBuffers records as the GUI consumes (chapter 12 §12.2).
- Acceptance: Given the feature matrix of chapter 11, when each row is checked against chapter 12 §12.2, then every row names a subcommand and flag (phase-4 audit WI-4.599).

**FR-CLI-102** (Phase 1) — `ovt` MUST emit machine-readable output by default when stdout is not a TTY (`--format json`) and human text when it is (`--format text`).
- Acceptance: Given `ovt info a.mp4 | cat`, when it runs, then stdout parses as JSON; given an interactive terminal, then a human table is printed.

**FR-CLI-103** (Phase 1) — `ovt` MUST support the formats `json`, `fb`, `csv`, `yuview`, `text` (and `aom-inspect`, `mv-csv` for `analyze`, `vmaf-json` for `metrics`); a format that a subcommand does not support MUST produce exit code 2 (USAGE) and a message naming the supported formats.
- Acceptance: Given `ovt info a.mp4 --format yuview`, when it runs, then the exit code is 2 and stderr contains `supported: json, fb, text`.

**FR-CLI-104** (Phase 1) — CLI output MUST be deterministic: two runs on the same input with the same flags MUST produce byte-identical stdout (chapter 0 §0.7 rule 5); no timestamps, hostnames, thread ids, absolute cache paths or pointer values may appear in the output.
- Acceptance: Given any corpus file, when `ovt syntax f --format fb` runs twice, then `cmp` of the two outputs succeeds.

**FR-CLI-105** (Phase 1) — All logs and progress MUST go to stderr; stdout carries only the result.
- Acceptance: Given `ovt index f --log-level trace --progress 2>/dev/null`, when it runs, then stdout is valid JSON.

**FR-CLI-106** (Phase 1) — `ovt` MUST accept `-` as an input path meaning stdin for formats that do not require random access (Annex B, OBU, IVF, Y4M); for ISOBMFF input from stdin it MUST spool to a temporary file under `--cache-dir` and continue.
- Acceptance: Given `cat a.h264 | ovt syntax -`, when it runs, then the output equals `ovt syntax a.h264`.

**FR-CLI-107** (Phase 1) — `ovt` MUST be usable without a display server, GPU or Qt (no Qt dependency in `cli/`; `OVT_BUILD_CLI` independent of `OVT_BUILD_GUI`, chapter 13 §13.2.2).
- Acceptance: Given a Linux container without `libQt6*`, when `ovt --version` runs, then it succeeds.

**FR-CLI-108** (Phase 1) — The same `ovt` binary MUST run on macOS, Linux and (as a test-only binary pushed with `adb`) Android; the Android APK does not ship the CLI.
- Acceptance: Given the `android-arm64` preset, when it is built, then `cli/ovt` exists and `adb shell /data/local/tmp/ovt --version` prints the version.

**FR-CLI-109** (Phase 1) — `ovt --help` and `ovt <sub> --help` MUST print within 50 ms and MUST NOT load any plugin or open any file.
- Acceptance: Given `time ovt analyze --help`, when it runs on the M1 reference, then wall time is ≤ 50 ms.

## 4.11 API — Library and scripting

**Rationale.** chapter 3 §3.6 item 3: no commercial tool offers a scripting API. ADR-003 exposes everything through the C API and FlatBuffers; the Python package (phase 4) wraps it. These requirements guarantee ABI stability, ownership rules and parity between API, CLI and GUI.

**FR-API-001** (Phase 1) — The C API in `core/include/ovt/*.h` MUST be the only public interface of `libovt-core`, MUST compile as C11 and C++20, MUST use only the types declared in chapter 0 §0.6.1 (`ovt_status`, `ovt_Buffer`, opaque handles, `ovt_ColorDescription`, `ovt_PlaneView`, `ovt_FrameId`), and every function MUST be documented with Doxygen including ownership of every pointer.
- Acceptance: Given the headers, when `core/tests/c_api_compile.c` is compiled with `clang -std=c11 -Wall -Werror -fsyntax-only` and `doxygen -q` runs with `WARN_AS_ERROR = YES`, then both exit 0.

**FR-API-002** (Phase 1) — Ownership MUST follow D4 and chapter 12 §12.3.1 rule 3: a FlatBuffer or buffer returned through an out-parameter is owned by the caller and released with `ovt_buffer_free` (or the type-specific `ovt_<type>_release`, e.g. `ovt_picture_release` for the ref-counted `ovt_Picture`); a pointer returned by value (`ovt_container_tree`, `ovt_stream_index`, `ovt_file_display_name`) stays owned by the handle and is valid until that handle's `*_close`; every ABI struct starts with `uint32_t struct_size`; all entry points are `noexcept`; and the API MUST never require the caller to free memory with `free()`.
- Acceptance: Given `test:ApiOwnership` runs under ASan and LeakSanitizer, when it opens, indexes, analyses one frame and closes `corpus/syn_h265_gop60_1080p` 100 times, then zero leaks and zero use-after-free reports occur.

**FR-API-003** (Phase 1) — The C API MUST be versioned with `OVT_API_VERSION_MAJOR/MINOR`, exported symbols MUST be limited to `ovt_*` via a version script / `-fvisibility=hidden`, and a minor version increase MUST NOT remove or change the signature of any function.
- Acceptance: Given two consecutive releases, when `abi-compliance-checker` (Linux) compares `libovt-core.so`, then it reports no removed symbols and no changed signatures for a minor bump.

**FR-API-004** (Phase 1) — Cancellation and progress MUST be exposed through `ovt_Job` (`ovt_job_create`, `ovt_job_cancel`, `ovt_job_progress` filling an `ovt_JobProgress` whose `fraction` is 0.0–1.0, `ovt_job_set_progress_callback`), and every API function that can run longer than 100 ms MUST accept a nullable `ovt_Job*`.
- Acceptance: Given `test:ApiJobCancel`, when `ovt_stream_build_index` on `corpus/syn_h264_1h_1080p` is cancelled from another thread after 200 ms, then it returns `OVT_E_CANCELLED` within 50 ms of the `ovt_job_cancel` call (NFR-007) and `ovt_job_progress` was observed strictly increasing.

**FR-API-005** (Phase 1) — Core objects MUST document thread-safety per chapter 0 §0.7.2: an `ovt_File` and `ovt_Container` MAY be shared read-only across threads; `ovt_Stream`, `ovt_Analysis`, `ovt_Decoder` MUST be used by one thread at a time; violations MUST be detected in debug builds with an assertion naming the object.
- Acceptance: Given `test:ApiThreadMisuse` (debug build), when two threads call `ovt_analysis_frame` on one `ovt_Analysis` concurrently, then the process aborts with the message `ovt_Analysis used from two threads`.

**FR-API-006** (Phase 1) — Logging MUST be routed through `ovt_set_logger`; when no logger is set the library MUST be silent on stdout and stderr.
- Acceptance: Given `test:ApiSilentDefault`, when the full open/index/analyze sequence runs with no logger set, then captured stdout and stderr are empty.

**FR-API-007** (Phase 4) — The Python package `ovt` (pybind11, `python/`) MUST expose `ovt.open(path) → Stream`, `Stream.index() → StreamIndex`, `Stream.container → ContainerTree`, `Stream.analyze(decode_index, planes=…) → FrameAnalysis`, `Stream.decode(display_index) → Picture` (NumPy views of planes, zero-copy), `ovt.metrics(ref, dist, metrics=[…], **normalisation) → MetricsResult`, `ovt.bdrate(...)`, with FlatBuffer-backed objects offering attribute access and `.to_json()`, and NumPy arrays for every `CellGrid` vector (`grid.qp` shape `(cells_h, cells_w)`); wheels are per interpreter (`cp311`, `cp312`, `cp313`; pybind11 has no stable-ABI build).
- Acceptance: Given `pip install ./python`, when `python -c "import ovt, numpy; s=ovt.open('corpus/syn_h265_gop60_1080p'); fa=s.analyze(2); assert fa.grid.qp.shape==(fa.grid.cells_h, fa.grid.cells_w); print(fa.to_json()[:1])"` runs, then it prints `{` and exits 0, and `python/tests/test_parity.py` shows `fa.to_json()` equal to `ovt analyze --frames 2 --format json --compact`.

**FR-API-008** (Phase 4) — The Python package MUST ship wheels for macOS arm64/x86_64 and Linux x86_64/arm64 (manylinux_2_28) via `cibuildwheel`, bundling `libovt-core` and the AV1/libvmaf components, with AVC/HEVC decoder plugins loaded from the system `ovt` installation when present (ADR-015).
- Acceptance: Given the CI release job, when it finishes, then one wheel per platform and supported interpreter (`cp311`, `cp312`, `cp313`) exists and `pip install ovt-*.whl && python -c "import ovt; print(ovt.__version__)"` succeeds on each platform runner.

**FR-API-009** (Phase 1) — The plugin ABI headers (`core/include/ovt/abi/*.h`) MUST be self-contained (no includes other than `<stdint.h>` and `<stddef.h>`), BSD-2-Clause licensed, and a third-party plugin compiled against them MUST load in `ovt` without any OVT source other than those headers.
- Acceptance: Given `tests/abi/external_plugin/` (built with only the two ABI headers on the include path), when `ovt plugins --plugin-dir tests/abi/external_plugin/build` runs, then the plugin is listed with its `id`.

**FR-API-010** (Phase 1) — The Python module and every API consumer MUST never evaluate input content as code; `options_json` passed to metric plugins MUST be parsed with a strict JSON parser and rejected on unknown keys.
- Acceptance: Given `test:MetricOptionsStrict`, when `create("{\"__import__\":1}")` is called on the `psnr` plugin, then it returns `OVT_E_INVALID_ARG`.

---

**FR-API-101** (Phase 4) — `pip install ovt` on a supported platform MUST import without any system dependency other than libc (glibc ≥ 2.28 on `manylinux_2_28`; libstdc++ linked statically into the extension) or macOS 13.
- Acceptance: Given a fresh `python:3.12` manylinux container, when `pip install ovt && python -c "import ovt; ovt.doctor()"` runs, then it exits 0.

**FR-API-102** (Phase 4) — `fa.grid.*`, `pic.y/u/v` and the `plane()` method of the Python `FrameAnalysis` wrapper MUST be zero-copy (no allocation proportional to the data size).
- Acceptance: Given a 4K frame, when `tracemalloc` wraps `fa.grid.qp`, then the peak allocation is < 4 KiB.

**FR-API-103** (Phase 4) — Every `ovt_status` ≠ `OVT_OK` MUST raise the corresponding `OvtError` subclass with the `Diagnostic` attached (chapter 12 §12.4.2).
- Acceptance: Given a truncated file, when `ovt.open(f).video.index()` runs, then `ovt.Corrupt` is raised with `.diagnostic["code"]` set.

**FR-API-104** (Phase 4) — `ovt.metrics.run(...)` with `metrics=["vmaf"]` MUST give pooled values equal to the `vmaf` CLI output within 1e-4.
- Acceptance: Given the Netflix test vectors (chapter 14), when both run, then `abs(diff) < 1e-4` for every frame.

**FR-API-105** (Phase 4) — The Python stubs MUST describe every public symbol and `mypy --strict` on `python/examples/` MUST pass.
- Acceptance: Given the CI job `python-typecheck`, when it runs on `python/`, then it exits 0.

**FR-API-106** (Phase 4) — `import ovt` MUST take ≤ 300 ms (no plugin scan at import; scan on first use).
- Acceptance: Given `python -X importtime -c "import ovt"`, when it runs on the M1 reference, then the cumulative import time is ≤ 300 ms.

## 4.12 AND — Android and ChromeOS

**Rationale.** Backbone chapter 0 §0.0 names Android tablets and Chromebooks as concurrent primary platforms; chapter 3 §3.6 item 1 shows no competitor has them. ADR-004 (Qt Quick via `QtQuickView`), ADR-014 (`SurfaceView` HDR path, `TextureView` forbidden) and Appendix B.4 (desktop windowing, `content://`, unverified ChromeOS HDR) drive these requirements. "Pixel Tablet" and "HDR Chromebook" are the reference devices named in chapter 13.

**FR-AND-001** (Phase 0) — The Android app MUST be a Gradle project under `android/` producing one APK/AAB with `arm64-v8a` and `x86_64` ABIs, `minSdk 28`, `targetSdk 36`, built by `cmake --preset android-{arm64,x64}-release` plus `./gradlew assembleRelease`, and installable on a Pixel Tablet and an x86_64 Chromebook.
- Acceptance: Given the CI `android-build` job, when it finishes, then `app-release.apk` contains `lib/arm64-v8a/libovt-core.so` and `lib/x86_64/libovt-core.so` (`unzip -l`), and `adb install` succeeds on both reference devices (`hw-test:AndroidInstall`).

**FR-AND-002** (Phase 0) — Video and overlays MUST be rendered to a `SurfaceView` whose buffers are tagged with `ANativeWindow_setBuffersDataSpace` (`ADATASPACE_BT2020_PQ` or `ADATASPACE_BT2020_HLG` for HDR content, `ADATASPACE_BT709` otherwise), MUST NOT use `TextureView`, and the Qt Quick UI MUST be hosted in a `QtQuickView` sibling layered above the `SurfaceView` with a transparent background.
- Acceptance: Given the phase-0 APK on the `android-hdr` device (chapter 14 §14.9.2; `chromebook-hdr` may stand in — D12: the Pixel Tablet is SDR and shows the documented fallback), when the PQ test pattern (`corpus/syn_h265_main10_pq_hdr`) is shown, then `adb shell dumpsys SurfaceFlinger` lists the app layer with dataspace `BT2020_PQ` and `grep -r TextureView android/` returns no match.

**FR-AND-003** (Phase 0) — The app MUST query `Display.getMode().getSupportedHdrTypes()` and `Display.getHdrSdrRatio()` (API ≥ 34) and on API ≥ 35 MUST call `SurfaceView.setDesiredHdrHeadroom()` with `min(content_peak / 203, max supported)` while HDR content is shown, and MUST request `COLOR_MODE_HDR` on the window.
- Acceptance: Given `corpus/syn_h265_main10_pq_hdr` on the `android-hdr` acceptance device of chapter 14 §14.9.2 (the Pixel Tablet is SDR-only, D12), when displayed, then `adb logcat -s OVT` shows `hdr: types=[HDR10,HLG] ratio=<r> desiredHeadroom=4.93` and the 1000-nit patch is visibly brighter than UI white (`hw-test:AndroidPqHeadroom`, photographed).

**FR-AND-004** (Phase 0) — The ChromeOS HDR spike MUST be executed in week one of phase 0 on an HDR-capable Chromebook (or Chromebook + HDR monitor): render the PQ pattern via the `SurfaceView` path and record whether Exo passes it through as HDR; the result MUST be recorded in `docs/adr/ADR-014-chromeos-result.md` and, if negative, the Chromebook build MUST use the tone-mapped SDR path with the FR-HDR-007 badge.
- Acceptance: Given the spike, when `docs/adr/ADR-014-chromeos-result.md` exists, then it contains the device model, ChromeOS version, measured or photographed luminance of the 203- and 1000-nit patches and a `verdict: passthrough|tonemapped` line.

**FR-AND-005** (Phase A/1) — Files MUST be opened through the Storage Access Framework (`ACTION_OPEN_DOCUMENT`, `content://` URIs) with persisted URI permission (`takePersistableUriPermission`), passing the `ParcelFileDescriptor` to `ovt_file_open_fd`, and the core MUST read via `pread` on that descriptor without copying the file; when the provider returns a non-seekable descriptor (pipe), the app MUST fall back to a one-time copy into the app cache directory with a visible progress indicator.
- Acceptance: Given a 4 GiB MP4 in Downloads on the Pixel Tablet, when the user opens it via the picker, then the file is indexed and the app's data directory size (`adb shell du -s`) grows by less than 50 MiB (no copy), and reopening after restart needs no new picker interaction.

**FR-AND-006** (Phase A/1) — The app MUST also accept `ACTION_VIEW`/`ACTION_SEND` intents for MIME types `video/mp4`, `video/x-matroska`, `video/webm`, `video/x-ivf`, `application/octet-stream` with extensions `.264 .h264 .265 .hevc .obu .ivf .y4m .yuv`, and `file://` URIs from file managers.
- Acceptance: Given `adb shell am start -a android.intent.action.VIEW -d content://… -t video/mp4 org.openvideotools.studio`, when it runs, then the app opens the stream and shows the first frame within 2 s.

**FR-AND-007** (Phase A/1) — The activity MUST declare `android:resizeableActivity="true"`, MUST handle `configChanges` for `screenSize|smallestScreenSize|screenLayout|orientation|density|keyboard|keyboardHidden|navigation|uiMode` without recreation, and MUST preserve the open file, current frame, zoom, pan, overlay mode and panel layout across resize, rotation and free-form window changes on ChromeOS and Android desktop mode.
- Acceptance: Given the app in a free-form window on the Chromebook at frame 40 with 4× zoom, when the tester resizes the window from 1200×800 to 700×1000 and rotates the tablet, then the current frame is still 40, zoom still 4×, and `adb logcat` shows no `onCreate` after the initial one (`hw-test:ResizePreservesState`).

**FR-AND-008** (Phase A/1) — The layout MUST adapt by width class exactly as chapter 11 §11.5 defines: ≥ 1200 dp shows the full three-pane skeleton (FR-GUI-001); 840–1199 dp shows the two-pane layout (picture + right tabbed area, compact timeline, left drawer); 600–839 dp shows the picture region with the tabbed panel area below the picture; < 600 dp (phones, unsupported) a single pane with bottom navigation that MUST not crash; the timeline MUST remain visible in every class.
- Acceptance: Given the app on the Pixel Tablet, when the window is resized across the 1199/1200 dp, 839/840 dp and 599/600 dp boundaries using split-screen, then the layout changes to the specified class each time (`ui-test:AndroidWidthClasses` with the Qt Quick Test harness at the three widths).

**FR-AND-009** (Phase A/1) — Mouse and trackpad input MUST behave as on desktop: hover MUST show tooltips and hover highlights (`MotionEvent.ACTION_HOVER_MOVE`), the secondary button and ChromeOS two-finger tap MUST open the Details/context menu (`MotionEvent.BUTTON_SECONDARY`, `View.OnContextClickListener`), the wheel MUST zoom about the cursor, and `Ctrl`-wheel MUST zoom the timeline.
- Acceptance: Given a Bluetooth mouse on the Pixel Tablet, when the tester hovers a syntax node, then the FR-SYN-017 tooltip appears within 700 ms; when right-clicking a block, then the Details panel opens (`hw-test:AndroidMouse`).

**FR-AND-010** (Phase A/1) — Every keyboard binding of FR-GUI-008 MUST work with an attached keyboard on Android and ChromeOS, with `Ctrl` as the modifier (not `Cmd`), and F-keys MUST be received even when ChromeOS maps the top row to system keys (the app MUST also offer `Alt+1`…`Alt+=` as alternates for F1–F12).
- Acceptance: Given the Chromebook keyboard, when the tester presses `Alt+2`, then the F2 Prediction mode activates; when `Search+2` (ChromeOS F2) is pressed, then the same mode activates.

**FR-AND-011** (Phase A/1) — Touch input MUST support: one-finger drag pan, pinch zoom about the pinch centre, single tap select (with FR-GUI-009 cycling), long-press ≥ 500 ms for Details, two-finger tap for fit, and touch targets in panels MUST be ≥ 48 dp.
- Acceptance: Given the Pixel Tablet without a mouse, when the tester pinches at the picture centre, then zoom changes and the centre sample stays under the pinch centre ± 2 px; when the tester long-presses a block, then the Details sheet opens (`hw-test:AndroidTouch`).

**FR-AND-012** (Phase A/1) — External displays MUST be supported: when a second display is connected (`DisplayManager.DISPLAY_CATEGORY_PRESENTATION` or ChromeOS extended desktop), the user MUST be able to move the window to it, and HDR capability MUST be re-queried per display so that an HDR external monitor on an SDR tablet shows PQ pass-through while the internal panel shows tone-mapped output.
- Acceptance: Given the Chromebook with an HDR monitor attached, when the window is dragged to the monitor, then `adb logcat -s OVT` shows `display changed: id=<n> hdr=true` and the FR-HDR-007 badge disappears (`hw-test:ExternalHdrDisplay`).

**FR-AND-013** (Phase A/1) — Memory per open stream on Android MUST stay ≤ 1 GB PSS at 4K (chapter 0 §0.7.3), the `FrameAnalysis` LRU MUST be reduced to 16 frames and planes to 2 frames on devices with ≤ 8 GB RAM, and the app MUST handle `onTrimMemory(TRIM_MEMORY_RUNNING_LOW)` by clearing the LRUs.
- Acceptance: Given `corpus/syn_av1_4k_2min` on the Pixel Tablet, when the tester scrubs 100 frames with F2 active, then `adb shell dumpsys meminfo org.openvideotools.studio` reports `TOTAL PSS ≤ 1 048 576 kB`.

**FR-AND-014** (Phase A/2) — Analysis plugins MUST run on Android as in-process shared libraries built for both ABIs, with the libaom inspection build compiled with NEON (arm64) and SSE/AVX2 (x86_64) and libde265 with its SSE4.1 kernels on x86_64 (libde265 1.0.15 has no NEON and nothing beyond SSE4.1; appendix B), and the phase-2 analysis budget on the Pixel Tablet MUST be ≤ 5 s per 60-frame 1080p GOP on `pixel-tablet` (chapter 14 §14.9.3 / `tests/perf/budgets.json`, D19 — not a multiple of the M1 budget; chapter 0 §0.7.4 mirrors it).
- Acceptance: Given `corpus/syn_h265_gop60_1080p` on the Pixel Tablet, when the user requests analysis of frame 45, then the result appears within 5 s and the APK contains `libovt_plugin_de265.so` for both ABIs.

**FR-AND-015** (Phase A/1) — Playback decoding on Android MUST use MediaCodec through libavcodec's `*_mediacodec` decoders with surface output disabled (CPU-readable buffers for plane views) and MUST fall back to software (`dav1d` for AV1, libavcodec for AVC/HEVC when the decoder plugin is present) when MediaCodec rejects the stream. At the FFmpeg 7.1 pin, libavcodec maps no 10-bit MediaCodec buffer format (`COLOR_FormatYUVP010` is not supported in buffer mode; D30), so 10-bit HEVC/AV1 streams take the software fallback (the acceptance below exercises it).
- Acceptance: Given `corpus/syn_h265_422_10bit` on the Pixel Tablet, when opened, then `adb logcat -s OVT` shows `decoder: mediacodec rejected, fallback to software` and the first frame displays within 3 s.

**FR-AND-016** (Phase A/1) — Decoder plugins for AVC/HEVC MUST be loadable on Android either bundled in the APK (F-Droid build) or downloaded as a Play Asset Delivery pack (Play build) per ADR-015, and the app MUST show the AV1-only state clearly when they are absent (`"HEVC decoding requires the codec pack"` with an install action).
- Acceptance: Given the Play build without the codec pack, when `corpus/syn_h265_gop60_1080p` is opened, then the container and syntax panels populate and the picture region shows the codec-pack prompt; after installing the pack, then playback works without restart.

**FR-AND-017** (Phase A/1) — Screenshots and frame-grab exports on Android MUST be SDR tone-mapped PNGs (the platform screenshot path is SDR) and the export dialog MUST state this when the content is HDR, offering the 16-bit PNG / raw plane export of FR-PLN-010 as the HDR-preserving alternative.
- Acceptance: Given `corpus/syn_h265_main10_pq_hdr` on the Pixel Tablet, when the tester chooses Export → Frame grab, then the dialog shows the note `PNG is tone-mapped; use 16-bit PNG or raw planes to keep HDR values` and both options produce files in the chosen `content://` directory.

**FR-AND-018** (Phase A/0) — The app MUST be publishable on Google Play (AAB, target API 36) and F-Droid (reproducible build from `android/` with `fdroid build`), and the F-Droid build MUST contain no proprietary dependency (no Play Services, no Firebase).
- Acceptance: Given the release CI, when `./gradlew bundleRelease` and `fdroid build org.openvideotools.studio` run, then both succeed and `tools/check_fdroid_deps.py` reports zero non-free dependencies.

---

## 4.13 MAC — macOS

**Rationale.** macOS is the first development platform (chapter 0 §0.0) and the platform on which HDR output is verified first (phase 1 exit criterion). ADR-005 (libplacebo via MoltenVK on `CAMetalLayer`), ADR-016 (notarised DMG + Homebrew cask, never Mac App Store) and Appendix B.4 (EDR API) drive these requirements.

**FR-MAC-001** (Phase 0) — `ovt-studio` MUST build and run as a universal binary (arm64 + x86_64) for macOS 13 (Ventura) and later from `cmake --preset macos-arm64-release` and `macos-x86_64-release` (merged by `lipo` into the universal DMG build when `OVT_UNIVERSAL=ON`, chapter 13 §13.5.1), with the Qt 6 LTS runtime bundled in `ovt-studio.app/Contents/Frameworks`.
- Acceptance: Given the release job, when `lipo -archs ovt-studio.app/Contents/MacOS/ovt-studio` runs, then it prints `x86_64 arm64`, and the app launches on a macOS 13 Intel Mac and a macOS 15 Apple Silicon Mac.

**FR-MAC-002** (Phase 1) — HDR output MUST use a `CAMetalLayer` with `wantsExtendedDynamicRangeContent = true`, pixel format `MTLPixelFormatRGBA16Float` and colour space `kCGColorSpaceExtendedLinearSRGB` (or `bgr10a2Unorm` with `kCGColorSpaceITUR_2100_PQ` as a selectable alternative), MUST read the headroom from `NSScreen.maximumExtendedDynamicRangeColorComponentValue` on every `NSWindowDidChangeScreenNotification`, and MUST pass it to `DisplayTarget.headroom`.
- Acceptance: Given `ovt-studio --dump-swapchain` on a MacBook Pro XDR, when the window is moved to an external SDR monitor and back, then the log shows `headroom=1.0` then `headroom≥4.0` and the FR-HDR-007 badge toggles accordingly (`hw-test:MacEdrScreenChange`).

**FR-MAC-003** (Phase 1) — Static HDR metadata MUST be set on the layer via `CAMetalLayer.edrMetadata` (`CAEDRMetadata.hdr10(minLuminance:maxLuminance:opticalOutputScale:)` from MDCV/CLL, or `.hlg`) when PQ/HLG content is displayed and cleared for SDR content.
- Acceptance: Given `corpus/syn_h265_main10_pq_hdr` then `corpus/syn_h264_gop60_1080p` opened in turn, when `ovt-studio --dump-swapchain` logs, then `edrMetadata=hdr10(...)` appears for the first and `edrMetadata=nil` for the second.

**FR-MAC-004** (Phase 1) — Playback decoding MUST use VideoToolbox (`h264_videotoolbox`, `hevc_videotoolbox`; AV1 is decoded in software by `libdav1d` at the FFmpeg 7.1 pin, which has no AV1 VideoToolbox hwaccel — D30; the `av1_videotoolbox` hwaccel of FFmpeg ≥ 8.0 on M3+ is an upgrade-path item, chapter 7 §7.6.3) via libavcodec hwaccel with zero-copy `CVPixelBuffer` → Metal texture import for display, and CPU download only when a plane view or analysis needs samples.
- Acceptance: Given `corpus/syn_h265_4k_main10`, when `ovt-studio --measure-render 300` plays at 1×, then dropped frames = 0 and `Activity Monitor`-equivalent CPU (`ps -o %cpu`) stays below 60 % of one core on the M1 reference.

**FR-MAC-005** (Phase 0) — The DMG MUST be signed with a Developer ID Application certificate, Hardened Runtime enabled with the entitlements `com.apple.security.cs.disable-library-validation` (for decoder plugins) only, notarised with `xcrun notarytool submit --wait`, and stapled with `xcrun stapler staple`; Gatekeeper MUST accept it on first launch without a right-click override.
- Acceptance: Given the release DMG on a clean macOS 15 VM, when `spctl --assess --type open --context context:primary-signature -v ovt-studio.dmg` and `spctl --assess --type execute -v ovt-studio.app` run, then both print `accepted` and `source=Notarized Developer ID`.

**FR-MAC-006** (Phase 0) — Decoder plugins loaded at runtime (`.dylib` in `ovt-studio.app/Contents/PlugIns/ovt/` or `~/Library/Application Support/ovt/plugins/`) MUST be signed with the same Developer ID or MUST be accepted under the disable-library-validation entitlement, and the app MUST refuse to load an unsigned plugin unless the user enables Settings → Advanced → Allow unsigned plugins.
- Acceptance: Given an unsigned `libovt_plugin_stub.dylib` in the user plugin directory, when `ovt-studio` starts, then the Plugins panel lists it as `blocked (unsigned)`; when the setting is enabled, then it loads.

**FR-MAC-007** (Phase 0) — A Homebrew cask `ovt-studio` and a formula `ovt` (CLI + Python module, built from source) MUST be published from the release job, and `brew install --cask ovt-studio && brew install ovt` MUST install both without conflicts.
- Acceptance: Given a clean Homebrew installation, when the two commands run, then `ovt --version` and `open -a ovt-studio` succeed.

**FR-MAC-008** (Phase 1) — `ovt-studio` MUST register as a handler for `.mp4 .mov .mkv .webm .ivf .264 .h264 .265 .hevc .obu .y4m .yuv` via `CFBundleDocumentTypes`, MUST support Finder "Open With" and drag-to-dock-icon, and MUST respect the macOS menu bar conventions (app menu, `Cmd+,` preferences, `Cmd+O`, `Cmd+W`, full-screen `Ctrl+Cmd+F` in addition to `F`).
- Acceptance: Given `open -a ovt-studio corpus/syn_h264_gop60_1080p`, when it runs, then the file opens; when `Cmd+,` is pressed, then Settings opens.

**FR-MAC-009** (Phase 1) — Colour-managed SDR output MUST tag the layer with `kCGColorSpaceSRGB` or `displayP3` per the content and let the system compositor manage the display profile, and OVT MUST NOT apply its own display ICC conversion on macOS.
- Acceptance: Given `corpus/syn_metrics_1080p_pair` (reference) (BT.709) on an SDR display, when a 50 % grey patch is grabbed with the Digital Color Meter in "Display native" mode, then the measured value equals the value measured for the same patch rendered by QuickTime Player ± 2/255.

---

## 4.14 LNX — Linux desktop

**Rationale.** Linux is a primary platform (chapter 0 §0.0) with HDR only via Wayland `color-management-v1` (Appendix B.4); ADR-016 fixes distro packages plus Flathub. These requirements fix the supported stack, HDR expectations by compositor, and packaging.

**FR-LNX-001** (Phase 0) — `ovt-studio` and `ovt` MUST build and run on x86_64 and arm64 with glibc ≥ 2.35 (Ubuntu 22.04 baseline), on both Wayland (`QT_QPA_PLATFORM=wayland`) and X11 (`xcb`), using Vulkan ≥ 1.2 for `VideoSurface` with a GLES fallback when no Vulkan ICD is present.
- Acceptance: Given the CI `build-linux-x64` job on Ubuntu 22.04, when `ctest --preset linux-x64-release` and `ovt-studio --dump-renderer` run under `xvfb-run` (X11) and `weston --backend=headless` (Wayland), then tests pass and the renderer log shows `backend=vulkan` or `backend=gles(fallback)`.

**FR-LNX-002** (Phase 4) — On Wayland compositors supporting `wp_color_management_v1` (KDE Plasma ≥ 6.0, GNOME ≥ 48, Sway ≥ 1.12) with Mesa ≥ 25.1, `VideoSurface` MUST create a swapchain with `VK_COLOR_SPACE_HDR10_ST2084_EXT` (or `EXTENDED_SRGB_LINEAR_EXT`) and attach `VK_EXT_hdr_metadata`, and when the extension or colour space is absent it MUST fall back to tone-mapped SDR with the FR-HDR-007 badge.
- Acceptance: Given KDE Plasma 6.3 with HDR enabled on an HDR monitor, when `ovt-studio --dump-swapchain corpus/syn_h265_main10_pq_hdr` runs, then the log shows `colorspace=HDR10_ST2084 hdr_metadata=sent`; given GNOME on X11, then it shows `colorspace=SRGB_NONLINEAR tonemap=BT2390` and the badge is visible.

**FR-LNX-003** (Phase 1) — Playback decoding on Linux MUST use VA-API (`h264_vaapi`, `hevc_vaapi`, `av1_vaapi`) through libavcodec when `/dev/dri/renderD*` is available, with DMA-BUF import into Vulkan for zero-copy display, and MUST fall back to software otherwise.
- Acceptance: Given an Intel or AMD GPU with Mesa VA-API drivers, when `ovt dump --hw --frames 10 --md5 corpus/syn_h265_gop60_1080p` runs, then the log shows `decoder: vaapi` and the MD5 equals the software MD5; given `LIBVA_DRIVER_NAME=none`, then the log shows the fallback.

**FR-LNX-004** (Phase 0) — Packages MUST be produced for Flathub (`org.openvideotools.Studio` manifest, runtime `org.kde.Platform` 6.x, with `--filesystem=host:ro` and `--device=dri` permissions only), Debian/Ubuntu (`.deb` for `ovt`, `ovt-studio`, `libovt-core`, `python3-ovt`), and Fedora (`.rpm` with AVC/HEVC decoder plugins in a separate `ovt-plugins-freeworld` package per ADR-015), from the release job.
- Acceptance: Given the release job, when `flatpak install --user ovt-studio.flatpak` and `flatpak run org.openvideotools.Studio --version` run, then the version matches the tag; when `lintian ovt_*.deb` runs, then it reports no errors.

**FR-LNX-005** (Phase 1) — The application MUST install a `.desktop` file with `MimeType=video/mp4;video/x-matroska;video/webm;video/x-ivf;video/h264;video/h265;…`, an AppStream metainfo file passing `appstreamcli validate`, and scalable icons, so that file managers offer "Open with ovt-studio".
- Acceptance: Given the installed `.deb`, when `appstreamcli validate /usr/share/metainfo/org.openvideotools.Studio.metainfo.xml` and `desktop-file-validate /usr/share/applications/org.openvideotools.Studio.desktop` run, then both exit 0.

**FR-LNX-006** (Phase 1) — HiDPI MUST be supported with fractional scaling on Wayland (`wp_fractional_scale_v1` via Qt) and `QT_SCALE_FACTOR` on X11, and the picture region MUST map one video sample to one physical pixel at 1× zoom regardless of UI scale.
- Acceptance: Given a Wayland session at 150 % scale, when `corpus/syn_h264_gop60_1080p` is shown at 1× zoom, then a 1920-sample-wide frame spans exactly 1920 physical pixels in a screenshot taken with `grim`.

**FR-LNX-007** (Phase 1) — The CLI MUST run on headless servers without a GPU or display (no Qt, no Vulkan, no `DISPLAY`) for every subcommand except those that render (`decode --png` uses the CPU zimg path), so that CI pipelines can use `ovt` in containers.
- Acceptance: Given the `ubuntu:22.04` Docker image with only `ovt` and its runtime libraries, when `ovt metrics --metric psnr,vmaf …` and `ovt dump --png --frames 0` run, then both exit 0 and `ldd $(which ovt)` lists no `libQt6*`, `libvulkan*` or `libX11*`.

---

## 4.15 NFR — Non-functional requirements

**Rationale.** Backbone chapter 0 §0.7 lists the cross-cutting rules (robustness, threading, memory, performance, determinism, diagnostics, accessibility, security, exports) and ADR-002/ADR-016 the licensing and governance constraints. Each becomes a numbered, testable NFR here. Performance numbers are those of chapter 0 §0.7.4 on the M1 reference; CI measures them on the macOS arm64 runner with a stored baseline and a 20 % tolerance, and a regression beyond the tolerance fails the build.

### 4.15.1 Performance

**NFR-001** (Phase 1) — Header-only indexing (`StreamIndex`) of a 1-hour 1080p H.264 file MUST complete in ≤ 2 s on the M1 reference.
- Acceptance: Given `corpus/syn_h264_1h_1080p`, when `ovt index --time --no-cache` runs, then `index_ms ≤ 2000` (`test:PerfIndexH264`).

**NFR-002** (Phase 1) — The first frame MUST be displayed ≤ 500 ms after the open command for a 1080p stream.
- Acceptance: Given `corpus/syn_h264_1h_1080p`, when `ovt-studio --open <file> --measure-first-frame` runs, then `first_frame_ms ≤ 500`.

**NFR-003** (Phase 2) — Overlay redraw at 4K with MV arrows on every 4×4 cell MUST take ≤ 16 ms (95th percentile, as measured by `bench_overlay_*`; chapter 16 §16.6).
- Acceptance: Given `corpus/syn_av1_4k_2min` with F2 active, when `ovt-studio --measure-render 300` runs, then `p95_ms ≤ 16`.

**NFR-004** (Phase 2) — Block analysis of one 1080p frame from the nearest RAP in a GOP of ≤ 60 frames MUST take ≤ 1.5 s with `ovt-plugin-de265` and ≤ 3 s with `ovt-plugin-aom`.
- Acceptance: Given `corpus/syn_h265_gop60_1080p` and `corpus/syn_av1_gop60_1080p`, when `ovt analyze --frames 59 --time --no-cache` runs, then `analysis_ms ≤ 1500` and `≤ 3000` respectively.

**NFR-005** (Phase 3) — PSNR + SSIM MUST run at ≥ 200 fps and VMAF at ≥ 60 fps for 1080p on 4 threads (chapter 14 §14.9.3 is the single source of these budgets, D19).
- Acceptance: As FR-VQ-015 (`test:PerfMetrics`).

**NFR-006** (Phase 1) — Seeking to any display index in a stream with GOP length ≤ 250 frames MUST present the frame within 400 ms with hardware decoding and within 1.5 s with software decoding at 1080p.
- Acceptance: Given `corpus/syn_h264_1h_1080p`, when `ovt dump --seek 43 210 --frames 1 --time` runs with `--hw` and `--sw`, then `seek_ms ≤ 400` and `≤ 1500`.

**NFR-007** (Phase 1) — Cancellation of any job MUST be observed within 50 ms of `ovt_job_cancel` (chapter 0 §0.7.2).
- Acceptance: Given `test:JobCancelLatency` over index, analysis, decode and metrics jobs, when each is cancelled, then the return occurs within 50 ms (+ 100 ms slack on CI).

**NFR-008** (Phase 1) — Cold start of `ovt-studio` to an interactive empty window MUST take ≤ 1.5 s on the M1 reference and ≤ 3 s on the Pixel Tablet.
- Acceptance: Given a warm disk cache, when `ovt-studio --measure-startup` runs, then `startup_ms ≤ 1500` (macOS) and `≤ 3000` (Android, `adb shell am start -W` `TotalTime`).

### 4.15.2 Memory

**NFR-009** (Phase 1) — Peak RSS per open stream MUST be ≤ 2 GB on desktop at 4K and ≤ 1 GB PSS on Android (chapter 0 §0.7.3).
- Acceptance: As FR-GUI-029 and FR-AND-013.

**NFR-010** (Phase 1) — The on-disk cache MUST be bounded by a user-settable size (default 20 GiB desktop, 2 GiB Android — chapter 0 §0.7.3; restated in chapter 6 §6.7.4 and chapter 12 `ovt_cache_set_max_bytes`) with LRU eviction by file, MUST be zstd-compressed (level 3), and MUST be invalidated when `source_hash` changes.
- Acceptance: Given a cache limit of 100 MB, when 20 streams of 10 MB cache each are indexed, then the cache directory stays ≤ 100 MB and the oldest entries are gone (`test:CacheEviction`); when the source file is modified, then the next open reports `cache: miss (hash changed)`.

**NFR-011** (Phase 1) — The library MUST handle allocation failure by returning `OVT_E_NOMEM` from the API call in progress and MUST leave all handles in a usable state.
- Acceptance: Given `test:OomInjection` with a failing allocator after N allocations for N in 1…1000, when the open/index sequence runs, then every call returns `OVT_OK` or `OVT_E_NOMEM` and no crash or leak occurs.

### 4.15.3 Robustness

**NFR-012** (Phase 0) — Every parser (container, syntax, plugin input) MUST have a libFuzzer target under `core/tests/fuzz/` and CI MUST run each for ≥ 60 s per pull request (`fuzz-smoke`) and ≥ 20 min per target nightly (`fuzz-nightly`, chapter 13 §13.7.1) with ASan/UBSan, with any crash blocking the merge.
- Acceptance: Given the CI `fuzz-smoke` job, when it runs, then the targets `ovt_fuzz_{isobmff,annexb,obu,h264_headers,h265_headers,av1_headers,y4m,ivf}` (chapter 13 §13.2.1 naming; sources `core/tests/fuzz/fuzz_<parser>.cpp`) each execute ≥ 60 s with zero crashes.

**NFR-013** (Phase 1) — No input file MAY cause the process to crash, hang (> 30 s without progress) or consume unbounded memory; corrupt input MUST produce `OVT_E_CORRUPT` with at least one `Diagnostic`.
- Acceptance: Given `core/tests/fuzz/corpus/*` and `corpus/syn_broken_*` and `corpus/syn_mp4_broken_*` (≥ 500 files), when `ovt probe`, `ovt index`, `ovt analyze --frames 0` run on each under `timeout 30` with a 4 GB `ulimit -v`, then every exit code is in {0, 3, 4} and no timeout occurs.

**NFR-014** (Phase 2) — A plugin returning `OVT_E_CORRUPT` or `OVT_E_PLUGIN` MUST NOT invalidate the `StreamIndex`, the container tree or other frames' cached `FrameAnalysis`, and the GUI MUST show the error in the Diagnostics panel while remaining interactive.
- Acceptance: Given `corpus/syn_broken_h265_slice_error_f12`, when frame 12 is analysed, then the Diagnostics panel shows the error, frame 11's analysis is still displayable, and the Syntax panel is unchanged.

**NFR-015** (Phase 1) — All tests MUST pass under AddressSanitizer, UndefinedBehaviorSanitizer and (Linux) ThreadSanitizer builds (`OVT_ENABLE_SANITIZERS=ON`).
- Acceptance: Given CI jobs `asan-ubsan` and `tsan`, when `ctest` runs, then all tests pass with zero sanitizer reports.

**NFR-016** (Phase 1) — No raw pointer arithmetic on input data MAY exist outside `ovt::BitReader` and `ovt::io::RandomAccessFile` (chapter 0 §0.7.1).
- Acceptance: Given the clang-tidy configuration `.clang-tidy` with the custom check `ovt-no-raw-input-arith` (`tools/clang-tidy/`), when `run-clang-tidy core/src` runs, then zero violations are reported.

### 4.15.4 Determinism

**NFR-017** (Phase 1) — Identical input MUST yield byte-identical FlatBuffers (`StreamIndex`, `ContainerTree`, `FrameAnalysis`, `MetricsResult`) across runs, thread counts and platforms, with no timestamps, pointers or absolute paths inside the records (chapter 0 §0.7.5).
- Acceptance: Given `corpus/syn_{h264,h265,av1}_gop60_1080p`, when `ovt analyze --frames 0-7 --flatbuffers` runs with `--threads 1` and `--threads 8` on macOS and Linux, then all SHA-256 sums are equal (`test:DeterminismThreads`).

**NFR-018** (Phase 3) — Metric results MUST be reproducible to the last printed digit across runs and thread counts, and the engine version (`MetricsResult.engine`) MUST be recorded so that differences between library versions are attributable.
- Acceptance: Given the Netflix vectors, when `ovt metrics --metric vmaf,psnr,ssim --threads 1` and `--threads 8` run, then the JSON outputs are byte-identical except the `engine.threads` field.

### 4.15.5 Security

**NFR-019** (Phase 0) — The build MUST enable stack protector, `_FORTIFY_SOURCE=2`, PIE, full RELRO (Linux), and the macOS Hardened Runtime, and MUST NOT disable them for any target.
- Acceptance: Given the release binaries, when `checksec --file=ovt` (Linux) and `codesign -d --entitlements - ovt-studio.app` (macOS) run, then `checksec` reports `Full RELRO, Canary found, NX enabled, PIE enabled` and the entitlements contain only `disable-library-validation`.

**NFR-020** (Phase 4) — Sidecar metric processes MUST run with a restricted environment (FR-VQ-019), MUST NOT inherit file descriptors other than stdin/stdout/stderr and the shared-memory handle, and MUST be terminated when the parent exits.
- Acceptance: Given the reference sidecar, when `ovt metrics --metric psnr_sidecar` runs and the parent is killed with SIGKILL, then the child exits within 5 s (`test:SidecarLifetime`).

**NFR-021** (Phase 1) — OVT MUST NOT make any network connection except: DASH/HLS manifest and segment fetches explicitly requested by the user (phase 4), and the UVQ model download after explicit confirmation (FR-VQ-010); there is no update check (updates come through the distribution channel: Flathub, Homebrew, Play, F-Droid).
- Acceptance: Given a fresh install, when `ovt-studio` runs for 60 s with `corpus/syn_h264_gop60_1080p` under a network monitor (`nettop` / `tcpdump`), then zero outbound connections are recorded.

**NFR-022** (Phase 1) — Dependency versions MUST be pinned by exact tag or commit in `third_party/VERSIONS.md` and `vcpkg.json` (`builtin-baseline`), and CI MUST fail when a dependency's recorded SHA-256 differs from the downloaded archive.
- Acceptance: Given `third_party/VERSIONS.md`, when `tools/check_pins.py` runs in CI, then every entry has a version and a SHA-256 and every ExternalProject `URL_HASH` matches.

### 4.15.6 Accessibility and internationalisation

**NFR-023** (Phase 1) — All GUI text MUST meet WCAG 2.1 AA contrast (≥ 4.5:1; ≥ 3:1 for ≥ 24 px text) in both themes, all controls MUST be keyboard-reachable, and the Qt Accessibility bridge MUST expose panel names and tree items to VoiceOver (macOS), TalkBack (Android) and Orca (Linux).
- Acceptance: Given `ovt-studio` on macOS with VoiceOver enabled, when the tester focuses the Syntax panel, then VoiceOver reads `Syntax, tree` and the focused node's name; `tools/check_contrast.py` reports zero failures.

**NFR-024** (Phase 2 — the first data colour maps are the phase-2 overlays, WI-2.439; re-phased, chapter 17 §17.4 OI-15) — All colour maps used for data MUST include at least one colour-blind-safe option (`viridis`, `cividis`) and the frame-type colour language MUST have a shape-coded alternative (FR-GUI-020).
- Acceptance: As FR-GUI-019 and FR-GUI-020.

**NFR-025** (Phase 1) — Every user-visible string in the GUI MUST be translatable (`qsTr`), the CLI MUST emit English only, and dates/numbers in the GUI MUST follow the selected locale while exports follow FR-CLI-010.
- Acceptance: As FR-GUI-023 and FR-CLI-010.

### 4.15.7 Maintainability and upstream-first

**NFR-026** (Phase 0) — Every patch to a third-party decoder MUST (a) live in `plugins/<plugin>/patches/` or `third_party/patches/`, (b) be ≤ 500 changed lines per patch and ≤ 2 000 changed lines in total per decoder, (c) MUST NOT change decoding results: only read-only accessors, side-data export or copies of intermediate buffers, each guarded by the build flag, (d) be behind a build flag, and (e) have an upstream submission (PR/mailing-list URL) recorded in `third_party/UPSTREAMING.md` before the work item is closed.
- Acceptance: Given the repository, when `tools/check_patches.py` runs, then every `*.patch` has ≤ 500 changed lines, no decoder's patches exceed 2 000 lines in total, and each has a matching row in `UPSTREAMING.md` with a URL.

**NFR-027** (Phase 0) — Public interfaces (C API, plugin ABIs, FlatBuffers schemas, CLI grammar) MUST be documented in `docs/spec/` and generated artefacts (JSON Schemas, API reference) MUST be rebuilt and diffed in CI so that undocumented changes fail the build.
- Acceptance: Given a PR that changes `core/schema/ovt_model.fbs`, when CI runs, then the job `docs-generated` fails unless `docs/spec/schemas/*.schema.json` was regenerated in the same PR.

**NFR-028** (Phase 0) — Unit-test line coverage of `core/src/` MUST be ≥ 80 % overall and ≥ 90 % for `bitreader/`, `syntax/` and `container/`, measured by `llvm-cov` in CI.
- Acceptance: Given the `coverage` CI job, when it runs, then the reported percentages meet the thresholds or the job fails.

**NFR-029** (Phase 0) — Code MUST be formatted by `clang-format` (`.clang-format` at the root) and `qmlformat`, and CI MUST reject unformatted changes.
- Acceptance: Given a PR, when `tools/check_format.sh` runs, then it exits 0 only if `git diff --exit-code` after formatting is empty.

**NFR-030** (Phase 0) — The repository layout MUST match chapter 0 §0.4 exactly (with `cmake/` at the top level and repo-rooted paths, D14), and every module in §0.3 MUST correspond to one directory `core/src/<module>/` with its own `CMakeLists.txt` and `tests/`.
- Acceptance: Given the repository, when `tools/check_layout.py` compares the tree against `docs/spec/layout.txt`, then zero missing or extra top-level entries are reported.

### 4.15.8 Licensing gate

**NFR-031** (Phase 0) — CI MUST run a licence gate that scans every dependency (vcpkg manifest, `third_party/`, Gradle, Python) and fails on any component licensed AGPL, SSPL, BUSL, Commons Clause or non-commercial, and `THIRD_PARTY_NOTICES.md` (repository root, generated) MUST be regenerated on every build.
- Acceptance: Given the `license-gate` job with `tools/license_gate.py`, when a test dependency with `license: AGPL-3.0` is added to `vcpkg.json` on a branch, then the job fails naming the dependency; on `main`, then it passes and `THIRD_PARTY_NOTICES.md` lists every dependency with its SPDX id.

**NFR-032** (Phase 0) — Each source file MUST carry an SPDX licence header matching its directory's licence per ADR-002 (`core/`, `render/` LGPL-2.1-or-later; `core/include/ovt/abi/` BSD-2-Clause; `app/`, `cli/`, `android/`, `python/` GPL-3.0-or-later; plugins per their own `LICENSE`).
- Acceptance: Given the repository, when `reuse lint` runs, then it reports compliance with the REUSE specification.

**NFR-033** (Phase 0) — Official builds MUST honour ADR-015: AVC/HEVC *analysis* decoder components MUST be separate runtime-loaded libraries (the core FFmpeg flavour's playback decoders follow `OVT_PATENT_CODECS_IN_CORE`, ON by default since the owner decision of 2026-11-12), and a build with `OVT_BUILD_PLUGIN_FFMPEG=OFF` (chapter 13 §13.2.2; the GPL flavour is selected by `OVT_FFMPEG_GPL`/`OVT_FFMPEG_GPL_PLUGIN_ONLY`) MUST still produce a fully functional AV1 product with the AOM PATENTS notice in the About dialog.
- Acceptance: Given a build with the option off, when `ovt plugins --format json` runs, then only `aom`, `dav1d` and `vmaf` components are listed and `ovt analyze corpus/syn_av1_gop60_1080p --frames 0` succeeds while `ovt dump corpus/syn_h264_gop60_1080p` exits 4 with `DEC-PLUGIN-0001 decoder plugin not installed (h264)` (chapter 7 §7.9.4).

### 4.15.9 Documentation

**NFR-034** (Phase 0) — The specification (`docs/spec/*.md`), ADRs (`docs/adr/*.md`) and developer guides (`docs/dev/*.md`: building, threading, adding a codec, adding a metric, adding a plugin) MUST be kept in the repository and built into a static site (`mkdocs`) by CI, with broken intra-doc links failing the build.
- Acceptance: Given the `docs` CI job, when `mkdocs build --strict` runs, then it exits 0.

**NFR-035** (Phase 1) — Every CLI subcommand MUST have a man page generated from the same declarative command table as `--help`, and every GUI panel MUST have a Help → Panel help entry linking to its section in `docs/spec/11-gui.md`.
- Acceptance: Given the build, when `man ovt-metrics` runs (Linux/macOS), then the flags listed equal those printed by `ovt metrics --help`; when Help → Panel help is chosen with the Syntax panel active, then the browser opens `…/11-gui.md#syntax-panel`.

**NFR-036** (Phase 1) — Every diagnostic code MUST be documented in `docs/spec/diagnostic-codes.md` with meaning, spec reference and user action, and CI MUST fail when a code is emitted in source but missing from the document.
- Acceptance: Given the repository, when `tools/check_diag_codes.py` runs, then every string matching `[A-Z]{2,3}(-[A-Z0-9]{2,8})?-[0-9]{4}` in `core/src/**` appears in the document.

---

## 4.16 Traceability

Each FR area is specified in detail by one primary chapter (chapter 0 §0.10). Requirement counts include the `1nn` ids (added after the first draft). The "also" column names chapters that contribute acceptance infrastructure or platform detail.

| FR area | Count | Primary chapter | Also |
|---|---|---|---|
| GEN | 20 | `05-architecture.md` | `13-platforms-build-ci.md`, `15-licensing-and-governance.md` |
| CNT | 24 | `08-container.md` | `06-data-model.md`, `14-testing.md` |
| SYN | 20 | `07-bitstream-and-decoders.md` | `06-data-model.md`, `14-testing.md` |
| DEC | 14 | `07-bitstream-and-decoders.md` | `05-architecture.md`, `10-rendering-and-hdr.md` |
| BLK | 24 | `07-bitstream-and-decoders.md` (plugins), `06-data-model.md` (extension tables) | `10-rendering-and-hdr.md` (overlays), `11-gui.md` |
| PLN | 12 | `10-rendering-and-hdr.md` | `11-gui.md`, `09-metrics.md` (compare, first-diff) |
| HDR | 15 | `10-rendering-and-hdr.md` | `08-container.md`, `07-bitstream-and-decoders.md` (metadata), `09-metrics.md` (domains) |
| VQ | 25 | `09-metrics.md` | `06-data-model.md`, `12-cli-and-api.md`, `14-testing.md` |
| GUI | 35 | `11-gui.md` | `10-rendering-and-hdr.md`, `03-competitive-reference.md` (UX-1…12) |
| CLI | 25 | `12-cli-and-api.md` | `06-data-model.md` (JSON schemas) |
| API | 16 | `12-cli-and-api.md` | `05-architecture.md`, `07-bitstream-and-decoders.md` (plugin ABI) |
| AND | 18 | `13-platforms-build-ci.md`, `11-gui.md` (adaptations) | `10-rendering-and-hdr.md` (SurfaceView path), `15-licensing-and-governance.md` (Play/F-Droid) |
| MAC | 9 | `13-platforms-build-ci.md` | `10-rendering-and-hdr.md` (EDR), `15-licensing-and-governance.md` (notarisation, Homebrew) |
| LNX | 7 | `13-platforms-build-ci.md` | `10-rendering-and-hdr.md` (Wayland CM) |
| NFR | 36 | `14-testing.md` (performance, fuzz, determinism), `05-architecture.md` (threading, memory), `15-licensing-and-governance.md` (gate) | `13-platforms-build-ci.md` |

**Coverage of chapter 3 §3.4 must-have rows** (editor check; every must-have row of §3.4.1/§3.4.2 is realised by at least one FR):

| Row | Primary FRs |
|---|---|
| A1 syntax tree + hex/bit linkage | FR-SYN-001…020, FR-GUI-014, FR-CNT-002 |
| A2 partition overlay, click-to-select, Selection Info | FR-BLK-019, FR-GUI-009, FR-GUI-010 |
| A3 prediction overlay (intra directions, L0/L1, skip/merge) | FR-BLK-017, FR-BLK-022 |
| A4 QP map and bits heat map | FR-BLK-015, FR-BLK-017, FR-GUI-008 |
| A5 per-stage pictures / residual view | FR-BLK-014, FR-PLN-005 |
| A6 reference structure, filmstrip, DPB | FR-DEC-011, FR-GUI-017, FR-GUI-018 |
| A7 frame-size timeline with type colours and QP plot | FR-GUI-003, FR-GUI-005, FR-GUI-020 |
| A8 conformance error list with jump-to-location | FR-GEN-006, FR-GUI-012, FR-GUI-026 |
| A9 debug YUV, first-mismatch finder, per-block PSNR overlay | FR-PLN-009, FR-CLI-016, FR-VQ-008 |
| A10 CLI mirroring the GUI | FR-CLI-007, FR-DEC-006…008 |
| A11 statistics normalised by area/bits with export | FR-BLK-020, FR-BLK-021, FR-GUI-031 |
| A12 codec coverage AVC/HEVC Main-Main10/AV1 now, others via `Codec` enum + ABI | FR-GEN-003, FR-GEN-012, FR-DEC-012, FR-API-009 |
| B1 PSNR/SSIM/MS-SSIM/VMAF with timeline | FR-VQ-001…011, FR-VQ-016 |
| B2 side-by-side / split / difference views, pixel read-outs | FR-PLN-004, FR-PLN-007 |
| B3 reference YUV/Y4M ingestion with format dialog | FR-PLN-008 |
| B4 automatic alignment and scaling | FR-VQ-002, FR-VQ-003 |
| B5 ROI-restricted metrics | FR-VQ-013 |
| B6 CSV/JSON export with percentiles | FR-VQ-012, FR-VQ-014 |
| B7 CLI batch `--reference/--distorted` | FR-CLI-009, FR-VQ-007 |
| B8 HDR metadata display and correct PQ/HLG preview | FR-HDR-001…005, FR-HDR-007, FR-HDR-011, FR-GUI-016 (MaxCLL/MaxFALL measurement FR-HDR-006 is phase 3) |

No uncovered must-have row was found. Chapter 3 §3.4 now carries the FR ids per row (column "FR ids").

Total: 264 functional requirements (228 original + 36 added after the first draft: `FR-<AREA>-1nn`) and 36 non-functional requirements (the `NFR-41x`/`NFR-45x` ids used by earlier drafts of chapter 10 are retired and were never part of this count). §4.17 lists the work items per requirement; `tools/check_wi_refs.py --table` (chapter 13 WI-0.007) MUST report every requirement ID that no work item claims and every work item that claims no requirement.

---

## 4.17 FR → WI traceability

One row per requirement. **WIs** lists the chapter-16 work items that realise it (from the "Requirements realised by this chapter" tables of chapters 08–15, the FR citations inside work item blocks, and editor assignment by section where a chapter has no such table — chapters 07, 11, 13 and 14). **Priority** is derived, not independently authored: `must` when the requirement is in a phase 0, 1 or A, or realises a must-have row of chapter 3 §3.4; `should` for phases 2–3; `nice` for phase 4. Two requirements have no work item (**GAP**) and are listed in chapter 17 §17.4 (OI-07, OI-08); their chapter 3 §3.3/§3.4 rows carry the flag "deferred". Chapter 16 §16.4 carries the inverse mapping (the `FRs` column per work item) and is **derived from this table** (`tools/check_wi_refs.py --table`, chapter 13 WI-0.007, MUST report zero inverse mismatches, zero rows citing a superseded WI, and — for every requirement — at least one realising WI whose scheduled phase is not later than the requirement's phase; prohibitions such as NFR-021 list the WI whose acceptance test observes them). Rows whose phase differs from the WI phase digit follow the scheduled phase of chapter 16 §16.4 (§16.3.11/§16.3.12 re-phasing). Deliberate deferrals (FR-HDR-006 → 3, FR-HDR-012/013 → 4, NFR-024 → 2) are recorded in chapter 17 §17.4 OI-15.

| Area | Requirement | Phase | Priority | WIs |
|---|---|---|---|---|
| GEN | FR-GEN-001 | 0 | must | WI-0.000, WI-1.511 |
| GEN | FR-GEN-002 | 0 | must | WI-0.000 |
| GEN | FR-GEN-003 | 0 | must | WI-0.080, WI-1.106 |
| GEN | FR-GEN-004 | 1 | must | WI-1.200 |
| GEN | FR-GEN-005 | 1 | must | WI-1.217, WI-1.232 |
| GEN | FR-GEN-006 | 1 | must | WI-1.224, WI-1.453, WI-1.500 |
| GEN | FR-GEN-007 | 1 | must | WI-0.557, WI-2.150 |
| GEN | FR-GEN-008 | 0 | must | WI-0.001, WI-0.030, WI-0.036 |
| GEN | FR-GEN-009 | 1 | must | WI-0.085, WI-1.510 |
| GEN | FR-GEN-010 | 1 | must | WI-0.000 |
| GEN | FR-GEN-011 | 1 | must | WI-1.100, WI-1.204 |
| GEN | FR-GEN-012 | 4 | must | WI-4.599 |
| GEN | FR-GEN-101 | 0 | must | WI-0.006 |
| GEN | FR-GEN-102 | 0 | must | WI-0.001 |
| GEN | FR-GEN-103 | 0 | must | WI-0.030, WI-0.560 |
| GEN | FR-GEN-104 | 0 | must | WI-0.036, WI-0.056 |
| GEN | FR-GEN-105 | 0 | must | WI-0.601 |
| GEN | FR-GEN-106 | 0 | must | WI-0.602 |
| GEN | FR-GEN-107 | 0 | must | WI-0.600 |
| GEN | FR-GEN-108 | 0 | must | WI-0.605 |
| CNT | FR-CNT-001 | 1 | must | WI-1.201 |
| CNT | FR-CNT-002 | 1 | must | WI-1.205, WI-1.206, WI-1.207, WI-1.208, WI-1.209, WI-1.210, WI-1.211, WI-1.212, WI-1.213, WI-1.214, WI-1.215, WI-1.216 |
| CNT | FR-CNT-003 | 1 | must | WI-1.203, WI-1.571 |
| CNT | FR-CNT-004 | 1 | must | WI-1.204 |
| CNT | FR-CNT-005 | 1 | must | WI-1.217 |
| CNT | FR-CNT-006 | 1 | must | WI-1.218, WI-1.219 |
| CNT | FR-CNT-007 | 1 | must | WI-1.231 |
| CNT | FR-CNT-008 | 1 | must | WI-1.213, WI-1.221 |
| CNT | FR-CNT-009 | 1 | must | WI-1.224 |
| CNT | FR-CNT-010 | 1 | must | WI-1.225 |
| CNT | FR-CNT-011 | 1 | must | WI-1.226 |
| CNT | FR-CNT-012 | 1 | must | WI-1.227 |
| CNT | FR-CNT-013 | 1 | must | WI-1.228 |
| CNT | FR-CNT-014 | 1 | must | WI-1.229 |
| CNT | FR-CNT-015 | 4 | nice | WI-4.200, WI-4.203 |
| CNT | FR-CNT-016 | 4 | nice | WI-4.204, WI-4.207 |
| CNT | FR-CNT-017 | 4 | nice | WI-4.208, WI-4.210 |
| CNT | FR-CNT-018 | 1 | must | WI-1.239 |
| CNT | FR-CNT-101 | 1 | must | WI-1.201, WI-1.224 |
| CNT | FR-CNT-102 | 1 | must | WI-1.223 |
| CNT | FR-CNT-103 | 1 | must | WI-1.232 |
| CNT | FR-CNT-104 | 1 | must | WI-1.234 |
| CNT | FR-CNT-105 | 1 | must | WI-1.229 |
| CNT | FR-CNT-106 | 1 | must | WI-1.238 |
| SYN | FR-SYN-001 | 0 | must | WI-1.102, WI-1.570 |
| SYN | FR-SYN-002 | 1 | must | WI-1.102, WI-1.103 |
| SYN | FR-SYN-003 | 1 | must | WI-1.104 |
| SYN | FR-SYN-004 | 1 | must | WI-1.108, WI-1.109, WI-1.110 |
| SYN | FR-SYN-005 | 1 | must | WI-1.112, WI-1.113, WI-1.114 |
| SYN | FR-SYN-006 | 1 | must | WI-1.116, WI-1.117, WI-1.118 |
| SYN | FR-SYN-007 | 1 | must | WI-1.106, WI-1.572 |
| SYN | FR-SYN-008 | 1 | must | WI-1.106, WI-1.572 |
| SYN | FR-SYN-009 | 1 | must | WI-1.111, WI-1.115, WI-1.119 |
| SYN | FR-SYN-010 | 1 | must | WI-1.111, WI-1.115, WI-1.119 |
| SYN | FR-SYN-011 | 1 | must | WI-1.110, WI-1.114, WI-1.116, WI-1.120 |
| SYN | FR-SYN-012 | 1 | must | WI-1.110, WI-1.114 |
| SYN | FR-SYN-013 | 1 | must | WI-1.102, WI-1.106, WI-0.086 |
| SYN | FR-SYN-014 | 1 | must | WI-1.130, WI-1.458 |
| SYN | FR-SYN-015 | 1 | must | WI-1.457 |
| SYN | FR-SYN-016 | 1 | must | WI-0.560, WI-1.125 |
| SYN | FR-SYN-017 | 2 | should | WI-1.457 |
| SYN | FR-SYN-018 | 1 | must | WI-1.508 |
| SYN | FR-SYN-019 | 4 | nice | WI-4.109, WI-4.543 |
| SYN | FR-SYN-020 | 4 | nice | WI-4.109, WI-4.543 |
| DEC | FR-DEC-001 | 1 | must | WI-1.121 |
| DEC | FR-DEC-002 | 1 | must | WI-1.111, WI-1.122 |
| DEC | FR-DEC-003 | 1 | must | WI-1.115, WI-1.123 |
| DEC | FR-DEC-004 | 1 | must | WI-1.119, WI-1.124 |
| DEC | FR-DEC-005 | 1 | must | WI-1.121 |
| DEC | FR-DEC-006 | 1 | must | WI-2.171 |
| DEC | FR-DEC-007 | 1 | must | WI-1.574, WI-2.171 |
| DEC | FR-DEC-008 | 1 | must | WI-1.507, WI-2.171 |
| DEC | FR-DEC-009 | 1 | must | WI-1.455, WI-1.456 |
| DEC | FR-DEC-010 | 2 | should | WI-2.151, WI-2.152 |
| DEC | FR-DEC-011 | 2 | must | WI-1.121, WI-2.484 |
| DEC | FR-DEC-012 | 4 | must | WI-4.104, WI-4.105, WI-4.106 |
| DEC | FR-DEC-013 | 4 | nice | WI-4.112 |
| DEC | FR-DEC-014 | 1 | must | WI-1.456, WI-2.171 |
| BLK | FR-BLK-001 | 2 | should | WI-2.150, WI-2.153 |
| BLK | FR-BLK-002 | 2 | should | WI-0.084, WI-0.086 |
| BLK | FR-BLK-003 | 2 | should | WI-0.084, WI-0.086 |
| BLK | FR-BLK-004 | 2 | should | WI-2.164, WI-2.165 |
| BLK | FR-BLK-005 | 2 | should | WI-2.164 |
| BLK | FR-BLK-006 | 2 | should | WI-2.156, WI-2.159 |
| BLK | FR-BLK-007 | 2 | should | WI-2.156 |
| BLK | FR-BLK-008 | 2 | should | WI-2.157 |
| BLK | FR-BLK-009 | 1 | must | WI-2.169, WI-2.170 |
| BLK | FR-BLK-010 | 4 | nice | WI-4.100, WI-4.101, WI-4.102 |
| BLK | FR-BLK-011 | 2 | should | WI-2.156, WI-2.164, WI-2.170 |
| BLK | FR-BLK-012 | 2 | should | WI-2.156, WI-2.164, WI-2.170 |
| BLK | FR-BLK-013 | 2 | should | WI-2.156, WI-2.164, WI-2.170 |
| BLK | FR-BLK-014 | 2 | must | WI-2.158, WI-2.166 |
| BLK | FR-BLK-015 | 2 | must | WI-2.157, WI-2.165 |
| BLK | FR-BLK-016 | 2 | should | WI-0.084, WI-2.151 |
| BLK | FR-BLK-017 | 2 | must | WI-2.430, WI-2.437 |
| BLK | FR-BLK-018 | 2 | should | WI-2.434, WI-2.437, WI-2.438 |
| BLK | FR-BLK-019 | 2 | must | WI-2.482 |
| BLK | FR-BLK-020 | 2 | must | WI-2.485 |
| BLK | FR-BLK-021 | 2 | must | WI-1.461, WI-2.485 |
| BLK | FR-BLK-022 | 2 | must | WI-0.088, WI-0.089, WI-2.521 |
| BLK | FR-BLK-023 | 2 | should | WI-2.166, WI-2.483 |
| BLK | FR-BLK-024 | 2 | should | WI-0.089, WI-2.521 |
| PLN | FR-PLN-001 | 1 | must | WI-1.408 |
| PLN | FR-PLN-002 | 1 | must | WI-1.401, WI-2.435 |
| PLN | FR-PLN-003 | 1 | must | WI-1.409, WI-2.433 |
| PLN | FR-PLN-004 | 1 | must | WI-1.456 |
| PLN | FR-PLN-005 | 2 | must | WI-1.408, WI-2.480 |
| PLN | FR-PLN-006 | 1 | must | WI-1.456, WI-4.452 |
| PLN | FR-PLN-007 | 2 | must | WI-1.410 |
| PLN | FR-PLN-008 | 1 | must | WI-1.229, WI-1.454 |
| PLN | FR-PLN-009 | 3 | must | WI-2.486, WI-3.308 |
| PLN | FR-PLN-010 | 1 | must | WI-1.412 |
| PLN | FR-PLN-101 | 1 | must | WI-1.403, WI-1.575 |
| PLN | FR-PLN-102 | 4 | nice | **GAP** (deferred, OI-08) |
| HDR | FR-HDR-001 | 1 | must | WI-1.223, WI-1.402, WI-2.172 |
| HDR | FR-HDR-002 | 1 | must | WI-1.462 |
| HDR | FR-HDR-003 | 1 | must | WI-1.403, WI-1.406, WI-4.450, WI-A.401 |
| HDR | FR-HDR-004 | 1 | must | WI-1.403 |
| HDR | FR-HDR-005 | 1 | must | WI-1.402, WI-1.462 |
| HDR | FR-HDR-006 | 3 | should | WI-3.532, WI-4.452, WI-4.457 |
| HDR | FR-HDR-007 | 1 | must | WI-1.456, WI-1.463 |
| HDR | FR-HDR-008 | 1 | must | WI-1.403 |
| HDR | FR-HDR-009 | 3 | should | WI-3.303 |
| HDR | FR-HDR-010 | 4 | nice | WI-4.453, WI-4.454 |
| HDR | FR-HDR-011 | 1 | must | WI-1.411 |
| HDR | FR-HDR-012 | 4 | nice | WI-4.455 |
| HDR | FR-HDR-013 | 4 | nice | WI-4.456 |
| HDR | FR-HDR-014 | 2 | should | WI-2.431 |
| HDR | FR-HDR-101 | 1 | must | WI-1.463, WI-A.400 |
| VQ | FR-VQ-001 | 3 | must | WI-3.300, WI-3.318 |
| VQ | FR-VQ-002 | 3 | must | WI-3.301, WI-3.305, WI-3.318 |
| VQ | FR-VQ-003 | 3 | must | WI-3.304, WI-4.309 |
| VQ | FR-VQ-004 | 3 | should | WI-3.301 |
| VQ | FR-VQ-005 | 3 | should | WI-3.306 |
| VQ | FR-VQ-006 | 3 | should | WI-3.307, WI-3.308 |
| VQ | FR-VQ-007 | 3 | must | WI-3.309, WI-3.310, WI-3.591 |
| VQ | FR-VQ-008 | 3 | must | WI-3.309, WI-4.307 |
| VQ | FR-VQ-009 | 3 | should | WI-3.314, WI-3.315 |
| VQ | FR-VQ-010 | 3 | should | WI-3.315, WI-3.316 |
| VQ | FR-VQ-011 | 3 | should | WI-3.306, WI-3.316 |
| VQ | FR-VQ-012 | 3 | must | WI-3.311 |
| VQ | FR-VQ-013 | 4 | must | **GAP** (deferred, OI-07) |
| VQ | FR-VQ-014 | 3 | must | WI-3.310, WI-3.312 |
| VQ | FR-VQ-015 | 3 | should | WI-3.317 |
| VQ | FR-VQ-016 | 3 | must | WI-3.451 |
| VQ | FR-VQ-017 | 3 | should | WI-3.313 |
| VQ | FR-VQ-018 | 3 | should | WI-3.452 |
| VQ | FR-VQ-019 | 4 | nice | WI-4.304, WI-4.306 |
| VQ | FR-VQ-020 | 4 | nice | WI-4.300, WI-4.303 |
| VQ | FR-VQ-021 | 4 | nice | WI-3.311 |
| VQ | FR-VQ-022 | 3 | should | WI-3.316, WI-3.451 |
| VQ | FR-VQ-023 | 3 | should | WI-3.308 |
| VQ | FR-VQ-024 | 3 | should | WI-3.301, WI-3.302 |
| VQ | FR-VQ-101 | 4 | nice | WI-4.308 |
| GUI | FR-GUI-001 | 1 | must | WI-1.450, WI-1.451 |
| GUI | FR-GUI-002 | 1 | must | WI-1.450, WI-1.451 |
| GUI | FR-GUI-003 | 1 | must | WI-1.455 |
| GUI | FR-GUI-004 | 1 | must | WI-1.452, WI-1.455 |
| GUI | FR-GUI-005 | 1 | must | WI-1.455 |
| GUI | FR-GUI-006 | 1 | must | WI-1.466 |
| GUI | FR-GUI-007 | 1 | must | WI-1.409, WI-1.456 |
| GUI | FR-GUI-008 | 2 | must | WI-1.466 |
| GUI | FR-GUI-009 | 2 | must | WI-1.456, WI-2.482 |
| GUI | FR-GUI-010 | 2 | must | WI-2.483 |
| GUI | FR-GUI-011 | 2 | should | WI-1.452, WI-2.482 |
| GUI | FR-GUI-012 | 1 | must | WI-1.453, WI-1.457 |
| GUI | FR-GUI-013 | 1 | must | WI-1.457, WI-1.460 |
| GUI | FR-GUI-014 | 1 | must | WI-1.458 |
| GUI | FR-GUI-015 | 1 | must | WI-1.460 |
| GUI | FR-GUI-016 | 1 | must | WI-1.462 |
| GUI | FR-GUI-017 | 2 | must | WI-1.455 |
| GUI | FR-GUI-018 | 2 | must | WI-2.484 |
| GUI | FR-GUI-019 | 2 | should | WI-2.439, WI-2.481 |
| GUI | FR-GUI-020 | 1 | must | WI-1.455, WI-2.439 |
| GUI | FR-GUI-021 | 2 | should | WI-2.489, WI-1.577 |
| GUI | FR-GUI-022 | 1 | must | WI-1.453 |
| GUI | FR-GUI-023 | 1 | must | WI-1.450, WI-1.466 |
| GUI | FR-GUI-024 | 1 | must | WI-1.466, WI-A.453 |
| GUI | FR-GUI-025 | 1 | must | WI-1.450 |
| GUI | FR-GUI-026 | 1 | must | WI-1.453 |
| GUI | FR-GUI-027 | 1 | must | WI-1.454 |
| GUI | FR-GUI-028 | 2 | should | WI-2.488 |
| GUI | FR-GUI-029 | 1 | must | WI-0.087, WI-2.480 |
| GUI | FR-GUI-030 | 1 | must | WI-1.404, WI-1.405 |
| GUI | FR-GUI-031 | 2 | must | WI-2.485 |
| GUI | FR-GUI-032 | 4 | nice | WI-4.112, WI-4.470 |
| GUI | FR-GUI-101 | 1 | must | WI-1.452, WI-1.576 |
| GUI | FR-GUI-102 | 1 | must | WI-1.457, WI-2.482 |
| GUI | FR-GUI-103 | 2 | should | WI-2.487 |
| CLI | FR-CLI-001 | 1 | must | WI-1.500 |
| CLI | FR-CLI-002 | 1 | must | WI-1.501 |
| CLI | FR-CLI-003 | 1 | must | WI-1.500 |
| CLI | FR-CLI-004 | 1 | must | WI-1.503, WI-1.510 |
| CLI | FR-CLI-005 | 1 | must | WI-1.504 |
| CLI | FR-CLI-006 | 1 | must | WI-1.505 |
| CLI | FR-CLI-007 | 1 | must | WI-1.507, WI-2.521 |
| CLI | FR-CLI-008 | 2 | should | WI-2.521 |
| CLI | FR-CLI-009 | 3 | must | WI-3.530 |
| CLI | FR-CLI-010 | 1 | must | WI-1.500 |
| CLI | FR-CLI-011 | 1 | must | WI-1.500, WI-1.512 |
| CLI | FR-CLI-012 | 1 | must | WI-1.500 |
| CLI | FR-CLI-013 | 1 | must | WI-1.508 |
| CLI | FR-CLI-014 | 4 | nice | WI-4.543 |
| CLI | FR-CLI-015 | 1 | must | WI-1.500 |
| CLI | FR-CLI-016 | 3 | must | WI-3.530 |
| CLI | FR-CLI-101 | 1 | must | WI-1.500, WI-4.599 |
| CLI | FR-CLI-102 | 1 | must | WI-1.500 |
| CLI | FR-CLI-103 | 1 | must | WI-1.500 |
| CLI | FR-CLI-104 | 1 | must | WI-1.510 |
| CLI | FR-CLI-105 | 1 | must | WI-1.500 |
| CLI | FR-CLI-106 | 1 | must | WI-1.500 |
| CLI | FR-CLI-107 | 1 | must | WI-1.500 |
| CLI | FR-CLI-108 | 1 | must | WI-0.034, WI-1.500 |
| CLI | FR-CLI-109 | 1 | must | WI-1.500 |
| API | FR-API-001 | 1 | must | WI-1.511, WI-4.540 |
| API | FR-API-002 | 1 | must | WI-1.511, WI-4.541 |
| API | FR-API-003 | 1 | must | WI-1.511 |
| API | FR-API-004 | 1 | must | WI-1.512, WI-4.542 |
| API | FR-API-005 | 1 | must | WI-1.511 |
| API | FR-API-006 | 1 | must | WI-1.512 |
| API | FR-API-007 | 4 | nice | WI-4.540, WI-4.542 |
| API | FR-API-008 | 4 | nice | WI-4.540 |
| API | FR-API-009 | 1 | must | WI-1.511, WI-2.520 |
| API | FR-API-010 | 1 | must | WI-1.511, WI-3.300, WI-4.540 |
| API | FR-API-101 | 4 | nice | WI-4.540 |
| API | FR-API-102 | 4 | nice | WI-4.541 |
| API | FR-API-103 | 4 | nice | WI-4.540 |
| API | FR-API-104 | 4 | nice | WI-4.542 |
| API | FR-API-105 | 4 | nice | WI-4.545 |
| API | FR-API-106 | 4 | nice | WI-4.540 |
| AND | FR-AND-001 | 0 | must | WI-0.050, WI-A.450 |
| AND | FR-AND-002 | 0 | must | WI-0.052, WI-A.400, WI-A.401 |
| AND | FR-AND-003 | 0 | must | WI-4.458, WI-A.404 |
| AND | FR-AND-004 | 0 | must | WI-0.055 |
| AND | FR-AND-005 | A/1 | must | WI-0.051, WI-1.101, WI-A.452 |
| AND | FR-AND-006 | A/1 | must | WI-A.452 |
| AND | FR-AND-007 | A/1 | must | WI-0.054, WI-A.455 |
| AND | FR-AND-008 | A/1 | must | WI-A.451 |
| AND | FR-AND-009 | A/1 | must | WI-A.453 |
| AND | FR-AND-010 | A/1 | must | WI-A.453 |
| AND | FR-AND-011 | A/1 | must | WI-A.453 |
| AND | FR-AND-012 | A/1 | must | WI-A.404, WI-A.451 |
| AND | FR-AND-013 | A/1 | must | WI-0.087, WI-1.577 |
| AND | FR-AND-014 | A/2 | must | WI-0.053, WI-2.168, WI-1.577 |
| AND | FR-AND-015 | A/1 | must | WI-0.053, WI-2.171 |
| AND | FR-AND-016 | A/1 | must | WI-0.053, WI-A.456 |
| AND | FR-AND-017 | A/1 | must | WI-2.488, WI-A.452 |
| AND | FR-AND-018 | A/0 | must | WI-0.056, WI-A.456 |
| MAC | FR-MAC-001 | 0 | must | WI-0.060, WI-0.063 |
| MAC | FR-MAC-002 | 1 | must | WI-1.406 |
| MAC | FR-MAC-003 | 1 | must | WI-1.406, WI-1.411 |
| MAC | FR-MAC-004 | 1 | must | WI-2.171 |
| MAC | FR-MAC-005 | 0 | must | WI-0.061 |
| MAC | FR-MAC-006 | 0 | must | WI-0.061, WI-0.607 |
| MAC | FR-MAC-007 | 0 | must | WI-0.062 |
| MAC | FR-MAC-008 | 1 | must | WI-0.060, WI-1.454 |
| MAC | FR-MAC-009 | 1 | must | WI-1.402, WI-1.406 |
| LNX | FR-LNX-001 | 0 | must | WI-0.073 |
| LNX | FR-LNX-002 | 4 | nice | WI-4.450, WI-4.451 |
| LNX | FR-LNX-003 | 1 | must | WI-2.171 |
| LNX | FR-LNX-004 | 0 | must | WI-0.071, WI-0.072 |
| LNX | FR-LNX-005 | 1 | must | WI-0.070, WI-0.072 |
| LNX | FR-LNX-006 | 1 | must | WI-0.073, WI-1.405 |
| LNX | FR-LNX-007 | 1 | must | WI-1.500 |
| NFR | NFR-001 | 1 | must | WI-0.560, WI-1.125 |
| NFR | NFR-002 | 1 | must | WI-0.560, WI-2.171 |
| NFR | NFR-003 | 2 | should | WI-2.438 |
| NFR | NFR-004 | 2 | should | WI-0.560, WI-2.151 |
| NFR | NFR-005 | 3 | should | WI-3.317 |
| NFR | NFR-006 | 1 | must | WI-0.560, WI-2.171 |
| NFR | NFR-007 | 1 | must | WI-1.512 |
| NFR | NFR-008 | 1 | must | WI-0.560, WI-1.450 |
| NFR | NFR-009 | 1 | must | WI-0.087, WI-0.560 |
| NFR | NFR-010 | 1 | must | WI-0.091, WI-0.092 |
| NFR | NFR-011 | 1 | must | WI-1.511, WI-1.512 |
| NFR | NFR-012 | 0 | must | WI-0.033, WI-0.557 |
| NFR | NFR-013 | 1 | must | WI-0.557, WI-1.224 |
| NFR | NFR-014 | 2 | should | WI-2.151, WI-2.152 |
| NFR | NFR-015 | 1 | must | WI-0.031, WI-0.558 |
| NFR | NFR-016 | 1 | must | WI-0.559, WI-1.102 |
| NFR | NFR-017 | 1 | must | WI-0.085, WI-1.510 |
| NFR | NFR-018 | 3 | should | WI-3.310, WI-3.590 |
| NFR | NFR-019 | 0 | must | WI-0.001, WI-0.061 |
| NFR | NFR-020 | 4 | nice | WI-4.304 |
| NFR | NFR-021 | 1 | must | WI-1.450, WI-3.315, WI-4.210 |
| NFR | NFR-022 | 1 | must | WI-0.002, WI-0.010 |
| NFR | NFR-023 | 1 | must | WI-1.466, WI-2.439 |
| NFR | NFR-024 | 2 | should | WI-2.439 |
| NFR | NFR-025 | 1 | must | WI-1.450, WI-1.500 |
| NFR | NFR-026 | 0 | must | WI-0.607, WI-2.159, WI-2.162 |
| NFR | NFR-027 | 0 | must | WI-0.006, WI-1.501, WI-1.511 |
| NFR | NFR-028 | 0 | must | WI-0.004, WI-0.030, WI-0.031 |
| NFR | NFR-029 | 0 | must | WI-0.007 |
| NFR | NFR-030 | 0 | must | WI-0.000 |
| NFR | NFR-031 | 0 | must | WI-0.032, WI-0.602, WI-0.603 |
| NFR | NFR-032 | 0 | must | WI-0.600, WI-0.601 |
| NFR | NFR-033 | 0 | must | WI-0.602, WI-0.603 |
| NFR | NFR-034 | 0 | must | WI-0.006 |
| NFR | NFR-035 | 1 | must | WI-1.466, WI-1.500 |
| NFR | NFR-036 | 1 | must | WI-0.007, WI-1.224 |

---

### Glossary additions (chapter 4)

- **Alignment confidence** — the margin `S(o*) − S(o₂)` between the best and second-best NCC alignment scores (range 0–2, chapter 9 §9.3.1); below `min_confidence` (0.05) the result is flagged `low_confidence`.
- **Analysis path** — the GOP-local software decode through an analysis plugin that produces `FrameAnalysis` records (ADR-009), as opposed to the playback path.
- **Area-normalised statistic** — a histogram value expressed as the percentage of luma samples covered by the counted blocks.
- **Bits-normalised statistic** — a histogram value expressed as the percentage of the frame's total bits consumed by the counted blocks.
- **Codec extension table** — the per-codec FlatBuffer table (`AvcMb`, `HevcCu`, `HevcPu`, `HevcTu`, `Av1Block`, `Av1Txb`) nested in `Block.ext` holding every syntax element read for that block.
- **Comparison project** — a GUI session holding one reference and up to eight distorted files for joint metric runs and RD plotting.
- **Content URI** — an Android `content://` resource handle obtained through the Storage Access Framework and opened as a file descriptor.
- **Dataspace** — the Android buffer tag (`ADATASPACE_*`) telling the compositor the colour space and transfer of a surface.
- **Diagnostic code** — the stable identifier `<AREA>-<CODEC|FMT>-<nnnn>` attached to every user-visible error or warning.
- **Display model preset** — a named set of display parameters (peak and black luminance, EOTF, HLG OOTF) used by metrics in linear-light domains; ids per chapter 9 §9.4: `pq_1000`, `pq_4000`, `pq_10000`, `hlg_1000`, `bt1886_100` (plus `auto`, `custom`).
- **EDR (Extended Dynamic Range)** — Apple's HDR output model in which values above 1.0 in a linear sRGB layer are displayed up to the screen's headroom.
- **Encoding toolset summary** — a per-stream list of coding tools that were used at least once, with frame and block counts.
- **First-mismatch finder** — the job that compares two inputs sample by sample and reports the first differing frame, plane and position.
- **Golden test** — a test comparing output against a stored reference file (image, JSON or CSV) within a stated tolerance.
- **Hardened Runtime** — macOS code-signing mode required for notarisation that restricts dynamic code loading unless entitlements allow it.
- **Headroom** — the ratio between a display's peak luminance and its SDR reference white (1.0 means SDR only).
- **Licence gate** — the CI job that scans dependencies and fails on forbidden licences.
- **Lossless check (identity metric)** — a metric that reports whether two frames are bit-identical and counts differing samples.
- **M1 reference** — the performance reference machine `m1-air-8gb` (MacBook Air M1, 8 GB, macOS 15; chapter 14 §14.9.2) for all desktop budgets in this chapter.
- **Minimum CVS** — the smallest set of units, from the preceding random access point and including active parameter sets, that decodes a given frame.
- **Normalisation stage** — the metrics pre-processing pipeline, in the fixed order of chapter 9 §9.3: `crop → scale → chroma_upsample → bit_depth → range → colour → domain → temporal` (temporal alignment/pairing last), recorded in `NormalizationConfig`.
- **Notarisation** — Apple's automated malware scan whose ticket is stapled to a signed app so Gatekeeper accepts it.
- **Oracle** — an independent reference implementation (HM, JM, libaom `inspect`, FFmpeg `trace_headers`, MP4Box, `vmaf` CLI) whose output is treated as ground truth in tests.
- **Playback path** — the hardware-accelerated or libavcodec software decode used for display and scrubbing (ADR-009).
- **Pooling** — reduction of per-frame scores to sequence-level values; the always-present `pooled_mean/harmonic/min/p1/p5/std` vectors plus optional `PoolingResult`s per `ovt.fb.PoolingMethod` (chapter 6 §6.5, chapter 9 §9.8.1).
- **Plane stage** — a named intermediate picture of the decoding process (`Prediction`, `Residual`, `PreDeblock`, `PreSaoOrCdef`, `PreLoopRestoration`, `Reconstruction`, `Coefficients`).
- **PSS** — Proportional Set Size, Android's per-process memory measure reported by `dumpsys meminfo`.
- **Sidecar metric** — a metric implemented as an external process speaking the OVT JSON Lines protocol over stdio with frames in shared memory.
- **Virtual file** — an `ovt_File` implementation that presents a DASH/HLS representation's concatenated segments as one seekable byte range.
- **Width class** — the Android layout breakpoint set (< 600, 600–839, 840–1199, ≥ 1200 dp; chapter 11 §11.5) used to choose the panel arrangement.
- **Working domain** — the sample representation a metric declares it needs (`CodeValue`, `LinearLight`, `PU21`, `ICtCp`, `ModelRGB`).
