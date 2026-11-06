# WI-0.552b acceptance record

Second half of WI-0.552 (§16.2.6; WI-0.552a landed the tooling). This PR records the sha256 and size of the
fast-lane entries on the Mac Mini, regenerates `tests/corpora/LICENSES.md`, and records the fast-subset fetch.

Machine: Mac Mini (Apple silicon, macOS 15), Python 3.15.2. Date: 2026-11-06. Spec version: 1.0-rc4.
The fetch and the tools tests used `.venv/bin/python` (it provides `jsonschema`; the system `python3` does not).
Nothing was installed.

## Step 1: `python3 tools/manifest_add.py fill --jobs 4`

77 of 85 attempted entries filled (exit 3: eight returned HTTP 404 and stay unverified):
`jvt_ALPHACONFORMANCEG`, `jvt_src19td_IBP_264`, `jvt_CVFC1_SVA_C`, `jvt_MVCNV_1`, `jctvc_DBLK_A_SONY_4`,
`jctvc_INITQP_B_Sony_1`, `jctvc_LTRPSPS_A_FJ_1`, `jctvc_GENERAL_444_8bit_RExt_Sony_2`.

## Step 2: `python3 tools/license_gate.py --policy tools/license_policy.toml --corpora --write-licenses-md`

`license gate: PASS (0 findings, 0 components)`. `LICENSES.md` is unchanged: filling adds hashes only, no
licence or source changes.

## Retagging to stay inside the 300 MiB budget

With the filled entries the hashed `fast` entries totalled 656,559,179 bytes (626.1 MiB), over the 314,572,800 byte
budget, and `test_fast_subset_stays_within_the_ci_budget` failed. The budget is unchanged. The 13 largest `fast`
entries were retagged from `fast` to `slow` (the existing non-fast tag, §14.3.2), skipping any entry that would
leave a codec or container without a `fast` stream. 380,795,642 bytes (363.2 MiB) moved.

| Entry | size_bytes |
|---|---|
| `jvt_LS_SVA_D` | 58,030,932 |
| `jvt_HPCAFL_BRCM_C` | 54,372,573 |
| `jvt_HPCV_BRCM_A` | 49,036,847 |
| `mkv_test5` | 31,762,747 |
| `jvt_CI1_FT_B` | 24,960,033 |
| `mkv_test1` | 23,339,337 |
| `jvt_CAPM3_Sony_D` | 22,917,826 |
| `mkv_test3` | 21,061,472 |
| `jvt_CABAST3_Sony_E` | 20,390,390 |
| `jvt_CVSEFDFT3_Sony_E` | 18,805,508 |
| `jvt_CVSE2_Sony_B` | 18,798,970 |
| `jvt_CVWP1_TOSHIBA_E` | 18,679,933 |
| `jvt_CVSE3_Sony_H` | 18,639,074 |

`fast` verified total after retagging: 275,535,205 bytes (262.8 MiB); 190 `fast` entries (was 203).

## Step 3: `python3 tools/fetch_corpora.py --tags fast --max-total-bytes 314572800`

Run with an empty cache (`OVT_CORPORA_DIR` pointing at a new directory) using `.venv/bin/python`.

```console
exit code: 0
ok: 110 entries; fetched cache: 113 files, 302,409,096 bytes (288.4 MiB, includes the .zip archives)
skip: 80 "not fetchable by URL (manual download, vcs or hash pending)"; 0 budget skips
```

The 80 skips are the synthetic (`syn_*`) entries and the entries still unverified (the 8 HTTP 404 above plus
entries with no URL: aom Annex B/`av1c`, cta, dashif, apple adv, hdr, bento4).

## Checks

- `python3 tools/check_fast_subset.py` and `--all-chapters`: `0 problem(s)` each.
- `pytest tools`: `test_fast_subset_stays_within_the_ci_budget` passes. 13 failures in `tools/tests/test_normalizers.py`
  (`ModuleNotFoundError: numpy` in `.venv`) are identical on `main` and are not caused by this change.
