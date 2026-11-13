# WI-0.567 acceptance record

Machine: GitHub-hosted ubuntu-24.04 runner, `ghcr.io/openvideotools/ovt-oracles:main`. Workflow: `goldens-h26x`
(`.github/workflows/goldens-h26x.yml`), which runs `tools/goldens/gen_h26x.py` twice and requires the two outputs to
be byte-identical. Spec version: 1.0-rc14. Commit: the head of the WI-0.567 pull request.

## Test 1: the cross-oracle self-check finds zero undocumented disagreements

`gen_h26x.py` runs `tools/goldens/h26x_selfcheck.py` on every stream that has both a `trace_headers` and a
`gstparse` golden: 86 streams, 16 770 shared members compared (a member both oracles name, in units whose NAL unit
types match), 0 disagreements. `tests/golden/KNOWN_DIFFS.md` therefore has no rows. The per-stream counts are in
`tests/golden/h26x-index.json` (`selfcheck`). The job fails when any stream has a disagreement that is not a row of
`KNOWN_DIFFS.md`.

## Produces

- `tests/golden/h264/*.{trace_headers,gstparse,jm-trace,ffmpeg-mvs}.json.gz`: 86 trace_headers + 88 gstparse (H.264
  and HEVC together), 22 jm-trace, 22 ffmpeg-mvs; `tests/golden/h265/*.{trace_headers,gstparse,hm-grid}.json.gz`
  with 32 hm-grid; `tests/golden/epb/syn_h264_epb_dense.json`.
- `tests/golden/h26x-index.json` lists, per corpus id, the goldens, the number of pictures each covers, the SHA-256 of
  the corpus file and every skipped oracle or entry with its reason.

## Not covered

- 25 HEVC streams have no `hm-grid` golden because even one picture of the grid exceeds 1 MB (1080p); 2 more
  entries are skipped for the reasons in the index (an FFmpeg limitation each, an HM assertion on SEI type 200, tracks of
  another codec in the file).
- 18 manifest entries are not processed (no recorded hash, encrypted, image items, segments): `skipped_entries`.
