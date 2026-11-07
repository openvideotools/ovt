<!--
SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
SPDX-License-Identifier: CC-BY-SA-4.0
-->

# WI-0.040 acceptance record

Spec version 1.0-rc15. Machine: cloud container (Ubuntu 24.04, CMake 3.28.3).

## Test 1: `corpus_path` is below `OVT_CORPORA_DIR`

Already provided by WI-0.550: `test_support/corpus: finds verified entries only` and
`test_support/corpus: the .ok record names the primary file` (`core/tests/test_support_corpus.cpp`) set
`OVT_CORPORA_DIR` and assert that `corpus_path(id)` is `<dir>/<id>/<file>`. This WI adds no C++.

## Test 2: empty `OVT_CORPORA_DIR` skips, `OVT_REQUIRE_CORPORA=1` fails

Registered in `tests/golden/CMakeLists.txt` against the fixture manifest `tests/golden/corpus_gate/manifest.json`
(two fetchable entries tagged `fast`, three entries that are not fetchable):

| Case | Test |
|---|---|
| a golden test registered with `CORPUS_TAG fast` is skipped when nothing is fetched | `golden_corpus_gate_skips` (ctest reports `Skipped`) |
| the exact message is printed for an empty and for a partly filled cache | `corpus_gate_message_when_empty`, `corpus_gate_message_when_partial` |
| `OVT_REQUIRE_CORPORA=1` fails the same run | `corpus_gate_fails_when_required` (`WILL_FAIL`) |
| with the subset complete the wrapped command runs | `corpus_gate_runs_command_when_complete` |

The message is `corpus subset 'fast' missing: run tools/fetch_corpora.py --tags fast`.
The fetchable-entry rule gives 712 entries for the real manifest, the same count as the fetcher's own selection.
