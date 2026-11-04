# ADR-010

Copied verbatim from the specification, chapter 0 §0.2 (`docs/spec/00-backbone.md` line 77, spec version 1.0.0-rc1). The specification is normative; when the two differ, the specification wins and this file is re-copied. The full rationale is in chapter 5.

## Decision

**Serialisation: FlatBuffers** (Apache-2.0) for every analysis record (`ovt_model.fbs`, `ovt_metrics.fbs`, `ovt_container.fbs`). The same buffers are used in memory, across the plugin ABI, in the on-disk cache (zstd-compressed) and in CLI output. JSON (via FlatBuffers' reflection or explicit writers) and **YUView statistics CSV** are export formats.

## Why

Zero-copy, schema-versioned, generated readers for C++/Python/Kotlin/JS; one schema everywhere; large SoA vectors are cheap.

## Alternatives rejected

Protobuf (copying), Arrow (heavier, less natural for trees), hand-rolled structs (no versioning).
