# Sanitizers and suppressions

OVT runs three sanitizer configurations (chapter 14 §14.8.1). The presets come from WI-0.004 and the CI jobs
from WI-0.031; this page covers how third-party code is built and how a suppression is added.

| Preset | Sanitizers | Runtime options |
|---|---|---|
| `linux-x64-asan-ubsan`, `macos-arm64-asan` | `-fsanitize=address,undefined` | `ASAN_OPTIONS` and `UBSAN_OPTIONS` of the `asan-env` test preset |
| `linux-x64-tsan` | `-fsanitize=thread` | `TSAN_OPTIONS=halt_on_error=1 suppressions=tests/tsan.supp` |

Run the lane locally with `tools/ctest.sh sanitizers` (or `ctest --preset sanitizers`). Run the canary with
`ctest --preset sanitizers -L canary`; it must fail, otherwise ASan is not active.

## Third-party code is sanitized too

`cmake/OvtThirdPartyCommon.cmake` copies the `-fsanitize` flags of the active `OVT_SANITIZER` set into
`OVT_TP_CMAKE_TOOLCHAIN_ARGS` as `CMAKE_C_FLAGS`, `CMAKE_CXX_FLAGS` and the three `CMAKE_*_LINKER_FLAGS`, so every
CMake-based `third_party/<dep>.cmake` script builds with the same
instrumentation as OVT. Without it ASan reports false positives where instrumented and plain code meet, and TSan
misses races inside the decoders. Optimisation and debug flags are not copied: third-party code keeps its
`RelWithDebInfo` build type.

A script that does not use CMake (Meson, `configure`) must pass `OVT_TP_SANITIZER_C_FLAGS` (compile and link) and
`OVT_TP_SANITIZER_LINK_FLAGS` on itself. A script must not put its own `CMAKE_C_FLAGS` or `CMAKE_CXX_FLAGS` in
`CMAKE_ARGS`; the later definition silently replaces the other. Both are empty when sanitizers are off.

## Suppression files

| File | Runtime | Read through | Entry kinds |
|---|---|---|---|
| `tests/asan.supp` | LeakSanitizer | `LSAN_OPTIONS=suppressions=tests/asan.supp` | `leak` only |
| `tests/ubsan.supp` | UBSan | `UBSAN_OPTIONS=suppressions=tests/ubsan.supp` | check names such as `alignment`, `shift-base`, `shift-exponent`, `signed-integer-overflow`, `vptr`, `undefined` |
| `tests/tsan.supp` | TSan | `TSAN_OPTIONS=suppressions=tests/tsan.supp` | `race`, `race_top`, `thread`, `mutex`, `signal`, `deadlock`, `called_from_lib` |

One file serves one runtime: ASan's own kinds (`interceptor_*`, `odr_violation`) belong to `ASAN_OPTIONS` and a
file that mixes them with `leak` makes both runtimes abort with `failed to parse suppressions`. UBSan takes check
names, not `-fsanitize` group names, so `shift` and `implicit-conversion` are rejected. `check_suppressions.py`
enforces both.

Append the option from the table to the options of the preset. The preset environments do not set the
suppression options yet (they live in
`CMakePresets.json`, which needs the owner's approval, and in the CI jobs of WI-0.031); until then, with the
files empty, nothing is suppressed.

## Adding a suppression

1. Reproduce the report and make sure the code is not OVT's. Findings in OVT code are fixed, never suppressed.
2. Find or open the upstream issue. A suppression without a link to it is rejected.
3. Add the entry under a comment that carries the link, with no blank line in between, as narrowly as possible
   (one function or one file, not a whole library):

   ```text
   # libaom: leaks the film grain table on a failed read. https://aomedia.issues.chromium.org/issues/NNNN
   # Remove when the pin in third_party/VERSIONS.md reaches the fixed release.
   leak:aom_film_grain_table_read
   ```

   Consecutive entries share the comment above the first one; a blank line ends the block.
4. Run `python -I tools/check_suppressions.py`. It exits 1 with `E-NOURL`, `E-KIND` or `E-EMPTY` and the line
   number when an entry has no `https://host/path` link above it, uses an entry kind that the sanitizer does not
   know, or has no pattern.
5. Name the entry and the issue in the pull request body, under Deviations. Remove the entry when the pin moves
   past the fix.
