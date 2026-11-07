# WI-0.553 acceptance record

WI-0.553 (synthetic stream generator) is split per §16.2.6. The parts are:

- 0.553a: #121, merged.
- 0.553b: #162, merged, and part b2, this record's first PR.
- 0.553c: still to come.

This file records where each acceptance test stands. Automated tests are named by their pytest node.

| AT | Status | Where |
|---|---|---|
| 1. `generate.py --check` in the oracle image, every `syn_*` hash equals the manifest | open | part c (oracle-image run on the Mac Mini runner) |
| 2. encoder version differs from the pin, exit 5, no file written | pass | `tools/tests/test_generate_synthetic.py` (#121, extended in #162) |
| 3. `syn_av1_hidden_altref_showexisting.ivf`: `inspect -A` has `showFrame: 0`, and `aomdec` outputs more frames than `inspect` shows | open | part c (needs `aomdec` and `inspect` from the oracle image) |
| 4. `ovt syntax --hex` reports ≥ 20 `epb_positions` on `syn_h264_epb_dense.264`; `ovt metrics --align auto` reports `offset_frames == +7` on `syn_align_drop7` | **deferred** | see below |
| 5. `check_fast_subset.py --all-chapters` exits 0 | pass | `tools/tests/test_generate_synthetic.py` (#121) |

## AT 4: deferred

AT 4 checks OVT's own CLI on two generated streams. Neither command exists on `main` yet:

- `ovt syntax --hex` (`epb_positions`) is produced by **WI-1.504** (chapter 12, `ovt syntax` with `--trace`;
  its step 4 is `--hex` with EPB markers).
- `ovt metrics --align auto` (`offset_frames`) needs **WI-3.304** (temporal alignment) and **WI-3.530**
  (`ovt metrics` and `ovt compare`).

The two streams, `syn_h264_epb_dense` and `syn_align_drop7`, are produced by WI-0.553 (§14.3.3 recipes 22
and 24). The test stays with WI-0.553 and runs once those WIs merge. Until then this AT is the only one of
WI-0.553 that is not closed.

The non-stream fixtures `render/tests/fixtures/real_1080p_hevc_frame.fb` and `real_4k_hevc_frame.fb`
(`recipes.json` `fixtures`) are made with `ovt analyze`, which is **WI-2.174**. `generate.py --fixtures` builds
them once that WI merges.

No GitHub issue exists yet for WI-1.504, WI-2.174, WI-3.304 or WI-3.530 (checked 2026-11-06). The
dependency is recorded here and in the project memory, so the session that implements any of them re-runs
this AT.
