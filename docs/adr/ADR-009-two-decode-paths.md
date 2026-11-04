# ADR-009

Copied verbatim from the specification, chapter 0 §0.2 (`docs/spec/00-backbone.md` line 76, spec version 1.0.0-rc1). The specification is normative; when the two differ, the specification wins and this file is re-copied. The full rationale is in chapter 5.

## Decision

**Two decode paths.** *Playback path:* libavformat → libavcodec (hardware: VideoToolbox on macOS, MediaCodec on Android, VA-API on Linux; software fallback) → `libovt-render`. *Analysis path:* stream index → seek to nearest random access point → feed units to the analysis plugin → `FrameAnalysis` records + planes → cache. The analysis path runs on worker threads and is GOP-local (only the frames needed for the frame under inspection).

## Why

Hardware decoders expose no internals; instrumented decoders are slow (libde265 has no NEON; libaom inspection builds are 3–10× slower than dav1d).

## Alternatives rejected

Single software path (too slow on Android for 4K scrubbing).
