<!-- SPDX-FileCopyrightText: 2026 The Open Video Tools contributors -->
<!-- SPDX-License-Identifier: CC-BY-SA-4.0 -->
# tests/corpora

`manifest.json` is the single list of test media (chapter 14 §14.2.4). No media is committed:
`tools/fetch_corpora.py` downloads and hash-verifies the files into a cache directory. `LICENSES.md` is
generated from the manifest by `tools/license_gate.py --policy tools/license_policy.toml --corpora --write-licenses-md`.

## Where the corpora live (`OVT_CORPORA_DIR`)

| Where | Value |
|---|---|
| CMake cache variable | `OVT_CORPORA_DIR`, default `<source>/build/corpora` (`cmake/OvtOptions.cmake`) |
| Environment, at test and fetch time | `OVT_CORPORA_DIR` when set and non-empty; it overrides the CMake value |
| Fetcher | `--cache-dir` > `$OVT_CORPORA_DIR` > the platform cache directory (`tools/fetch_corpora.py`) |

Layout: `<cache>/<id>/<file>` plus `<cache>/<id>/.ok`. The fetcher writes `.ok` only after the SHA-256 check;
tests find a file with `ovt::test::corpus_path("<id>")` (`core/tests/test_support/corpus.h`), which returns a path
below the cache directory.

```sh
tools/fetch_corpora.py --tags fast                  # the fast subset used by CI and local runs
cmake -B build/x -DOVT_CORPORA_DIR=/data/ovt-corpora # or: export OVT_CORPORA_DIR=/data/ovt-corpora
```

## Missing corpora skip, they do not fail (`cmake/OvtCorpora.cmake`)

A test that needs media is skipped when the media is not fetched, so a fresh checkout stays green:

- C++ tests tagged `[needs-corpus]` call `ovt::test::corpus_or_skip(id)`: Catch2 `SKIP("corpus file missing: <id>")`.
- A golden test registered with `ovt_add_golden_test(... CORPUS_TAG <tag>)` runs through `cmake/OvtCorpusGate.cmake`.
  When any fetchable entry of the subset has no `<cache>/<id>/.ok`, the test prints
  `corpus subset '<tag>' missing: run tools/fetch_corpora.py --tags <tag>` and ctest reports it as skipped.
  "Fetchable" is the fetcher's rule: a `url` or `vcs`, and neither `unverified` nor `manual_download`.
- `OVT_REQUIRE_CORPORA=1` turns every such skip into a failure. Use it on lanes that fetch corpora, so a
  broken download cannot hide as a skip.
- CMake helpers for other callers: `ovt_corpora_index()`, `ovt_corpus_set_present(<tag> <var>)`,
  `ovt_corpus_gate_command()`, `ovt_corpus_skip_properties()`. Reading the manifest in CMake takes about six seconds,
  so only registrations that use `CORPUS_TAG` pay for it, once per configure.

## Sources and licences

Each manifest entry carries its source, licence and SPDX identifier; `LICENSES.md` lists them. Most media is licensed
for testing only (`redistributable: false`). Never commit it, mirror it or attach it to a release: corpora are not
part of any OVT release artifact. A PR that touches `manifest.json` regenerates `LICENSES.md`.
Generated streams (`synthetic/`) are CC0. ITU-T H.265 conformance streams come as zips from the JCT-VC site and
are extracted member by member by the fetcher; the AOM Argon vectors are large and belong to a separate, nightly-only
set.
