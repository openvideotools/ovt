# WI-4.105 acceptance record: ovt-plugin-hm (part a)

Part a of WI-4.105 (chapter 07 §7.6.4). AT1 and AT3 run on the real corpora on the Mac Mini; AT2 needs the part b
mapping and the WI-4.106 comparator and is not covered here.

## Mac Mini run, 2026-11-14

- **Machine:** ShafferDix-Mini, Apple silicon (arm64), macOS 15.8.2, user `ovt-build`.
- **Commit:** branch `claude/project-thread-56e2sb` at 12ae915 (rerun; the first run at e89fe13 found the EOS
  feeder bug and two wrong `hashes` flags, fixed in c717fd7).
- **Corpora:** the 40 `jctvc_*` ids of `kAt1Streams` fetched with `tools/fetch_corpora.py` into `~/wi4105/corpora`;
  `syn_h265_422_444_rext` (recipe 11) generated in a locally built oracle image with
  `tests/corpora/synthetic/generate.py --only syn_h265_422_444_rext --check` (hashes match the manifest).

```console
$ cmake --preset macos-arm64-release -DOVT_BUILD_PLUGIN_HM=ON
$ cmake --build --preset macos-arm64-release --target ovt_plugin_hm_tests
$ OVT_CORPORA_DIR=~/wi4105/corpora OVT_REQUIRE_CORPORA=1 ctest --test-dir build/macos-arm64-release -R "hm plugin" --output-on-failure
1/3 Test #748: hm plugin: malformed hvcC, units and arguments are rejected .......   Passed    0.55 sec
2/3 Test #749: hm plugin: hm_md5_vs_sei on the HEVC_v1 conformance streams .......   Passed   31.57 sec
3/3 Test #750: hm plugin: RExt 4:4:4 12-bit yields a FrameAnalysis per picture ...   Passed    0.60 sec
100% tests passed out of 3
```

**AT1 passes** on all 40 HEVC_v1 streams (90801 assertions), including RAP_B_Bossen_2. **AT3 passes**
(`syn_h265_444_12bit.265`).

## Part b: Mac Mini run, 2026-11-14

- **Machine:** as above.
- **Commit:** branch `claude/project-thread-56e2sb` at 172e6d7, built with the preset's `-Werror`. Earlier runs
  found an unused constant under AppleClang and a test that required whole-stream goldens (both fixed in a5f1273),
  then MVCLIP_A frame 1 MVs wrapped instead of clamped to ±32767 (chapter 06 §6.2.3, fixed in 172e6d7).
- **Corpora:** as above, plus the 32 committed `tests/golden/h265/*.hm-grid.json.gz` goldens.
- **Commands:** the three commands of part a.

**Result as reported by the Mini session:** `ctest -R "hm plugin"` passes 6 of 6:
`caps and ABI table`, `two instances decode interleaved`, `malformed hvcC, units and arguments are rejected`,
`RExt 4:4:4 12-bit yields a FrameAnalysis per picture`, `hm_md5_vs_sei on the HEVC_v1 conformance streams`
(56.9 s, now also running the FrameAnalysis validator on every picture) and
`CellGrid matches hm_cu_dump on the hm-grid goldens` (22.4 s; MVCLIP_A matches with the clamp and MDL-GEN-0003).

### Part b rerun at 6075cca (4:2:2 chroma cbf), 2026-11-14

- **Machine and commands:** as above, with `OVT_REQUIRE_CORPORA=1`, the 40 HEVC_v1 streams and both
  `syn_h265_422_444_rext` streams; build with `-Werror`.
- **Result as reported by the Mini session:** `ctest -R "hm plugin"` passes 7 of 7: the new
  `4:2:2 HevcTu chroma cbf has one entry per chroma TB` (0.76 s), `hm_md5_vs_sei on the HEVC_v1 conformance streams`
  (60.2 s), `CellGrid matches hm_cu_dump on the hm-grid goldens` (25.6 s), `RExt 4:4:4 12-bit yields a FrameAnalysis
  per picture`, `two instances decode interleaved`, `caps and ABI table` and
  `malformed hvcC, units and arguments are rejected`.

## Part c1: Mac Mini run, 2026-11-15

- **Machine:** as above.
- **Commit:** branch `claude/project-thread-56e2sb` at d7ed383 (PR #543), `macos-arm64-release` with
  `-DOVT_BUILD_PLUGIN_HM=ON` and the preset's `-Werror`.
- **Corpora and commands:** as for part b, with `OVT_REQUIRE_CORPORA=1`. To run only the new case through Catch2,
  escape the comma in its name: `"hm plugin: partitions\, filter parameters and RPS match the slice headers"`.

**Result as reported by the Mini session:** `ctest -R "hm plugin"` passes 8 of 8. The new
`partitions, filter parameters and RPS match the slice headers` takes 34.3 s and passes all 202054 assertions over
its 16 HEVC_v1 streams. Among them, 245 of the 300 pictures of RPS_E_qualcomm_5 have long-term references,
DSLICE_A_HHI_5 has 700 dependent slice segments, and TILES_B_Cisco_1, WPP_A_ericsson_MAIN_2 and CAINIT_A_SHARP_4
are covered too.
