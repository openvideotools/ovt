# WI-1.412 acceptance record

Spec version: 1.0-rc37. Spec questions: #637 (mDCV/cLLi, lodepng's native chunk fields, null view, ViewPng8 alpha,
AT2 reference), #638 (PR size with vendored lodepng).

## Test 1: `pq_patches_1000`, `HdrPng16` → `cICP = 09 10 00 01`, 1000-nit patch within ±32/65535 of 0.7518

Automated: `render_test_export: HdrPng16 of pq_patches_1000 has cICP 09 10 00 01 and PQ 0.7518 at 1000 nits (AT1)`
in `render/tests/test_export.cpp`. The picture is frame 0 of `syn_pat_pq_patches_1000` built from
`tests/corpora/patterns/pq_patches_1000.expected.json` (the lossless encode decodes to these codes; see
`docs/acceptance/wi/WI-4.452.md`). The test re-reads the PNG and checks IHDR (1920×1080, 16-bit, colour type 2), the
`cICP` payload and its position before `IDAT`, and R, G and B of the 1000-nit column at three rows. It also checks that
a second export gives the same bytes.

Local run (2026-11-16, cloud container, Ubuntu 24.04, lavapipe, system libplacebo 6.338): all 12
`render_test_export:` cases pass, and the whole `ctest -R render_` set passes (101/101).

## Test 2: `SdrPng8` with BT.2390 vs the mpv reference (`imgdiff.py --mode hdr-parity`)

**Deferred to WI-1.575** (#637 item 5). The mpv `gpu-next` references of chapter 14 §14.4.15
(`tests/corpora/render-ref/*.png`) are produced by `tests/oracles/mpv_capture.py` of WI-1.575, which depends on this work
item, so no reference exists yet. WI-1.575's parity suite exports through `SdrPng8` and runs this comparison.

What this work item checks automatically instead:
- `render_test_export: SdrPng8 equals the Srgb8 render at 1:1`: the export's RGB codes match the on-screen offscreen
  render of the same picture within 1 code;
- `render_test_export: SdrPng8 of pq_patches_1000 with BT.2390 is reproducible and tone-mapped (AT2 precondition)`:
  byte-identical repeat exports, neutral patches, monotonic in nits, 4000 and 10000 nits not clipped together.
