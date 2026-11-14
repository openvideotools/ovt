# WI-1.570 acceptance record

Test 1 (`jvt_CABA3_TOSHIBA_E` is tagged `slow`, so only the nightly run fetches it). Run by hand on 2026-11-14 at
d39271d, ShafferDix-Mini (Apple M4, macOS 15.8.2), `macos-arm64-release`, with only this corpus fetched:

```console
$ OVT_CORPORA_DIR=~/wi1570/corpora OVT_REQUIRE_CORPORA=1 ovt_tests_container \
    "units annexb: corpus access units equal the ffprobe goldens" -s
All tests passed (2441 assertions in 1 test case)
```

`-s` names `jvt_CABA3_TOSHIBA_E`: 300 access units (0..299) equal the golden. All 9 `units *` cases: 5 passed, 4 skipped
(no AV1 corpus fetched; Homebrew ffmpeg has no libaom), 12635 assertions pass.
