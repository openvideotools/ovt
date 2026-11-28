# WI-1.213 acceptance record

Parts: a (#801, merged) and b (#807). Spec version: 1.0-rc46.

Acceptance lines AT2 and AT3 and the unit-level AT1 are automated tests (see the PR bodies). This record
covers AT1 against the real corpus file, which CI does not run.

## AT1: Widevine `pssh` in `bento4_widevine.mp4` (`syn_mp4_cenc_widevine`, recipe 68)

Given `bento4_widevine.mp4`, When decoded, Then `pssh.SystemID` derived `"Widevine"` and the protobuf
`key_id` equals `tenc.default_KID`.

Automated: `bento4_widevine: the Widevine pssh names the system and its key_id equals tenc.default_KID`
(`core/tests/container/decoders_encryption_test.cpp`, `[container][decoders][cenc][needs-corpus]`).

Date: 2026-11-22 14:15 UTC. Head: e32c23a. Machine: Mac Mini, Apple M4 (arm64), macOS 15.8.2.
Preset: `macos-arm64-debug`. Bento4 1.6.0.0 (`mp4encrypt` 1.7) from the local oracle image.

```console
$ generate.py --only syn_mp4_cenc_widevine --check      # inside the oracle image
generate syn_mp4_cenc_widevine
c6fe0640a2cb7d7561a40671b5e6334a9360602eeb267e86b4437361ca02e21d  syn_h264_multislice_cavlc
670a81ee31d36199d2d994e2d34a32789e71b7ca51330706d1b4ed4a997df72d  syn_h264_multislice_cavlc_mp4
9cf96f8ac42e87ad30ebc235b1e8f0f8fb4334eb715be609a6fd80d1c8ff6b52  syn_mp4_cenc_widevine
generate rc=0

$ OVT_CORPORA_DIR=<generated dir> OVT_REQUIRE_CORPORA=1 ctest --preset macos-arm64-debug -R bento4_widevine --output-on-failure
1/1 Test #520: bento4_widevine: the Widevine pssh names the system and its key_id equals tenc.default_KID ...   Passed    0.01 sec
100% tests passed out of 1

$ <direct run of the test binary>
All tests passed (19 assertions in 1 test case)
```

Result: Passed. The generated hashes match `tests/corpora/manifest.json`.
