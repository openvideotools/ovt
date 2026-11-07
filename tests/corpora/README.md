<!-- SPDX-FileCopyrightText: 2026 The Open Video Tools contributors -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
# tests/corpora

`manifest.json` is the single list of test media (chapter 14 §14.2.4). No media is committed: `tools/fetch_corpora.py`
downloads and hash-verifies files into a cache. `LICENSES.md` is generated from the manifest by
`tools/license_gate.py --policy tools/license_policy.toml --corpora --write-licenses-md`.

## Container entries (chapter 8 §8.9.3, WI-1.237)

| Tag | Content | Size | Fetch |
|---|---|---|---|
| `container-smoke` | 20 files, one per feature: ISOBMFF (fragmented, CENC, HEVC `hev1`/`hvc1`, Dolby Vision CENC, H.264 UGC), HEIF, AV1 IVF, H.264 and H.265 Annex B. All also carry `fast`. | 6.5 MiB | `tools/fetch_corpora.py --tags container-smoke` |
| `container-full` | every other verified ISOBMFF entry of the MPEG FileFormatConformance, Bento4 and GPAC sets | 162 files, 125 MiB | `tools/fetch_corpora.py --tags container-full` |
| `isobmff-broken` | seven generated broken MP4 files (recipes 34 to 37 and 72, `tests/corpora/synthetic/recipes.json`); each names the codes the reader must emit | tiny, generated | `tests/corpora/synthetic/generate.py` |

Every entry of these tags has an `expected` object read by the container tests:

```json
{ "format": "isobmff|ivf|annexb|matroska|mpegts", "codec": "H264|H265|AV1",
  "hdr_kind": "<HdrKind>", "diagnostics": ["CNT-nnnn"] }
```

`codec`, `hdr_kind` and `diagnostics` appear only where known. HEIF files report `format: isobmff`.

Redistribution: most container media is licensed for testing only (`redistributable: false`, see `LICENSES.md`). Never
commit it, mirror it or attach it to a release. Dolby and CTA WAVE clips that need a login or are large are
not tagged yet; their entries stay `unverified` until a mirror is pinned.
