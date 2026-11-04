# ADR-003

Copied verbatim from the specification, chapter 0 §0.2 (`docs/spec/00-backbone.md` line 70, spec version 1.0.0-rc1). The specification is normative; when the two differ, the specification wins and this file is re-copied. The full rationale is in chapter 5.

## Decision

**Layered architecture:** `libovt-core` (no UI, no GPU) → `libovt-render` (GPU, libplacebo, no toolkit types) → front-ends (`ovt` CLI, `ovt-studio` Qt GUI, Android shell, Python module). Everything a front-end shows is available from the core via the C API and from the CLI as JSON/FlatBuffers.

## Why

"Same binary for GUI and CLI" is the universal commercial convention; researchers script everything; Android and desktop share one core.

## Alternatives rejected

GUI-first monolith.
