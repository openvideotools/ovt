# WI-1.235 acceptance record

Machine: cloud container, Linux x86_64, GCC 13, `linux-x64-debug` with system dependencies. Date: 2026-11-21.
Spec version: 1.0-rc48 (part b).

This WI is reduced (chapter 16 §16.3.8): it delivers the core functions behind `ovt probe`. Tests of the subcommand
are WI-1.503.

| Given/When/Then | Test |
|---|---|
| AT1: a file with lazy boxes, `ovt_container_expand_all` → no `BoxNode` outside `mdat` carries `CNT-0007` | `probe: expand_all leaves no unexpanded box outside mdat` (200 fragments, so every `moof` starts deferred) |
| AT2 (as restated by #763): `ovt_container_list_diagnostics` returns a `DiagnosticRegistry` listing every `diag_codes.inc` code exactly once, in file order, with severity, message template and spec reference; count equal to the registry size | `probe: ovt_container_list_diagnostics lists every registry code once with severity and spec reference` (part b, PR WI-1.235b); `probe: every registry code appears exactly once with a severity` checks the C++ source. The registry holds 94 codes at this date; the "≥ 150" figure of the original wording is reached when the remaining container work items append their rows (WI-1.234 AT1 checks completeness). |

Command: `ovt_tests_container "probe*"` → all tests passed (334 assertions in 7 test cases). Determinism: `probe:
expand_all is deterministic` compares two runs byte for byte.

Part b (WI-1.235b, 2026-11-21): `ovt_tests_container "probe*"` → see the PR body for the run; determinism:
`probe: ovt_container_list_diagnostics is deterministic and rejects a null out` compares two buffers byte for byte.
