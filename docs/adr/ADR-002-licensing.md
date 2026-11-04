# ADR-002

Copied verbatim from the specification, chapter 0 §0.2 (`docs/spec/00-backbone.md` line 69, spec version 1.0.0-rc1). The specification is normative; when the two differ, the specification wins and this file is re-copied. The full rationale is in chapter 5.

## Decision

**Licensing:** `core/` and `render/` libraries **LGPL-2.1-or-later**; the `app/` GUI, `cli/`, `android/` and any code derived from YUView **GPL-3.0-or-later**; plugin ABI headers (`core/include/ovt/abi/*.h`) **BSD-2-Clause** so third parties can implement plugins. CI fails on any dependency licensed AGPL, SSPL, BUSL or non-commercial.

## Why

GPL-3 unlocks FFmpeg GPL builds, libde265 (LGPL-3), Qt including GPL-only modules, and YUView code reuse. An LGPL core lets companies embed the data layer in CI pipelines. BSD headers make vendor decoder plugins possible.

## Alternatives rejected

GPL-3 everywhere (simpler, blocks embedding); permissive everywhere (blocks libde265 static linking and YUView reuse).
