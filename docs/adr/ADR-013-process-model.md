# ADR-013

Copied verbatim from the specification, chapter 0 §0.2 (`docs/spec/00-backbone.md` line 80, spec version 1.0.0-rc1). The specification is normative; when the two differ, the specification wins and this file is re-copied. The full rationale is in chapter 5.

## Decision

**Process model (phase 1): in-process plugins**, one decoder instance per stream, never shared across threads; all plugin output is a FlatBuffer so an out-of-process mode (crash isolation for hostile streams) can be added later without changing consumers. CLI runs are naturally separate processes.

## Why

Simplicity for the implementation team; data model already IPC-ready.

## Alternatives rejected

Mandatory worker processes in phase 1.
