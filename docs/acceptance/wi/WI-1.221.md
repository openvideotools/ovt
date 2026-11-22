# WI-1.221 acceptance record

Spec version: see the PR body. Part a (PR #809) covers the samples of the `moov` part.

## Test 2: `syn_mp4_cenc_cbcs` gives `iv == tenc.default_constant_IV` and 16 encrypted / 144 clear bytes

Test: `cenc: syn_mp4_cenc_cbcs uses the constant IV and shades 16 of every 160 bytes` (`[container][cenc][needs-corpus]`).

Machine: Mac Mini, Apple M4 (arm64), macOS 15.8.2, preset `macos-arm64-debug`. Date: 2026-11-22, head `2ada0e6`
(also run on `4268d9c`, same result; only `cenc_test.cpp` differs between the two, by removed in-memory tests).

Corpus: `generate.py --only syn_mp4_edit_list_frag_cenc --check` in the oracle image, rc 0, every file matched the
manifest (`syn_mp4_cenc_cbcs.mp4` `6002ee8e057907d859960af3db0f143f7242327fb1bf5dc3c9e7eca9ad849352`, set hash
`27bba685b80834dc6b5d500af4cdc4dd48dbc207c48e3186efa6c3478283bf43`).

    $ OVT_CORPORA_DIR=<cache> OVT_REQUIRE_CORPORA=1 ovt_tests_container "cenc: syn_mp4_cenc_cbcs*"
    All tests passed (16808 assertions in 1 test case)

## Test 1: IVs equal the mp4ff golden

`cenc: mpegff_video_cenc_128iv gives the IVs and subsamples of the mp4ff golden`, run on the real file
(SHA-256 checked by `fetch_corpora.py`) with `OVT_REQUIRE_CORPORA=1`, Linux x86_64 container, 2026-11-22: passes.
The golden is `tests/golden/cenc/mpegff_video_cenc_128iv.mp4ff-senc.json` (oracle source `tests/oracles/mp4ff_senc/`).
