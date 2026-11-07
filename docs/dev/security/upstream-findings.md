# Upstream findings

Crashes and sanitizer reports that the fuzz targets or OSS-Fuzz found in third-party code (FFmpeg, libaom,
libde265, dav1d and others), as classified in step 3 of the triage procedure in [`fuzzing.md`](fuzzing.md).
Each finding is reported upstream with the minimised input. Its regression input stays in
`core/tests/fuzz/regressions/<target>/`, the test is marked `[xfail-upstream]`, and a suppression in
`tests/asan.supp` or `tests/ubsan.supp` links to the upstream issue (chapter 14 §14.8.1). Never suppress without
a row here.

| Date | Library and version | Upstream issue | OVT target and regression input | Sanitizer report | Status |
|---|---|---|---|---|---|
| | | | | | |

No findings yet.

Columns: *Status* is `reported`, `fixed upstream (version)` or `wontfix`. Remove the `[xfail-upstream]` mark and
the suppression when OVT's pinned version contains the fix (`third_party/VERSIONS.md`).
