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

Pending: the Mac Mini run (`ctest --preset pr-macos -R "ffmpeg plugin probe" -V`), recorded below when it is done.
The check is automated in `ovt_tests_ffmpeg_plugin_probe` (`--enable-videotoolbox` on Apple with
`OVT_ENABLE_HW_DECODE=ON`).

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
