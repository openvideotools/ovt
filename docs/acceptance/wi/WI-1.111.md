# WI-1.111 acceptance record: H.264 POC and frame boundary against JM

The acceptance tests are automated Catch2 cases in `core/tests/index/test_index_h264_poc.cpp`. They are tagged
`[needs-corpus]`, and the AT2 and AT3 cases are also `[slow]` because their streams are slow-tagged. They skip when
the corpus is absent: itu.int is blocked from cloud containers, so this record is the run on the Mac Mini runner.

- Date: 2026-11-15
- Machine: Mac mini Mac16,10 (Apple M4), macOS 15.8.2, preset `macos-arm64-release` (AppleClang 17)
- Source: `claude/project-thread-9wmb1j` @ f9053bc (WI-1.111a) with the WI-1.111b patch applied. The code is identical
  to this PR's.
- Environment: `OVT_CORPORA_DIR` held 38 fetched JVT entries, including `jvt_CVPCMNL1_SVA_C`, `jvt_MR6_BT_B`,
  `jvt_MR9_BT_B`, `jvt_CVFI1_Sony_D` and `jvt_sp1_bt_a`. `OVT_REQUIRE_CORPORA=1` was set.

```text
$ ovt_tests_index_h264 "*"
All tests passed (83694 assertions in 23 test cases)

$ ovt_tests_index_h264 "[needs-corpus]" -r compact
All tests passed (3402 assertions in 4 test cases)
```

| AT | Test | Result |
|---|---|---|
| 1 | `h264_poc_unit: CVPCMNL1_SVA_C and MR6_BT_B per-picture POC equal JM` (also `MR9_BT_B`) | pass (30, 120 and 64 pictures) |
| 2 | `h264_poc_mmco5: sp1_bt_a POC after mmco5 equals JM` (#568: replaces MR9_BT_B, which has no mmco5) | pass (39 mmco5 pictures, 400 pictures) |
| 3 | `h264_poc_fields: CVFI1_Sony_D has one picture per field` | pass (34 field pictures, 17 pairs) |

Oracle: stock JM 19.0 `ldecod` and FFmpeg 7.1.1 `-debug pict`, also run on the Mini on 2026-11-15. How
`core/tests/index/jm_poc_expected.h` was built from their output is described at the top of that file.
