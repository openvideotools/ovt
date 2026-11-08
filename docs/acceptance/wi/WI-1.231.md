# WI-1.231 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3.0, CMake 3.28.3, FFmpeg 6.1.1 (system package).
Date: 2026-11-08. Spec version: 1.0-rc16. Commit: the head of the WI-1.231 pull request.

Acceptance tests 2 and 3 are automated, fast and corpus-free (`core/tests/demux/unit_split_test.cpp`), and
they run in CI. Acceptance test 1 needs `syn_h264_multislice_cavlc_mp4`, a synthesised corpus entry (chapter 14
§14.3.3 recipe 29). It can't be fetched on the CI runners, so `unit_split_extract_samples` exits 77 and
`unit_split_corpus` is skipped there. This record is the run on a local stand-in.

## Test 1: avc1 samples of `syn_h264_multislice_cavlc_mp4`

The stand-in follows the recipe's intent: H.264 Baseline, CAVLC, four slices per picture, 4-byte length
prefixes in `avc1`. The oracle is the independent NAL walk of `tools/extract_samples.py` (spec question #338
replaces `MP4Box -dnal`).

```console
$ ffmpeg -f lavfi -i testsrc2=size=640x360:rate=30 -frames:v 30 -c:v libx264 -profile:v baseline -coder 0 \
    -slices 4 -g 15 -movflags +faststart corp/syn_h264_multislice_cavlc_mp4/syn_h264_multislice_cavlc.mp4
$ echo '{"primary": "syn_h264_multislice_cavlc.mp4"}' > corp/syn_h264_multislice_cavlc_mp4/.ok
$ OVT_CORPORA_DIR=$PWD/corp ctest --test-dir build/linux-x64-debug -R "unit_split_extract|unit_split_corpus"
    Start 157: unit_split_extract_samples
1/2 Test #157: unit_split_extract_samples .......   Passed    0.06 sec
    Start 158: unit_split_corpus
2/2 Test #158: unit_split_corpus ................   Passed    0.01 sec
100% tests passed, 0 tests failed out of 2
$ python3 -c "import json; d = json.load(open('build/linux-x64-debug/core/tests/demux/unit_split_samples/samples.json')); \
    print(d['sample_entry'], d['nal_length_size'], len(d['samples']), [s['nal_count'] for s in d['samples']][:8])"
avc1 4 30 [7, 4, 4, 4, 4, 4, 4, 4]
```

Result: pass. For each of the 30 samples, the number of units equals `nal_count`, no diagnostics are raised,
and the first byte of every unit is a NAL header (not `0x00`, forbidden bit 0, `nal_unit_type` 1..23). Sample 0
has 7 units: SEI, SPS and PPS before the four IDR slices. The other samples have four slices each.

When the real corpus entry exists (`tools/fetch_corpora.py` on a machine that synthesises the `syn_*` set), the
same two CTest entries run unchanged.
