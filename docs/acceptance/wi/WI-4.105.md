# WI-4.105 acceptance record: ovt-plugin-hm (part a)

Part a of WI-4.105 (chapter 07 §7.6.4). AT1 and AT3 run on the real corpora on the Mac Mini; AT2 needs the part b
mapping and the WI-4.106 comparator and is not covered here.

## Mac Mini run, 2026-11-14

- **Machine:** ShafferDix-Mini, Apple silicon (arm64), macOS 15.8.2, user `ovt-build`.
- **Commit:** branch `claude/project-thread-56e2sb` at e89fe13.
- **Corpora:** the 40 `jctvc_*` ids of `kAt1Streams` fetched with `tools/fetch_corpora.py` into `~/wi4105/corpora`;
  `syn_h265_422_444_rext` (recipe 11) generated in a locally built oracle image with
  `tests/corpora/synthetic/generate.py --only syn_h265_422_444_rext --check` (hashes match the manifest).

```console
$ cmake --preset macos-arm64-release -DOVT_BUILD_PLUGIN_HM=ON
$ cmake --build --preset macos-arm64-release --target ovt_plugin_hm_tests
$ OVT_CORPORA_DIR=~/wi4105/corpora OVT_REQUIRE_CORPORA=1 ctest --test-dir build/macos-arm64-release -R "hm plugin" --output-on-failure
1/5 Test #540: hm plugin: caps and ABI table .....................................   Passed    0.13 sec
2/5 Test #541: hm plugin: hm_md5_vs_sei on the HEVC_v1 conformance streams .......Subprocess aborted***Exception:  38.78 sec
3/5 Test #542: hm plugin: two instances decode interleaved .......................   Passed    0.63 sec
4/5 Test #543: hm plugin: malformed hvcC, units and arguments are rejected .......   Passed    0.01 sec
5/5 Test #544: hm plugin: RExt 4:4:4 12-bit yields a FrameAnalysis per picture ...   Passed    0.75 sec
80% tests passed, 1 tests failed out of 5
```

**AT3 passes** (test #544, `syn_h265_444_12bit.265`).

**AT1 fails** with three findings (40/40 streams present):

1. `jctvc_FILLER_A_Sony_1`: `CHECK( !r.expected.empty() )` fails at `test_hm_plugin.cpp:378`. The stream carries no
   decoded picture hash SEI: its SEI NAL units are all prefix SEIs with payload types 0, 1, 6 and 129 (buffering
   period, picture timing, recovery point, active parameter sets), none of type 132. Its `kAt1Streams` entry should
   be `false`.
2. `jctvc_RAP_A_docomo_6`: the same check fails. The stream has no SEI NAL units at all. Its entry should be `false`.
3. `jctvc_RAP_B_Bossen_2`: HM aborts the test process:
   `Assertion failed: (rpcPic->getPOC() < this->getPOC()), function checkLeadingPictureRestrictions, file
   TComSlice.cpp, line 912.` Before it, HM prints `Short-term reference picture with POC = 24 (22, 20, 16) seems to
   have been removed or not correctly decoded.` Streams after RAP_B in the list did not run.

To cover the streams after RAP_B, the AT1 case was run again with `jctvc_RAP_B_Bossen_2` moved out of the corpus
directory (and `OVT_REQUIRE_CORPORA` unset): the only failures were the two missing-hash checks above and RAP_B's
missing file (`39/40 AT1 streams present`; 88470 assertions, 3 failed). So the other 37 streams, including the ten
after RAP_B (TSKIP_A, MVCLIP_A, MVEDGE_A, NoOutPrior_A, ipcm_A, RPS_B to RPS_F), pass.

**Fixed after this run (2026-11-14):** finding 3 was the test's feeder, not the plugin: it dropped NAL units shorter
than 3 bytes, so the 2-byte EOS_NUT never reached HM, the CRA after it decoded as a mid-sequence CRA and HM's
leading-picture assert fired (HM's `CommonDef.h` undefines `NDEBUG`, so its asserts are always on, as in
TAppDecoder). Findings 1 and 2: both entries are now `false`. AT1 needs a rerun on the Mini.
