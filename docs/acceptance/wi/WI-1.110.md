# WI-1.110 acceptance record

## Corpus run: `h264_sei_corpus_cvse3_sony_h` on the Mac Mini

Machine: ShafferDix-Mini, Apple M4, macOS 15.8.2, `macos-arm64-release` (AppleClang 17). Date: 2026-11-14.
Branch head 737d061 (WI-1.110a). Corpus `jvt_CVSE3_Sony_H` (`CVSE3_Sony_H.jsv`) fetched from itu.int with
`tools/fetch_corpora.py` (hash checked against the manifest). The test stands in for CVSEFDFT3_Sony_E (acceptance test 2);
its oracle is the committed `tests/golden/h264/jvt_CVSE3_Sony_H.trace_headers.json.gz`.

```console
$ python3 -I tools/fetch_corpora.py --ids jvt_CVSE3_Sony_H --cache-dir ~/wi1110/corpora
ok          jvt_CVSE3_Sony_H
$ cmake --build --preset macos-arm64-release --target ovt_tests_syntax_h264_sei
$ OVT_CORPORA_DIR=~/wi1110/corpora OVT_REQUIRE_CORPORA=1 \
    build/macos-arm64-release/core/tests/syntax/h264/ovt_tests_syntax_h264_sei "h264_sei_corpus_cvse3_sony_h"
All tests passed (34 assertions in 1 test case)
```
