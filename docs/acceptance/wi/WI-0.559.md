<!--
SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
SPDX-License-Identifier: CC-BY-SA-4.0
-->

# WI-0.559 acceptance record

Date: 2026-11-04. Machine: GitHub-hosted `ubuntu-22.04` runners (CI) and a cloud container for the local
checks. Spec version: 1.0-rc1. Tools in CI: clang-tidy 15 (`clang-tidy-15`, Ubuntu 22.04 archive), gcc-13
compile commands from the `linux-x64-debug` preset.

## Test 1: pointer arithmetic in `core/src/syntax/` fails the PR job

A temporary commit `618b736` added `core/src/syntax/zz_wi0559_proof.cpp` (`std::uint8_t* p = data + 3;`) and
a `target_sources(ovt_core ...)` line; it was reverted in `a558cc9`. The `clang-tidy` job of
`static-analysis.yml` failed on it:

```text
core/src/syntax/zz_wi0559_proof.cpp:4:26: error: do not use pointer arithmetic [cppcoreguidelines-pro-bounds-pointer-arithmetic,-warnings-as-errors]
  std::uint8_t* p = data + 3;
                         ^
##[error]clang-tidy findings on the changed lines
```

Run: <https://github.com/openvideotools/ovt/actions/runs/41854857745> is the first run (no C++ changed);
the failing run is <https://github.com/openvideotools/ovt/actions/runs/41855834849> (job
`126664074368`). It also shows that `/usr/lib/llvm-15/share/clang/clang-tidy-diff.py` exists in the
`clang-tidy-15` package and finds the translation unit through `-path build/linux-x64-debug`.

## Test 2: implicit narrowing fails with `-Wconversion` under `OVT_WERROR=ON`

Provided by `ovt::flags` (chapter 13 §13.2.5, WI-0.000): `-Wconversion` and, with `OVT_WERROR=ON`,
`-Werror`. `core/src/metrics` has no sources yet, so there is no file to narrow in; the existing release CI
presets (`build-linux-x64`, `build-macos-arm64`) build with `OVT_WERROR=ON` and are green.

## Test 3: a target that does not link `ovt::flags` fails `tools/check_ovt_flags.py`

Automated: `tools/tests/test_check_ovt_flags.py::test_target_without_flags_names_target` (exit 1, names the
target), plus `test_clean_tree_passes`, `test_root_level_target_is_skipped` and
`test_unconfigured_build_dir_is_exit_2`. They run in the `lint` job's `Tool tests` step. On the real tree
the `clang-tidy` job runs the script after configuring `linux-x64-debug` and it exits 0.

## Whole-tree mode is green on today's tree

The whole-tree step (nightly in `static-analysis.yml`) was run once on this PR by a temporary commit
(`e103d49`, switched off again in the final commit):

```console
$ run-clang-tidy-15 -p build/linux-x64-debug -quiet -extra-arg=-Wno-unknown-warning-option '^$PWD/(core|render|plugins|cli|app)/.*\.(cpp|cc)$'
```

First run (`41856719263`) found 11 findings, all in `core/tests/` plus none in production code. After
`cli/src/main.cpp` was fixed (argv as a `std::span`, discarded `fprintf` results cast to `void`), the
header filter left out `core/include` (C headers cannot satisfy `modernize-use-using`), and
`core/tests/.clang-tidy` relaxed five test-idiom checks, the run is green: <https://github.com/openvideotools/ovt/actions/runs/41857502634>
(`clang-tidy` job `126669967774`, success). The three decisions are `SPEC-ASSUMPTION(#29)`.
