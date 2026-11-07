# WI-0.552b acceptance record

Second half of WI-0.552 (§16.2.6; WI-0.552a landed the tooling). This PR records the sha256 and size of the
fast-lane entries on the Mac Mini, adds `extract` members to the ITU entries, regenerates
`tests/corpora/LICENSES.md`, and records the fast-subset fetch.

Machine: Mac Mini (Apple silicon, macOS 15), Python 3.15.2. Date: 2026-11-06. Spec version: 1.0-rc4.
The fetch and the tools tests used `.venv/bin/python` (it provides `jsonschema`; the system `python3` does not).
Nothing was installed.

## Step 1: `python3 tools/manifest_add.py fill --jobs 4`

77 of 85 attempted entries filled (exit 3: eight returned HTTP 404 and stay unverified, see "404 entries").

## Step 2: `python3 tools/license_gate.py --policy tools/license_policy.toml --corpora --write-licenses-md`

`license gate: PASS (0 findings, 0 components)`. `LICENSES.md` is unchanged: filling adds hashes only, no
licence or source changes.

## ITU `extract` members

`sha256` and `size_bytes` are of the downloaded `.zip`. All 77 hashed `jvt`/`jctvc` entries now carry
`extract: {archive: "zip", member, md5_member?}`, chosen by listing every cached zip on the Mini: the Annex B member
by extension priority `.264 .h264 .26l .avc .jvt .bit .bin .bitstream .jsv` (`.jsv` only where it is the sole
bitstream, 11 entries; `jvt_SVCBST_1` takes the combined `SVCBST-1/SVCBST-1.264`). `jctvc` entries carry
`md5_member` = the `*_yuv.md5` file where the zip has one (it hashes the whole YUV); the two RExt entries whose zips
only hold `md5sum.txt` (`Main_422_10_A_RExt_Sony_2`, `Bitdepth_A_RExt_Sony_1`) have none. A fresh fetch now caches the
extracted bitstream (for example `jvt_BA_MW_D/BA_MW_D.264`, `jctvc_WPP_A_ericsson_MAIN_2/WPP_A_ericsson_MAIN_2.bit`
plus its `_yuv.md5`), with no `.zip` files in the cache.

## Retagging to stay inside the 300 MiB budget

SPEC-ASSUMPTION(#179): the named §14.3.2 list does not fit 300 MiB (see the issue). With the filled entries the
verified `fast` entries total 656,559,179 bytes (626.1 MiB), over the 314,572,800 byte budget, and
`test_fast_subset_stays_within_the_ci_budget` failed. The budget is unchanged. Kept in `fast`:
`CVWP1_TOSHIBA_E`, `CVSE2_Sony_B`, `CI1_FT_B`, `CVSE3_Sony_H`, `mkv_test1`, `mkv_test3`, `mkv_test5`. Moved from
`fast` to the existing non-fast tag `slow` (§14.3.2), uncited streams first, then those cited only by ch.7/ch.14,
keeping at least one `fast` stream per codec and container: 19 entries, 378,744,335 bytes (361.2 MiB).
Acceptance-cited streams among them: `HPCV_BRCM_A`, `CAPAMA3_Sand_F`, `CAMA1_Sony_C`, `CVPA1_TOSHIBA_B`,
`CVFI1_Sony_D`, `CAPM3_Sony_D`, `CABAST3_Sony_E`, `CVSEFDFT3_Sony_E`, `CAQP1_Sony_B`, `HPCAFL_BRCM_C`,
`sp1_bt_a`, `FM1_BT_B`, `BAMQ1_JVC_C`.

| Entry | size_bytes |
|---|---|
| `jvt_CAPM3_Sony_D` | 22,917,826 |
| `jvt_CVFC1_Sony_C` | 14,606,675 |
| `jvt_cvmp_mot_frm_L31_B` | 11,544,425 |
| `jvt_Sharp_MP_Field_1_B` | 8,973,251 |
| `jvt_LS_SVA_D` | 58,030,932 |
| `jvt_HPCAFL_BRCM_C` | 54,372,573 |
| `jvt_HPCV_BRCM_A` | 49,036,847 |
| `jvt_CABAST3_Sony_E` | 20,390,390 |
| `jvt_CVSEFDFT3_Sony_E` | 18,805,508 |
| `jvt_CVFI1_Sony_D` | 18,175,515 |
| `jvt_sp1_bt_a` | 16,046,980 |
| `jvt_CAMA1_Sony_C` | 14,517,408 |
| `jvt_FM1_BT_B` | 10,828,694 |
| `jvt_CAPAMA3_Sand_F` | 10,058,520 |
| `jvt_CAQP1_Sony_B` | 9,999,745 |
| `jvt_BAMQ1_JVC_C` | 9,719,423 |
| `jvt_CABA3_TOSHIBA_E` | 9,429,579 |
| `ytugc_clip02` | 5,918,384 |
| `jvt_CVPA1_TOSHIBA_B` | 15,371,660 |

`fast` verified total after retagging: 277,586,512 bytes (264.7 MiB).

## Step 3: `python3 tools/fetch_corpora.py --tags fast --max-total-bytes 314572800`

Run with an empty cache (`OVT_CORPORA_DIR` pointing at a new directory) using `.venv/bin/python`.

```console
exit code: 0
ok: 104 entries; 0 budget skips; 80 skips "not fetchable by URL" (synthetic and still-unverified entries)
cache: 116 files, 164,726,248 bytes (157.1 MiB; extracted bitstreams, no zip archives)
```

## 404 entries (still unverified; recorded under #160)

Resolved against the ITU listings with `manifest_add.py list-dir`:

- `jvt_CVFC1_SVA_C`: upstream is `CVFC1_Sony_C.zip`, which is already the manifest entry `jvt_CVFC1_Sony_C`.
- `jctvc_DBLK_A_SONY_4`: current revision is `DBLK_A_SONY_3.zip`.
- `jctvc_INITQP_B_Sony_1`: no such file; the listing has `INITQP_A_Sony_1.zip` and `INITQP_B_Main10_Sony_1.zip`.
- `jctvc_LTRPSPS_A_FJ_1`: no such file; the listing has `LTRPSPS_A_Qualcomm_1.zip`.
- `jctvc_GENERAL_444_8bit_RExt_Sony_2`: renamed `GENERAL_8b_444_RExt_Sony_2.zip`.
- `jvt_ALPHACONFORMANCEG`, `jvt_src19td_IBP_264`, `jvt_MVCNV_1`: not found in the listings at the paths tried.

The manifest ids and URLs are not changed here (they are spec-named, §14.3.2 spelling rule).

## Notes

- `dashif_bbb_30fps` and `apple_bipbop_adv_hevc` hash a top-level `.mpd`/`.m3u8`; origins can re-publish those in
  place, and the nightly `corpora-health` job will catch it.
- `check_fast_subset.py` and `--all-chapters`: `0 problem(s)` each. `pytest tools`: the budget test passes; 13
  failures in `tools/tests/test_normalizers.py` (`ModuleNotFoundError: numpy` in `.venv`) are identical on `main`.
