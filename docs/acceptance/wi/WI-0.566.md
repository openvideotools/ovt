# WI-0.566 acceptance record

Machine: GitHub-hosted ubuntu-24.04 runner, `ghcr.io/openvideotools/ovt-oracles:main` (libaom 3.12.1 `inspect`
and `aomdec`). Workflow: `goldens-av1` (`.github/workflows/goldens-av1.yml`) runs `tools/goldens/gen_av1.py` twice,
requires the two outputs to be byte-identical and requires every committed `tests/golden/av1/*.gz` and
`tests/golden/av1-index.json` to be byte-identical to the generated one. Spec version: 1.0-rc22.

## Test 1: hidden frames in `syn_av1_hidden_altref_showexisting`

Given `syn_av1_hidden_altref_showexisting.ivf`, When normalised, Then `shown=false` entries exist and the symbol
sums of shown frames are non-zero. `tests/golden/av1/syn_av1_hidden_altref_showexisting.aom-inspect.json.gz` covers
4 temporal units: 7 frames, 4 of them hidden (`show_frame = 0`), and one `show_existing_frame` output.
`tools/tests/test_golden_inspect_selfcheck.py::test_hidden_altref_showexisting_has_hidden_frames` asserts the hidden
frames; `test_frame_counts_match_aomdec[...]` asserts non-zero bits in every shown frame (`av1_selfcheck.py`).

## Step 5: frame counts against `aomdec --summary`

`gen_av1.py` runs `aomdec --summary` on the same IVF prefix as `inspect` and `av1_selfcheck.py` checks decoded
frames == temporal units, `show_existing_frame` outputs (aomdec shown frames minus shown `inspect` frames) >= 0 and
non-zero accounting bits in every shown frame. 37 goldens, 0 disagreements. `test_golden_inspect_selfcheck.py` repeats
the check from the committed goldens and the counts in the index.

## Produces

`tests/golden/av1/*.aom-inspect.json.gz`: 37 goldens (21 `aom` fast entries, 16 `syn_av1_*`), 1 to 8 temporal units each,
the largest 1 074 586 bytes gzip-compressed (CLAUDE.md: at most 2 MB as committed; the generator also limits the JSON
to 40 MB). `tests/golden/av1-index.json` lists the counts per golden. Not processed: 6 `aom` fast entries with no
recorded hash (#160) and the 2 `local_only` perf streams (`syn_av1_10min_1080p`, `syn_av1_4k_2min`). A 1080p
10-bit stream (`syn_av1_filmgrain_10bit`, `syn_av1_pq_hdr_obu`, `syn_av1_hlg_metadata`) has one temporal unit.
