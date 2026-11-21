# WI-2.435 acceptance record

Machine: cloud container, Ubuntu 24.04 x86_64 (4 cores), GCC 13.3.0, system libplacebo 6.338.2, Mesa 25.2.8 lavapipe.
Date: 2026-11-17. Spec version: 1.0-rc41. Spec questions: #712 (id buffer clear value), #713 (compare-view picking).

Both acceptance tests are automated in `render/tests/test_pick.cpp` and run in the fast lane on every platform with a
Vulkan device (lavapipe on Linux CI, MoltenVK on macOS):

| AT | Test | Tags |
|---|---|---|
| 1 | `render_test_pick_mv_index_at_arrow_tip (AT1)` | `[fast][vulkan][render][pick]` |
| 2 | `render_test_pick_pq_1000_nits (AT2)` | `[fast][vulkan][render][pick]` |

AT1 renders the §10.9.2 synthetic MV fixture (`make_synthetic_mv(3840, 2160)`, the in-memory twin of
`synthetic_4k_mv.fb`) at zoom 8 around the picture centre, waits for the id-buffer readback, and picks a pixel 2 px
inside the head of four arrows. `mv_index` equals each arrow's index in `FrameAnalysis.mvs`. AT2 picks a 10-bit
limited-range BT.2020 PQ grey of code 723: `nits_maxrgb` = 1000 ± 1 %.

The other cases cover the step list: CPU colour math (PQ round trip, BT.709 white = 203 nits at D65 xy, HLG 75 % = 203
nits), the CPU block-id lookup with all layers hidden, sample-window reuse within 32 samples, readback buffer reuse
over back-to-back renders, invalidation when the layers or the overlay set go away, and the 1×1 region fallback
(step 6, forced on Vulkan).

```console
$ cmake --build --preset linux-x64-debug --target ovt_tests_render
$ ./render/tests/ovt_tests_render "[pick]"
All tests passed (104 assertions in 8 test cases)
$ ./render/tests/ovt_tests_render "[pick]"      # second run, same result
All tests passed (104 assertions in 8 test cases)
$ ./render/tests/ovt_tests_render                # whole render suite
All tests passed (142454 assertions in 130 test cases)
```

## MoltenVK (Mac Mini)

Machine: Mac Mini, Apple M4, macOS 15.8.2, macos-arm64-debug, pinned libplacebo 7.349.0. Date: 2026-11-17.

At e61ab97 `render_test_pick_block_id_and_codes` failed: every probe luma read was 63 codes off. The cause is in the
pinned libplacebo: its Vulkan transfer passes the rect's end coordinates as the copy extent, so any download whose rect
does not start at (0, 0) reads the wrong texels. Upstream has since fixed it. b2212b3 stages sub-rect downloads at the
origin (`stage_rect` in `render/src/overlay/idbuffer.cpp`). The same code was audited in issue #715.

```console
$ ./render/tests/ovt_tests_render "[pick]"      # b2212b3, three runs
All tests passed (104 assertions in 8 test cases)
All tests passed (104 assertions in 8 test cases)
All tests passed (104 assertions in 8 test cases)
```

Full log: `wi2435-pick-pr714-b2212b3-mini-20261117T020317Z.txt` in the project files.
