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
