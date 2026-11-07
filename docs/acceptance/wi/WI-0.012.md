# WI-0.012 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, CMake 3.28.3, Ninja 1.11.1, GNU make 4.3, NASM.
Date: 2026-11-06. Spec version: 1.0-rc2. Commit: the head of the WI-0.012 pull request.

The container cannot reach ffmpeg.org (HTTP 403 from the proxy), so the local run used the n7.1.1 tag from
GitHub's FFmpeg mirror, packed into `ffmpeg-7.1.1.tar.xz`, with a scratch copy of `third_party/VERSIONS.md` whose
FFmpeg hash names that archive (`-DOVT_TP_VERSIONS_MD=<scratch>`). Source code is the same tag; only the archive
bytes differ (the official tarball adds a `VERSION` file). The Mac Mini run below uses the pinned ffmpeg.org
tarball unchanged.

```console
$ cmake --preset linux-x64-debug -DOVT_USE_VCPKG=OFF -DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER= \
    -DOVT_SYSTEM_DAV1D=ON -DOVT_TP_VERSIONS_MD=<scratch>/VERSIONS-local.md
-- OVT: FFmpeg n7.1.1 core (shared, _ovt) and plugin (static) flavours for linux; patent codecs in core: OFF; hwaccels: ON; GPL core/plugin: OFF/OFF
$ cmake --build --preset linux-x64-debug          # both flavours: about 3 min on 4 cores
$ ctest --preset pr
100% tests passed, 0 tests failed out of 137
```

## Test 1: Linux default options, no h264 symbols, avformat opens an MP4

```console
$ nm -D build/third_party/linux-x64-debug/ffmpeg-core/lib/libavcodec_ovt.so | grep -c ff_h264
0
$ ctest --test-dir build/linux-x64-debug -R "ffmpeg core probe" --output-on-failure
1/5 Test #14: ffmpeg core probe: avcodec_configuration is printed and names the flavour ......   Passed
2/5 Test #15: ffmpeg core probe: h264/hevc parsers are present in every flavour ..............   Passed
3/5 Test #16: ffmpeg core probe: the core flavour has no AVC/HEVC decoder unless OVT_PATENT_CODECS_IN_CORE   Passed
4/5 Test #17: ffmpeg core probe: the core flavour has the platform AV1 decoders ..............   Passed
5/5 Test #18: ffmpeg core probe: avformat opens an MP4 of the corpus .........................   Passed
```

Result: pass. The MP4 is corpus entry `mpegff_a1_foreman_qcif` (`tools/fetch_corpora.py --ids
mpegff_a1_foreman_qcif`). The manifest lists it as h264; FFmpeg demuxes it as MPEG-4 Part 2 video, so the test checks
for a demuxed video stream with a known codec id rather than for H.264.

Automated: `ovt_tests_ffmpeg_probe` (all five cases above).

## Test 2: the plugin flavour exports only the plugin entry points

```console
$ ctest --test-dir build/linux-x64-debug -R ffmpeg_plugin_symbol_leak -V
OK: .../libovt_ffmpeg_symbol_leak_fixture.so exports only: ovt_analysis_decoder_v1 ovt_plugin_describe
```

Result: pass, on the fixture module (spec question #129): `libovt_plugin_ffmpeg.so` itself is built by WI-2.169,
which points the same script at it.

Automated: `ffmpeg_plugin_symbol_leak` (`plugins/ovt-plugin-ffmpeg/tests/test_symbol_leak.sh`).

## Test 3: macOS plugin flavour reports `--enable-videotoolbox`

Machine: the Mac Mini build host (macOS, Apple silicon), 2026-11-06, head `10d32c2`, the pinned ffmpeg.org n7.1.1
tarball. The run was relayed from the Mac Mini thread; this record gives its results, not a verbatim log.

```sh
cmake --preset macos-arm64-debug && cmake --build --preset macos-arm64-debug
ctest --test-dir build/macos-arm64-debug -R ffmpeg -V
```

`pr-macos` is the test preset of `macos-arm64-release` (CMakePresets.json, chapter 13 §13.2.4), so a debug build is
run with `--test-dir`.

- `ffmpeg plugin probe: avcodec_configuration is printed and names the flavour`: passed. The plugin flavour's
  `avcodec_configuration()` contains `--enable-videotoolbox`. So does the core flavour's.
- The other core and plugin probe cases passed. `avformat opens an MP4 of the corpus` was skipped: corpus entry
  `mpegff_a1_foreman_qcif` is not fetched on the Mini.
- `ffmpeg_plugin_symbol_leak` failed because of the script, not because of a leak. BSD grep rejected the empty
  alternative in `^(…|)$`, so the export list came out empty. `nm -gU` on the fixture shows exactly
  `_ovt_analysis_decoder_v1` and `_ovt_plugin_describe`. The script is fixed in the commit after `10d32c2`
  (blank lines are filtered by a separate `grep -v '^$'`), and the Mac CI job on the PR re-runs it.

Result: pass (acceptance test 3). Test 2 on macOS: no leak; the script fix is verified by CI.

## Test 4: Android arm64 on the emulator

Deferred: no Android device or emulator is available to this work item (the Pixel Tablet is on hold, and the
emulator job is WI-0.035). The checks are automated in `ovt_tests_ffmpeg_probe` for `__ANDROID__` builds
(`--enable-mediacodec` in `avcodec_configuration()`, `avcodec_find_decoder_by_name("av1_mediacodec") != nullptr`).
The Android toolchain wrapper is covered by `ffmpeg_configure_flags` with a fake NDK.

## Determinism

Both flavours were built twice from an empty prefix (`rm -rf build/third_party/linux-x64-debug/ffmpeg-*`, then
`cmake --build`) and the installed libraries compared byte for byte:

```console
$ sha256sum ffmpeg-core/lib/*.so.*.* ffmpeg-plugin/lib/*.a   # after each build; diff of the two lists is empty
$ cmp first/libavcodec_ovt.so.61.19.101 second/libavcodec_ovt.so.61.19.101 && echo IDENTICAL
IDENTICAL
```

Result: pass. The first attempt differed in libavcodec: FFmpeg's `ffbuild/version.sh` ran `git describe` at make
time and found the OVT checkout around the build tree, so the OVT commit leaked into `av_version_info()`.
`GIT_CEILING_DIRECTORIES` is now set for configure, build and install; `av_version_info()` reports `7.1.1`.

## Amendment: other pool-covered decoders left out of the core (owner decision on #132)

Spec 1.0-rc7 (§13.3.3, POL-PAT-2) adds VVC, AAC/HE-AAC, MPEG-4 Part 2 and VC-1 to the core flavour's
`--disable-decoder=`/`--disable-hwaccel=` lists. The core probe case is renamed "the core flavour has no
pool-covered decoder unless OVT_PATENT_CODECS_IN_CORE" and checks all of them; `ffmpeg_configure_flags` checks
the new lists and that the plugin flavour names none of them.

Configure and build check, 2026-11-06, Linux x86-64 cloud container (Ubuntu 24.04, gcc), FFmpeg n7.1.1 from the
GitHub mirror, run with the core/linux flag list that `ovt_ffmpeg_configure_flags()` produces (HW off, no dav1d):

```console
$ grep -E "CONFIG_(VVC|AAC|AAC_FIXED|AAC_LATM|MPEG4|VC1|WMV3|H264|HEVC|VP9|MPEG2VIDEO|AV1)_DECODER |CONFIG_(AAC|VVC|MPEG4VIDEO|VC1)_PARSER " config_components.h
#define CONFIG_H264_DECODER 0
#define CONFIG_HEVC_DECODER 0
#define CONFIG_MPEG2VIDEO_DECODER 1
#define CONFIG_MPEG4_DECODER 0
#define CONFIG_VC1_DECODER 0
#define CONFIG_VP9_DECODER 1
#define CONFIG_VVC_DECODER 0
#define CONFIG_WMV3_DECODER 0
#define CONFIG_AAC_DECODER 0
#define CONFIG_AAC_FIXED_DECODER 0
#define CONFIG_AAC_LATM_DECODER 0
#define CONFIG_AV1_DECODER 1
#define CONFIG_AAC_PARSER 1
#define CONFIG_MPEG4VIDEO_PARSER 1
#define CONFIG_VC1_PARSER 1
#define CONFIG_VVC_PARSER 1
$ nm libavcodec_ovt.so | grep -c -E "ff_(vvc|aac|mpeg4|vc1|wmv3)_decoder$"
0
```

A program linked against the built `libavcodec_ovt.so` found no decoder for h264, hevc, vvc, aac, aac_latm, mpeg4,
vc1 and wmv3, and found the vp9, mpeg2video, av1, mp3 and opus decoders. The body of the renamed probe case,
compiled against the same build with `-Wall -Wextra -Werror`, passed. The full probe runs in the Mac CI job.

Result: pass.

## Amendment: Microsoft MPEG-4 Part 2 variants left out of the core (owner decision, spec 1.0-rc13)

Spec 1.0-rc13 (§13.3.3, POL-PAT-2) adds `msmpeg4v1`, `msmpeg4v2`, `msmpeg4v3`, `wmv1` and `wmv2` to the core
flavour's `--disable-decoder=` list. In n7.1.1 `configure` all five select only the `msmpeg4dec` component
(`wmv2` also selects shared DSP code), so the change is a flag-list change with no patch; none of them has a
hwaccel. The `h263` decoder (and `flv`) stays. The core probe case and `ffmpeg_configure_flags` check the new names.

Configure and build check, 2026-11-07, Linux x86-64 cloud container (Ubuntu 24.04, gcc 13), FFmpeg n7.1.1 from the
GitHub mirror, core/linux flag list from `ovt_ffmpeg_configure_flags()` (HW off, no dav1d, plus `--disable-x86asm`):

```console
$ grep -E "CONFIG_(MSMPEG4V1|MSMPEG4V2|MSMPEG4V3|WMV1|WMV2|MPEG4|VC1|WMV3|H263|H264|VP9|MPEG2VIDEO)_DECODER |CONFIG_MSMPEG4DEC |CONFIG_(MPEG4VIDEO|VC1|H263)_PARSER " config_components.h config.h
config_components.h:#define CONFIG_H263_DECODER 1
config_components.h:#define CONFIG_H264_DECODER 0
config_components.h:#define CONFIG_MPEG2VIDEO_DECODER 1
config_components.h:#define CONFIG_MPEG4_DECODER 0
config_components.h:#define CONFIG_MSMPEG4V1_DECODER 0
config_components.h:#define CONFIG_MSMPEG4V2_DECODER 0
config_components.h:#define CONFIG_MSMPEG4V3_DECODER 0
config_components.h:#define CONFIG_VC1_DECODER 0
config_components.h:#define CONFIG_VP9_DECODER 1
config_components.h:#define CONFIG_WMV1_DECODER 0
config_components.h:#define CONFIG_WMV2_DECODER 0
config_components.h:#define CONFIG_WMV3_DECODER 0
config_components.h:#define CONFIG_H263_PARSER 1
config_components.h:#define CONFIG_MPEG4VIDEO_PARSER 1
config_components.h:#define CONFIG_VC1_PARSER 1
config.h:#define CONFIG_MSMPEG4DEC 0
$ nm libavcodec_ovt.so | grep -E "ff_(h263|msmpeg4v[123]|wmv[12])_decoder$"
0000000000929c40 d ff_h263_decoder
```

`libavcodec_ovt.so` and `libavformat_ovt.so` built cleanly. A program built with `-Wall -Wextra -Werror` against
that `libavcodec_ovt.so` found no decoder for msmpeg4v1, msmpeg4v2, msmpeg4v3, wmv1, wmv2, mpeg4 and wmv3, and
found the h263, flv1, vp9 and mpeg2video decoders. `cmake -P third_party/ffmpeg/test_configure_flags.cmake` passes.

Result: pass.
