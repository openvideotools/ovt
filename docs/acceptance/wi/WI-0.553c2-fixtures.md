# WI-0.553c2 fixtures acceptance record

Records the non-stream fixture hashes owed by the WI-0.553 part c2 oracle run
(`docs/acceptance/wi/WI-0.553.md`, "Fixtures owed").

Machine: Mac Mini (Apple silicon, macOS 15), Docker Desktop, image `ovt-oracles:main` built as `linux/amd64`
(Rosetta) from `tests/oracles` on `main` at 1d2ac38 (includes #235, the oracle fixture fixes). Generator tree:
`main` at f99acf1 (includes #248). Date: 2026-11-08.

```sh
python3 tests/corpora/synthetic/generate.py --out /work/out --fixtures --update-manifest   # exit 0
```

| Fixture | sha256 | Size (bytes) |
|---|---|---|
| `fixture_synthetic_4k_grid` | `a6a40473f15d3487852b5e0f03141f6dac2defadd1405e8b2624e9ed32ec5ffc` | 5,625,896 |
| `fixture_synthetic_4k_mv` | `a13988b49df005f249ec232a0d50a28ed7ba9738264de99dc01dab32b6ccd2be` | 45,193,568 |
| `fixture_synthetic_8k_mv` | `d1360c8344568f5a88a4e55ff891cff19107013472742d03da5c4a6f81084f3c` | 180,783,728 |
| `fixture_hdr10plus_t35` (`f000.bin`) | `886e7d8c6df3ef980a4c7e930229963af0b3357fe2db985b0510556878a7b0a2` | 64 |

`fixture_hdr10plus_t35` also records `f030.bin` (`b0c12ef6cd3e6fa09b12e3d764b16dd419c79457d258bacb1f39c0c439a16431`)
and `f059.bin` (`97894b0fabe0cf4ab854fea09da85e417ae5a6e90d70f6b228153d8011a7529f`), 64 bytes each. The generator read
`syn_h265_hdr10plus` and extracted the T.35 payloads of pictures 0, 30 and 59.

Skipped, as designed: `fixture_real_1080p_hevc_frame` and `fixture_real_4k_hevc_frame` (need the `ovt` CLI,
WI-2.174), and the two `local_only` perf fixtures (`fixture_fa_*`).

The `.fb` files are not committed (CLAUDE.md §5: no generated FlatBuffers code or goldens over 2 MB); only their
hashes are recorded in the manifest.
