# WI-0.552a acceptance record

WI-0.552 is delivered in two pull requests (§16.2.6, letter suffixes; the §16.4 row stays WI-0.552 until
WI-0.552b merges). WI-0.552a: tooling, schema fields, fetcher `vcs` support, the named-stream check and every
entry whose origin is reachable from the cloud container. WI-0.552b: `manifest_add.py fill` and
`list-dir` on a machine that can reach `www.itu.int`, `dash.akamaized.net`, `testassets.dashif.org`, Apple and the
CTA WAVE host, the `LICENSES.md` regeneration, and the AT1 run below.

Machine: cloud container, Ubuntu 24.04 x86_64, Python 3.13. Date: 2026-11-06. Spec version: 1.0-rc4.

## Test 1: `fetch_corpora.py --tags fast` stays within 314 572 800 bytes and exits 0

**Deferred to WI-0.552b.** 150 of the 203 `fast` entries are `unverified` (jvt 38 of 40, jctvc 45 of 46, cta,
dashif, apple, hdr) because their hosts are blocked here. The fetcher skips them. For the hashed entries:

```console
$ python3 tools/fetch_corpora.py --tags fast --max-total-bytes 314572800 --jobs 8
... ok for every reachable entry; exit 3 only for jvt_BA1_Sony_D, jvt_CABA1_SVA_B, jctvc_AMP_A_Samsung_7
    (hashed earlier, host blocked here)
```

The verified `fast` total is about 120 MB (`test_fast_subset_stays_within_the_ci_budget` checks it). WI-0.552b
records the full run on the Mac Mini, with the total including the filled entries.

## Test 2: `license_gate.py --corpora` exits 0

```console
$ python3 tools/license_gate.py --policy tools/license_policy.toml --corpora
license gate: PASS (0 findings, 0 components)
```

Re-run in CI by the `license-gate` job.

## Test 3: `check_fast_subset.py` exits 0

```console
$ python3 tools/check_fast_subset.py && python3 tools/check_fast_subset.py --all-chapters
0 problem(s)
0 problem(s)
```

Automated: `test_check_fast_subset_passes_on_the_repository`, `test_named_stream_without_entry_is_reported`.
