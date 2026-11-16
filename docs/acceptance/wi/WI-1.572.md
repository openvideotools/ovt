# WI-1.572 acceptance record

The 10 `syn_h264_*` golden tests of part a use generated corpora (tests/corpora/synthetic/recipes.json). Hosted CI
does not generate them, so they exit 77 (skipped) there. Below is the Mac Mini run of PR #604 (00a6009): first the
container-path stream alone (the first time `h264_container_to_annexb` ran on the real file), then all 10 tests.

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

==================================================================
Full set, 2026-11-16 13:41 UTC — all 11 syn_h264 corpora generated in the ovt-oracles image with generate.py (each .ok sha256 matches tests/corpora/manifest.json; syn_h264_gop60_1080p with --check)
$ OVT_CORPORA_DIR=<dir> OVT_REQUIRE_CORPORA=1 ctest --preset macos-arm64-debug -R 'syntax_golden_h264_.*syn_h264' --output-on-failure
Test project /Users/ovt-build/mxf-main/build/macos-arm64-debug
      Start 111: syntax_golden_h264_trace_headers_syn_h264_multislice_cavlc_mp4
      Start 137: syntax_golden_h264_gstparse_syn_h264_multislice_cavlc_mp4
      Start 107: syntax_golden_h264_trace_headers_syn_h264_epb_dense
      Start 108: syntax_golden_h264_trace_headers_syn_h264_gop60_1080p
 1/10 Test #107: syntax_golden_h264_trace_headers_syn_h264_epb_dense ..............   Passed    0.06 sec
      Start 109: syntax_golden_h264_trace_headers_syn_h264_hrd_cbr
 2/10 Test #111: syntax_golden_h264_trace_headers_syn_h264_multislice_cavlc_mp4 ...   Passed    0.09 sec
      Start 110: syntax_golden_h264_trace_headers_syn_h264_hrd_vbr
 3/10 Test #137: syntax_golden_h264_gstparse_syn_h264_multislice_cavlc_mp4 ........   Passed    0.09 sec
      Start 133: syntax_golden_h264_gstparse_syn_h264_epb_dense
 4/10 Test #109: syntax_golden_h264_trace_headers_syn_h264_hrd_cbr ................   Passed    0.07 sec
      Start 134: syntax_golden_h264_gstparse_syn_h264_gop60_1080p
 5/10 Test #133: syntax_golden_h264_gstparse_syn_h264_epb_dense ...................   Passed    0.04 sec
      Start 135: syntax_golden_h264_gstparse_syn_h264_hrd_cbr
 6/10 Test #108: syntax_golden_h264_trace_headers_syn_h264_gop60_1080p ............   Passed    0.15 sec
      Start 136: syntax_golden_h264_gstparse_syn_h264_hrd_vbr
 7/10 Test #110: syntax_golden_h264_trace_headers_syn_h264_hrd_vbr ................   Passed    0.12 sec
 8/10 Test #135: syntax_golden_h264_gstparse_syn_h264_hrd_cbr .....................   Passed    0.08 sec
 9/10 Test #134: syntax_golden_h264_gstparse_syn_h264_gop60_1080p .................   Passed    0.15 sec
10/10 Test #136: syntax_golden_h264_gstparse_syn_h264_hrd_vbr .....................   Passed    0.12 sec

100% tests passed out of 10

Label Time Summary:
fast             =   0.97 sec*proc (10 tests)
golden           =   0.97 sec*proc (10 tests)
needs_corpus     =   0.97 sec*proc (10 tests)
syntax_golden    =   0.97 sec*proc (10 tests)

Total Test time (real) =   0.37 sec
rc=0
```
