# WI-1.109 acceptance record

## Corpus tests on the Mac Mini

Machine: ShafferDix-Mini, Mac mini (Mac16,10), Apple M4, macOS 15.8.2, `macos-arm64-release` (AppleClang 17).
Date: 2026-11-15. Branch head 9942d8d (WI-1.109b). The JVT streams were fetched from itu.int, and their hashes were
checked against the manifest.

```console
$ python3 -I tools/fetch_corpora.py --ids jvt_CVWP1_TOSHIBA_E,jvt_MR9_BT_B --cache-dir ~/wi1110/corpora
ok          jvt_CVWP1_TOSHIBA_E
ok          jvt_MR9_BT_B
$ cmake --preset macos-arm64-release
$ cmake --build --preset macos-arm64-release --target ovt_tests_syntax_h264_slice_header
$ OVT_CORPORA_DIR=~/wi1110/corpora OVT_REQUIRE_CORPORA=1 \
    build/macos-arm64-release/core/tests/syntax/h264/ovt_tests_syntax_h264_slice_header h264_slice_pwt_unit_corpus_cvwp1
All tests passed (18 assertions in 1 test case)
$ OVT_CORPORA_DIR=~/wi1110/corpora OVT_REQUIRE_CORPORA=1 \
    build/macos-arm64-release/core/tests/syntax/h264/ovt_tests_syntax_h264_slice_header h264_slice_mmco_unit_corpus_mr9
All tests passed (18 assertions in 1 test case)
```

The same two tests also pass on main at 0fbcd92 (WI-1.109a, #532), run on the same machine on 2026-11-15.
