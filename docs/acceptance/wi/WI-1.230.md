# WI-1.230 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64, GCC 13.3 (`linux-x64-debug`) and Clang 18
(`linux-x64-asan-ubsan`), system FFmpeg 6.1.1 (`-DOVT_SYSTEM_FFMPEG=ON`; the pinned FFmpeg 7.1 build runs in CI)
and ffprobe 6.1.1. Date: 2026-11-08. Spec version: 1.0-rc16.

## Test 1: packet counts and positions equal ffprobe

Automated: `demux: packet counts and positions equal ffprobe on every corpus MP4/IVF/ES/Y4M`
(`[demux][ffprobe][needs-corpus]`) and, on in-memory files that need no corpus,
`demux: packet counts and positions equal ffprobe on the synthetic files` (`[demux][ffprobe][fast]`). Both skip
when ffprobe is not on PATH. ffprobe runs with the demuxer options the Demuxer passes (§8.7.2 rule 4:
`-advanced_editlist 0 -export_all 1` for ISOBMFF, `-fflags +genpts` for MPEG-TS), so both sides demux the same
way. MPEG-TS and Annex B accept ±1 packet and compare positions only on equal counts (§8.7.1 "Oracle parity").

Corpus run: every manifest entry with a single `isobmff`/`mp4`/`ivf`/`annexb`/`mpegts`/`y4m`/`obu` container,
under 3 MB and fetchable from this container (itu.int is blocked here, so the JVT/JCT-VC streams were not run):
137 files, 118 IVF (aom AV1 test vectors), 11 MPEG-TS, 7 ISOBMFF, 1 Annex B.

```console
$ python3 tools/fetch_corpora.py --cache-dir /tmp/corpora --ids <the 182 ids> --jobs 8
$ OVT_CORPORA_DIR=/tmp/corpora build/linux-x64-debug/core/tests/demux/ovt_tests_demux "[needs-corpus]"
All tests passed (20070 assertions in 1 test case)
```

## Test 2: seek_to_time(30 s, backward)

Automated: `demux: seek_to_time(30 s, backward) lands on a key frame at or before 30 s` (a 60-frame 1 fps Y4M,
time base 1/1) and `demux: seek_to_time backward on ISOBMFF returns the preceding sync sample`. libavformat's
`yuv4_read_seek` steps one frame back for `AVSEEK_FLAG_BACKWARD`, so the Y4M case lands on pts 29, which
satisfies "keyframe with pts ≤ 30 s".

## Test 3: 0-byte file

Automated: `demux: a 0-byte file is rejected without leaking`, over the hints "", isobmff, ivf, y4m, annexb,
mpegts, obu, ebml, h264 and av1. Run under ASan + LSan:

```console
$ cmake --preset linux-x64-asan-ubsan -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ...
$ ASAN_OPTIONS=detect_leaks=1 build/linux-x64-asan-ubsan/core/tests/demux/ovt_tests_demux
test cases:   14 |   13 passed | 1 skipped
assertions: 3603 | 3603 passed
$ ASAN_OPTIONS=detect_leaks=1 build/linux-x64-asan-ubsan/core/tests/fuzz/ovt_fuzz_demuxer_open_regress \
    core/tests/fuzz/regressions/demuxer_open core/tests/fuzz/corpus/demuxer_open
fuzz regress: 8 inputs replayed
```

(The skipped case is the corpus parity test, which ran separately above.)

## Fuzz seeds

`core/tests/fuzz/corpus/demuxer_open/`: byte 0 selects the hint, the rest is a 16x16, 3-frame `testsrc` file
made with FFmpeg 6.1.1 (AV1 by libaom in MP4, IVF, OBU and WebM; raw Y4M; MPEG-2 video in MPEG-TS).
