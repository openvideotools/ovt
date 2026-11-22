# WI-1.235 acceptance record

Machine: cloud container, Linux x86_64, GCC 13, `linux-x64-debug` with system dependencies. Date: 2026-11-21.
Spec version: 1.0-rc45.

This WI is reduced (chapter 16 §16.3.8): it delivers the core functions behind `ovt probe`. Tests of the subcommand
are WI-1.503.

| Given/When/Then | Test |
|---|---|
| AT1: a file with lazy boxes, `ovt_container_expand_all` → no `BoxNode` outside `mdat` carries `CNT-0007` | `probe: expand_all leaves no unexpanded box outside mdat` (200 fragments, so every `moof` starts deferred) |
| AT2: the registry lists ≥ 150 codes, each once | **Not met.** `diag_codes.inc` holds 136 codes at 2ad0892 (94 when the PR was opened; later work items keep appending) and the C function is blocked on the schema table (spec question). `probe: every registry code appears exactly once with a severity` checks uniqueness only. |

Command: `ovt_tests_container "probe*"` → all tests passed (334 assertions in 7 test cases). Determinism: `probe:
expand_all is deterministic` compares two runs byte for byte.
