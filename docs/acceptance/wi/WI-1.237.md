# WI-1.237 acceptance record

Machine: cloud container, Linux 6.18 x86_64, Python 3.13, outbound HTTPS through a proxy.
Date: 2026-11-07. Spec version: see `docs/spec/00-front-matter.md`. Commit: the head of the WI-1.237 pull request.

## Test 1: `container-smoke` downloads and hash-verifies

The container cannot reach `www.itu.int` (the proxy answers `403 Forbidden`), so the two Annex B entries that
come from ITU-T sites (`jvt_HCAFF1_HHI_B`, `jctvc_ipcm_A_NEC_3`, 0.9 MiB together) were not fetched here.
The other 18 files were. The link was not limited to 100 Mbit/s and the run is not a clean-machine run.

```console
$ python3 -I tools/fetch_corpora.py --tags container-smoke --cache-dir <empty dir>
ok          aom_av1_1_b8_01_size_16x16
...                                         (18 ok lines)
failed      jvt_HCAFF1_HHI_B                (403 Forbidden from the proxy, 3 attempts)
failed      jctvc_ipcm_A_NEC_3              (403 Forbidden from the proxy, 3 attempts)
$ echo $?
3
```

Wall time 2.6 s for the 18 reachable files (5.9 MiB). The whole set is 6.5 MiB, so the 5 minute budget on a
100 Mbit/s link is not at risk. The two ITU files are fetched, hash-verified, by the hosted CI jobs that already
fetch other `fast` ITU entries; the CI result of this pull request stands as their record.

Size cap: `test_smoke_set_is_verified_fast_and_within_300_mib` (`tools/tests/test_container_corpus.py`).

## Test 2: wrong hash exits non-zero naming the file

Automated: `test_acceptance_2_wrong_sha256_exits_2_without_ok` in `tools/tests/test_fetch_corpora.py`.
