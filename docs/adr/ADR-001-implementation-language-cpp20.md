# ADR-001

Copied verbatim from the specification, chapter 0 §0.2 (`docs/spec/00-backbone.md` line 68, spec version 1.0.0-rc1). The specification is normative; when the two differ, the specification wins and this file is re-copied. The full rationale is in chapter 5.

## Decision

**Implementation language: C++20** for all native code (core libraries, analysis plugins, renderer, GUI). Public ABI is **C** (`extern "C"`).

## Why

Every dependency that matters (FFmpeg, libaom, dav1d, libde265, HM, libvmaf, libplacebo, zimg, Qt) is C/C++; zero FFI cost; lowest maintenance against fast-moving upstreams; one language for the whole team. Memory-safety risk in parsers is mitigated by a mandatory bounds-checked `BitReader`, sanitizer CI and fuzzing (NFR).

## Alternatives rejected

Rust core (safer parsers, but every decoder binding is thin or unmaintained and a Qt bridge adds a layer); Dart/Kotlin (no path to decoder internals).
