# WI-1.220 acceptance record

Spec version 1.0.0-rc46. Command for the corpus sections (needs the corpus cache, `OVT_REQUIRE_CORPORA=1`):

```sh
OVT_REQUIRE_CORPORA=1 ctest --preset linux-x64-debug -R "edit list: the corpus files of the acceptance tests"
```

| AT | Automated test | Corpus run (command output, date, machine) |
|---|---|---|
| 1 | `edit list: a B-frame delay removed by elst is one normal segment (x264 default)` (synthetic, fast); corpus section `syn_h264_weightedpred_ltr` | PASS 2026-11-22, Apple M4 / macOS 15.8.2 (Mac Mini), `macos-arm64-debug`, head b267666674a3726f834f75d4476ff589f14152c2, 15 of 15 assertions. Corpus `.mp4` regenerated with MP4Box (sha256 edfa1527b0deca39476a7c311a67df33720910660f24213bcbcff39490c2b74c): `elst` MediaTime 2, ctts present. Earlier heads failed because the ffmpeg-remuxed member had no ctts. |
| 2 | `edit list: a QuickTime empty edit delays the media by its duration` (synthetic, fast); corpus section `syn_mp4_elst` (entry `syn_mp4_edit_list_frag_cenc`) | PASS, same run: segments `[empty, normal]`, pts == the empty edit duration. |
| 3 | `elst ↔ tfdt: CNT-3010 when media_time matches neither T0+C0 nor C0` (direct `FragmentBuild`) | pending WI-1.224b: the hand-broken `syn_cmaf_bad_elst_cmfv` file joins the `isobmff-broken` set there |
