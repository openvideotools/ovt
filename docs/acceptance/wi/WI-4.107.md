# WI-4.107 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64 (4 cores), GCC 13.3.
Date: 2026-11-09. Spec version: 1.0-rc17.

ffmpeg.org, the FATE sample suite and the ITU conformance site are not reachable from the container. The FFmpeg tree
is a `git clone --depth 1 --branch n7.1.1` of github.com/FFmpeg/FFmpeg with patch 0001 applied by `git am` and the
commit of `plugins/ovt-plugin-ffmpeg/patches/0002-hevc-export-mvs-venc-params.patch` on top (`git am` of the file
reproduces it byte for byte). The ctest `ffmpeg_patches_apply` checks the series against the pinned tarball in CI;
`tools/apply_patches.py --check` now applies the series cumulatively to a scratch copy, because 0002 edits
`doc/APIchanges` and the version headers after 0001 (checked locally on a `git archive` tarball of n7.1.1: the
cumulative check passes, the old per-patch check fails on `doc/APIchanges`).

The parts that need the FATE samples or the conformance streams run in CI or on the Mac Mini: see "Owed" at the end.

## Test 1: the MV and QP grids equal libde265 on the conformance streams

Automated: `ffmpeg_hevc plugin: MV and QP grids equal libde265 on HEVC_v1`
(`plugins/ovt-plugin-ffmpeg/tests/test_ffmpeg_hevc_sidedata.cpp`, labels `fast`, `third_party`, `needs-corpus`). For
20 HEVC_v1 streams (AMP, CAINIT, CONFWIN, dependent slices, MAXBINS, merge, POC, PPS, RPS, SAO, structure, tiles,
temporal scalability, weighted prediction, MV clipping and edges, IPCM, WPP Main 10), it decodes the stream through
`libovt_plugin_ffmpeg_hevc` and compares every 4x4 cell of the CellGrid (prediction, QP, list 0 and list 1 vectors)
with `ovt_plugin_de265_grid_dump`, which prints the same values from patched libde265's internals API (patch 0001 of
the de265 plugin). It also checks that 4 slice threads give the same grids as 1 thread. libde265 1.0.15 never
outputs the last picture it decodes in some streams (#251), which in hierarchical-B streams is not the last in output
order: the test matches the pictures in order and lets at most two ffmpeg_hevc pictures go unmatched, with a warning.
The oracle's vectors are clamped to the CellGrid range (int16 in 1/16 samples) before the comparison, as the plugin
clamps them with MDL-GEN-0003 (MVCLIP_A_qualcomm_3 points far outside the picture).

The conformance streams are not reachable from the container, so this test first runs in CI. Locally the same
comparison ran through the plugin ABI (a static driver around the plugin objects, since the local FFmpeg build is not
PIC) on four x265 streams: 8-bit with B-frames (s1, 416x240); P-only with AQ and 16x16 quantization groups (s2,
352x288); 10-bit with AMP, WPP, several slices, weighted prediction and a B-pyramid (s3); minimum CU 16 with transform
skip (s4).

```text
stream  pictures (de265 / plugin)  grid (cells)    cells equal  threads 4 == 1
s1      49 / 50                    104 x 60        yes          yes
s2      50 / 50                    88 x 72         yes          yes
s3      29 / 30                    104 x 60        yes          yes
s4      29 / 30                    88 x 72         yes          yes
```

Inside libavcodec, `tools/venc_data_dump` printed identical output for 1 thread, 4 frame threads and 4 slice threads on
all four streams, and the CU areas tile each picture. The decoded pixels (framemd5) do not change when the export is
on, and `codecview=mv=pf+bf+bb` output is the same for 1 and 4 frame threads.

## Test 2: the patch is submitted upstream

Owed, needs Karl. Posting the patch to ffmpeg-devel is an outward action. The commit message carries
`Upstream-Status: Pending`; it moves to `Submitted [<archive link>]` once Karl agrees and the mail is sent.

## FFmpeg FATE (no samples)

`make -k fate` on a full `configure --disable-doc --disable-x86asm` build of the patched tree: exit status 0, 1 569
tests run, none failed. The hevc checkasm tests are among them.

## Owed: runs on the Mac Mini

1. `make fate-hevc SAMPLES=…` with the FATE samples, patched tree: every existing hevc test passes.
2. `make GEN=1 fate-hevc-encparams fate-hevc-mvs SAMPLES=…` to write `tests/ref/fate/hevc-encparams` and
   `tests/ref/fate/hevc-mvs`; then the frame-thread variants must pass against those refs. The refs are folded into
   the 0002 commit and the patch is regenerated (`git format-patch --zero-commit --no-signature`). Until then those
   four new FATE tests have no reference file.
3. Speed: decode time of a 1080p conformance stream with and without `-export_side_data mvs+venc_params`, recorded
   here.
