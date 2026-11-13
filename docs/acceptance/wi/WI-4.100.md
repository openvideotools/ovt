# WI-4.100 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64 (4 cores), GCC 13.3.
Date: 2026-11-08. Spec version: 1.0-rc16.

ffmpeg.org is not reachable from the container, so the tree is a `git clone --depth 1 --branch n7.1.1` of
github.com/FFmpeg/FFmpeg, the tag the pinned release tarball is cut from. The ctest `ffmpeg_patches_apply` checks
the patch against the pinned tarball itself (SHA-256 from `third_party/VERSIONS.md`) in CI.

## Test 1: all existing FATE tests pass on the patched tree

```console
$ # ff/ holds the n7.1.1 tag plus the one commit that `git format-patch --zero-commit` turned into 0001
$ mkdir ffbuild && cd ffbuild
$ ../ff/configure --disable-doc --disable-programs --enable-ffmpeg --enable-ffprobe --disable-x86asm
License: LGPL version 2.1 or later
$ make -j4 && make -k -j4 fate; echo "fate rc=$?"
...
TEST    video_block_params
...
fate rc=0
$ grep -c '^TEST' fate.log
1569
```

Every test that needs no sample files ran and passed, the new `fate-video_block_params` included. The FATE sample
suite (`rsync://fate-suite.ffmpeg.org`) cannot be reached from the container, so the sample-based tests were not
run. The patch touches only libavutil (one new side-data type, one new file pair), and no decoder or filter reads
the new type until WI-4.101.

## Test 2: 8160 macroblocks round-trip through frame side data

Automated inside FFmpeg's tree: `fate-video_block_params` (`libavutil/tests/video_block_params.c`, reference
`tests/ref/fate/video_block_params`). It calls `av_video_block_params_create_side_data(frame, H264, 8160)`, writes
index-dependent values into every field of every `AVH264MacroblockParams`, clones the frame, finds the entry
again with `av_frame_get_side_data()` and compares every field of every block:

```console
$ make fate-video_block_params
TEST    video_block_params
$ libavutil/tests/video_block_params
alloc: type 0, nb_blocks 8160, block_size ok 1, size ok 1
alloc: blocks zeroed 1
alloc: empty, nb_blocks 0, size ok 1
alloc: unknown type NULL 1
side data: unknown type NULL 1, nb_side_data 0
side data: Video block parameters, size ok 1
side data: type 0, nb_blocks 8160, plane 120x68
side data: 8160 blocks round-tripped, errors 0
```

The same test, compiled with `-fsanitize=address,undefined` against the patched `libavutil.a`, prints the same
output with no sanitizer report. `make fate-libavutil` passes (43 tests). FFmpeg's `tools/patcheck` flags only
the usual false positives for test programs (`printf`, a non-static `main`).
