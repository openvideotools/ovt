# WI-4.101 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64 (4 cores), GCC 13.3.
Date: 2026-11-09. Spec version: 1.0-rc16.

ffmpeg.org, the FATE sample suite, the ITU conformance site and JM are not reachable from the container. The FFmpeg
tree is a `git clone --depth 1 --branch n7.1.1` of github.com/FFmpeg/FFmpeg with the two commits of
`plugins/ovt-plugin-ffmpeg/patches/0001-h264-export-mb-params.patch` on top (`git am` reproduces them byte for byte).
The ctest `ffmpeg_patches_apply` checks the file against the pinned tarball in CI. Since this WI,
`third_party/ffmpeg.cmake` applies the series to the plugin flavour, so CI builds and tests the patched decoder.

The parts that need the FATE samples or JM ran on the Mac Mini: see "Mac Mini runs" at the end.

## Test 1: the exported syntax equals JM's trace

Automated: `ffmpeg_mbparams_vs_jm` (`plugins/ovt-plugin-ffmpeg/tests/test_ffmpeg_blockparams.cpp`, ctest labels
`fast`, `needs-corpus`). For the 17 JVT streams with a JM trace golden (`tests/golden/h264/jvt_*.jm-trace.json.gz`:
CAVLC, CABAC, PCM, 8x8 transform, field pictures (HCAFF1_HHI_B, MR6_BT_B), MBAFF (MR9_BT_B), weighted prediction,
4:2:2 10-bit, multiple slices), it decodes the stream through `ovt_plugin_ffmpeg_blockparams` and compares, for
every macroblock the golden holds: `mb_type`, `sub_mb_type`, `ref_idx_l0/l1` (when the golden has them),
`coded_block_pattern`, `mb_qp_delta`, `intra_chroma_pred_mode`, `transform_size_8x8_flag`,
`mb_field_decoding_flag`, `mvd_l0/l1` in read order, and the signalled intra 4x4/8x8 modes (prev flag or rem,
derived from the exported modes with the H.264 8.3.1.1 neighbour rule; not for MBAFF pictures). The slice types,
field parity, MBAFF and `constrained_intra_pred_flag` come from the stream's `trace_headers` golden.
JM 19.0 traces the `mb_type` of CABAC P/SP slices as the syntax value + 1 (CI run of 2026-11-09: every macroblock of
the CABAC P pictures of HCAFF1_HHI_B and MR9_BT_B, the only ones among the goldens); the test applies that offset.

The test streams are not reachable from the container, so this test first runs in CI. The test's conversion from
the export to JM's signalled values was checked locally against FFmpeg's own parse instead: a throwaway build of
the patched tree printed the raw `mb_type`, `sub_mb_type` and mvd values at the points where the decoder reads them,
and `pred_intra_mode()` in signalled form. Those values were written as goldens and the test was run against
them, on four x264 streams (1280x720, 40 pictures each: CAVLC P, CAVLC with B and temporal direct, CABAC with B,
CABAC with 8 slices per picture, 8x8 transform in the last three):

```console
$ ./bptest_fake "ffmpeg plugin: ffmpeg_mbparams_vs_jm" -s | grep 'macroblocks [0-9]*, intra' | sort -u
  macroblocks 15840, intra modes 2064, mvds 8705, sub_mb_types 794
  macroblocks 15840, intra modes 3108, mvds 6595, sub_mb_types 415
  macroblocks 15840, intra modes 3720, mvds 6341, sub_mb_types 481
  macroblocks 15840, intra modes 4076, mvds 7804, sub_mb_types 629
test cases: 5 | 3 passed | 2 skipped     # 0 mismatches
```

With one mvd, one `sub_mb_type` and one intra mode changed in a golden, the test fails on exactly those three.

**CAMA1_Sony_C (the stream the WI names).** Its JM golden is committed (made on the Mac Mini with the oracle image,
2026-11-13), and `ffmpeg_mbparams_vs_jm on CAMA1_Sony_C` (`slow`) passes: 2,700 macroblocks at 1 and 4 threads.
CAMA1_Sony_C is intra-only (5 MBAFF frames, all I), so it checks the macroblock-level fields (`mb_type`, `cbp`,
`mb_qp_delta`, chroma mode, transform size, `mb_field_decoding_flag`) and **no `ref_idx`**. An earlier version of
this record said CAMA1 covered `ref_idx`; that was wrong.

**ref_idx.** The 17 fast goldens hold no `ref_idx` either: their traced P picture has one reference, and MR9_BT_B's
has no field macroblock pairs. `ffmpeg_mbparams_vs_jm ref_idx on CAPAMA3_Sand_F` (`slow`) covers it: an MBAFF + PAFF
CABAC stream with P and B pictures and several references. The test requires the golden to contain `ref_idx` values.
Its golden is owed from the Mac Mini (commands at the end); until then the case skips.

## Test 2: flag unset: no tables, same speed

The pool table is allocated in `alloc_picture()` only when `export_side_data` has
`AV_CODEC_EXPORT_DATA_VIDEO_BLOCK_PARAMS` and no hwaccel is active; every write is behind `if (sl->mbp)`, which is
NULL when the picture has no table. Decoded pixels do not change (framemd5 identical with the flag off, on, and
with 4 frame threads, on the five local streams).

Speed: unpatched n7.1.1 against the patched tree, both `--disable-x86asm` with only the h264 decoder, 1080p x264
streams (10 s, 300 frames), `-threads 1`, 7 runs each, interleaved, user time from `ffmpeg -benchmark`:

```console
$ ./b_base/ffmpeg -nostdin -benchmark -threads 1 -i hd_cabac.264 -f null -    # and b_patched, 7 runs each
hd_cabac base    [2.934, 2.617, 2.629, 2.449, 2.425, 2.543, 2.671]
         patched [2.591, 2.659, 2.603, 2.566, 2.468, 2.672, 2.553]   median -0.99 %
hd_cavlc base    [1.296, 1.286, 1.235, 1.317, 1.314, 1.262, 1.181]
         patched [1.292, 1.262, 1.331, 1.163, 1.344, 1.314, 1.215]   median +0.47 %
```

Both medians are inside ±1 %. The container is shared and noisy, so the Mac Mini repeated it with asm (Mac Mini
runs, step 6): 0.743 s unpatched against 0.737 s patched (median user time, 7 interleaved runs), a 0.8 % difference.

## Test 3: frame threads give the single-threaded output

Automated twice: `ffmpeg_mbparams_vs_jm` compares the whole dump at `threads 1` and at `threads 4` (frame
threading) on each of the 17 streams, and FFmpeg's `fate-h264-block-params-cabac-frame-threads` must match the
single-threaded reference. Locally, the dump of all five streams was identical at 1 and 4 threads over 3 runs each.
Under frame threading the export waits for the picture's decoding progress, and droppable pictures now report
progress while the flag is set (otherwise the export could read a table that the decoding thread is still filling).

## FATE without samples

```console
$ ../ff/configure --disable-doc --disable-programs --enable-ffmpeg --enable-ffprobe --disable-x86asm
$ make -j4 && make -k -j4 fate; echo "fate rc=$?"
fate rc=0
$ grep -c '^TEST' fate.log
1569
```

## Mac Mini runs

Mac Mini (macOS arm64), 2026-11-13, the patch from main applied to n7.1.1 and built with asm, using the commands below.

- Step 4: the three reference files were generated (`h264-block-params-cavlc` 27,932 bytes, `-cabac` 37,858,
  `-mbaff` 129,646). They are now in the h264 commit of `0001-h264-export-mb-params.patch`. All four
  `fate-h264-block-params-*` tests pass at the default thread count and with `THREADS=4`; the frame-threads test
  matches the single-threaded reference.
- Step 5: `make -k fate-h264` exits 0 (221 tests).
- Step 6: median user time 0.743 s unpatched, 0.737 s patched (0.8 %, inside ±1 %).
- CAMA1_Sony_C: both goldens generated twice with identical bytes; the test passes (see test 1).
- Owed: the CAPAMA3_Sand_F golden and test run (the same CAMA1 commands with `jvt_CAMA1_Sony_C` replaced by
  `jvt_CAPAMA3_Sand_F`, and the test name `ffmpeg_mbparams_vs_jm ref_idx on CAPAMA3_Sand_F`).

### Commands

The h264 decoder changed, so FFmpeg's h264 conformance tests must pass with the FATE samples, and the new FATE
tests need their reference files. From a checkout of branch `claude/project-thread-vmhjoo` in `~/ovt`:

```sh
# 1. The patched tree (n7.1.1 + the two commits)
mkdir -p ~/wi4101 && cd ~/wi4101
git clone --depth 1 --branch n7.1.1 https://github.com/FFmpeg/FFmpeg ff
git -C ff -c user.name=ovt -c user.email=noreply@ovt.dev am ~/ovt/plugins/ovt-plugin-ffmpeg/patches/0001-h264-export-mb-params.patch
git -C ff log --oneline -3     # avcodec/h264dec: export ..., avutil: add ..., configure: update copyright year

# 2. The FATE samples (about 1.5 GB)
mkdir -p ~/wi4101/fate-suite
rsync -vrltLW --timeout=60 rsync://fate-suite.ffmpeg.org/fate-suite/ ~/wi4101/fate-suite/

# 3. Build with the default (asm) configuration
mkdir -p ~/wi4101/build && cd ~/wi4101/build
../ff/configure --disable-doc --samples=$HOME/wi4101/fate-suite
make -j8

# 4. Reference files of the new tests (written to ../ff/tests/ref/fate/), then all four tests, twice
make fate-h264-block-params-cavlc fate-h264-block-params-cabac fate-h264-block-params-mbaff GEN=1
make fate-h264-block-params-cavlc fate-h264-block-params-cabac fate-h264-block-params-mbaff \
     fate-h264-block-params-cabac-frame-threads
make fate-h264-block-params-cavlc fate-h264-block-params-cabac fate-h264-block-params-mbaff \
     fate-h264-block-params-cabac-frame-threads THREADS=4
wc -c ../ff/tests/ref/fate/h264-block-params-*

# 5. The h264 conformance subset and the rest of the h264 tests
make -k -j8 fate-h264 2>&1 | tee fate-h264.log; echo "fate-h264 rc=$?"
grep -c '^TEST' fate-h264.log

# 6. Speed (acceptance test 2) with asm: unpatched vs patched, flag unset
git clone --depth 1 --branch n7.1.1 https://github.com/FFmpeg/FFmpeg ../ff-base
mkdir -p ../build-base && cd ../build-base && ../ff-base/configure --disable-doc && make -j8 ffmpeg && cd ../build
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30:duration=10 -c:v libx264 -crf 23 -bf 3 ~/wi4101/hd.264  # any ffmpeg with libx264
for i in 1 2 3 4 5 6 7; do for b in ../build-base ../build; do
  $b/ffmpeg -nostdin -benchmark -threads 1 -i ~/wi4101/hd.264 -f null - 2>&1 | grep -o 'utime=[0-9.]*s' | sed "s|^|$b |"
done; done
```

Then the JM check of acceptance test 1 on CAMA1_Sony_C, in the oracle image (amd64; Docker on the Mini runs it under
emulation):

```sh
cd ~/ovt
python3 -I tools/fetch_corpora.py --ids jvt_CAMA1_Sony_C --cache-dir ~/wi4101/corpora
mkdir -p ~/wi4101/goldens
docker run --rm --platform linux/amd64 -v "$PWD:/repo:ro" -v ~/wi4101/corpora:/corpora -v ~/wi4101/goldens:/out \
  -w /repo -e HOME=/tmp ghcr.io/openvideotools/ovt-oracles:main \
  python3 -I tools/goldens/gen_h26x.py --out /out --corpora-dir /corpora --only jvt_CAMA1_Sony_C \
  --oracles trace_headers,jm --work-dir /tmp/work
ls -l ~/wi4101/goldens/h264/
cp ~/wi4101/goldens/h264/jvt_CAMA1_Sony_C.trace_headers.json.gz ~/wi4101/goldens/h264/jvt_CAMA1_Sony_C.jm-trace.json.gz \
  tests/golden/h264/
cmake --build --preset macos-arm64-release --target ovt_plugin_ffmpeg_blockparams_tests ovt_plugin_ffmpeg_blockparams
OVT_CORPORA_DIR=~/wi4101/corpora build/macos-arm64-release/plugins/ovt-plugin-ffmpeg/tests/ovt_plugin_ffmpeg_blockparams_tests \
  "ffmpeg plugin: ffmpeg_mbparams_vs_jm on CAMA1_Sony_C"
```

For CAPAMA3_Sand_F, replace `jvt_CAMA1_Sony_C` with `jvt_CAPAMA3_Sand_F` in the commands above and run the test
`ffmpeg plugin: ffmpeg_mbparams_vs_jm ref_idx on CAPAMA3_Sand_F`; it must report `ref_idx` greater than 0.
