# WI-1.572 acceptance record

Part a (PR #604): the 10 `syn_h264_*` golden tests use generated corpora (tests/corpora/synthetic/recipes.json).
Hosted CI does not generate them, so those tests exit 77 (skipped) there. The record below is the Mac Mini run of
the container-path stream, the first time `h264_container_to_annexb` ran against the real file. The run of the full
10-test set is added when the PAFF stream's generation finishes.

## syn_h264_multislice_cavlc_mp4 (trace_headers and gstparse)

```console
WI-1.572a (PR #604 @ 00a6009) — syn_h264_multislice_cavlc_mp4 golden test on the Mac Mini, 2026-11-16 13:17 UTC
Machine: Mac16,10 Apple M4, macOS 15.8.2; preset macos-arm64-debug
Corpus: python3 -I tests/corpora/synthetic/generate.py --only syn_h264_multislice_cavlc_mp4 --out <dir> --check, in the ovt-oracles image (pinned encoders); hashes:
c6fe0640a2cb7d7561a40671b5e6334a9360602eeb267e86b4437361ca02e21d  syn_h264_multislice_cavlc
670a81ee31d36199d2d994e2d34a32789e71b7ca51330706d1b4ed4a997df72d  syn_h264_multislice_cavlc_mp4

$ OVT_CORPORA_DIR=<dir> OVT_REQUIRE_CORPORA=1 ctest --preset macos-arm64-debug -R "syntax_golden_h264_.*syn_h264_multislice_cavlc_mp4" --output-on-failure
Test project /Users/ovt-build/mxf-main/build/macos-arm64-debug
    Start 111: syntax_golden_h264_trace_headers_syn_h264_multislice_cavlc_mp4
    Start 137: syntax_golden_h264_gstparse_syn_h264_multislice_cavlc_mp4
1/2 Test #137: syntax_golden_h264_gstparse_syn_h264_multislice_cavlc_mp4 ........   Passed    0.22 sec
2/2 Test #111: syntax_golden_h264_trace_headers_syn_h264_multislice_cavlc_mp4 ...   Passed    0.22 sec

100% tests passed out of 2

Label Time Summary:
fast             =   0.44 sec*proc (2 tests)
golden           =   0.44 sec*proc (2 tests)
needs_corpus     =   0.44 sec*proc (2 tests)
syntax_golden    =   0.44 sec*proc (2 tests)

Total Test time (real) =   0.34 sec
rc=0
```
