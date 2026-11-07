# WI-0.553 acceptance record

WI-0.553 (synthetic stream generator) is split per §16.2.6. The parts are:

- 0.553a: #121, merged.
- 0.553b: #162 (b1), merged, and #171 (b2, helpers).
- 0.553c1: FlatBuffers fixtures (`gen_fixture_fb.py`, `gen_dense_fa.py`), the `syn_raw_<ctag>` family
  (`raw_formats.json`) and the `syn_broken_syntax_<code>` family (`corrupt_conditions.json`).
- 0.553c2: the `*_expected.json` sidecar writers and the oracle hash run (AT 1, AT 3), still to come.

This file records where each acceptance test stands. Automated tests are named by their pytest node.

| AT | Status | Where |
|---|---|---|
| 1. `generate.py --check` in the oracle image, every `syn_*` hash equals the manifest | open | part c2 (oracle-image run on the Mac Mini, built under amd64 emulation; waits for the owner's OK to start Docker there) |
| 2. encoder version differs from the pin, exit 5, no file written | pass | `tools/tests/test_generate_synthetic.py` (#121, extended in #162) |
| 3. `syn_av1_hidden_altref_showexisting.ivf`: `inspect -A` has `showFrame: 0`, and `aomdec` outputs more frames than `inspect` shows | open | part c2 (needs `aomdec` and `inspect` from the oracle image) |
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

## Part c1 notes

The FlatBuffers fixtures are not committed. `generate.py --fixtures` writes them to their repository paths,
and the manifest holds one `kind: "fixture"` entry per file. The hashes are recorded by the same run as the
stream hashes (part c2). `tools/tests/test_fixture_fb.py` reads the buffers back with a schema-driven reader and
checks the V-rules they can break. V-032 (canonical bytes) can only be checked once `canonicalize()` exists
(WI-0.085; field order question #175). The `pat_*.fb` pattern fixtures wait on #173.

Each `corrupt_conditions.json` edit was checked on streams from the local x264, x265 and libaom (FFmpeg 6.1
`trace_headers` and decode logs, 2026-11-06). The rewritten field reads back with the new value
(`vps_reserved_0xffff_16bits = 0`, `reserved_zero_2bits = 3`, `level_idc = 10`,
`first_slice_segment_in_pic_flag = 0`, `nuh_temporal_id_plus1` rejected). FFmpeg also reports the matching
condition where it has one: "non-existing PPS 0", "non-existing SPS 0 referenced in buffering period",
"Could not find ref with POC 5", and "Error parsing OBU data" for the truncated AV1 frame. The codes OVT
itself reports are checked by the syntax WIs that consume the family.
