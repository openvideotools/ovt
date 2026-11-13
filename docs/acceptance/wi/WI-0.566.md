# WI-0.566 acceptance record

Machine: GitHub-hosted ubuntu-24.04 runner, `ghcr.io/openvideotools/ovt-oracles:main` (libaom 3.12.1 `inspect`
and `aomdec`). Workflow: `goldens-av1` (`.github/workflows/goldens-av1.yml`) runs `tools/goldens/gen_av1.py` twice,
requires the two outputs to be byte-identical and requires every committed `tests/golden/av1/*.gz` and
`tests/golden/av1-index.json` to be byte-identical to the generated one. Spec version: 1.0-rc22.

## Test 1: hidden frames in `syn_av1_hidden_altref_showexisting` — NOT MET under the 2 MB cap

Given `syn_av1_hidden_altref_showexisting.ivf`, When normalised, Then `shown=false` entries exist. The first
temporal unit of that 720p stream normalises to 4 678 344 bytes, over the 2 MB limit of CLAUDE.md, so no golden is
committed for it (`tests/golden/av1-index.json`, `skipped`). The same is true of every 720p/1080p `syn_av1_*` stream.
`tools/tests/test_golden_inspect_selfcheck.py::test_hidden_altref_showexisting_has_hidden_frames` skips with that
reason and starts to run as soon as a golden for the stream exists (`gen_av1.py --max-bytes` raises the cap).
The rule is Karl's to change (coordinator asked, 2026-11-07).

## Step 5: frame counts against `aomdec --summary`

`gen_av1.py` runs `aomdec --summary` on the same IVF prefix as `inspect` and `av1_selfcheck.py` checks decoded
frames == temporal units, `show_existing_frame` outputs (aomdec shown frames minus shown `inspect` frames) >= 0 and
non-zero accounting bits in every shown frame. 14 goldens, 0 disagreements. `test_golden_inspect_selfcheck.py` repeats
the check from the committed goldens and the counts in the index.

## Produces

`tests/golden/av1/*.aom-inspect.json.gz`: 14 goldens (13 `aom`, 1 `syn_av1_444_422_profile2`), each 1 to 2 temporal
units. 23 `aom` and `syn_av1_*` entries are left out because one temporal unit exceeds 1 900 000 bytes
(`skipped` in the index with the size of each); 6 `aom` entries have no recorded hash and 2 are `local_only`.
