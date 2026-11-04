# ADR-006

Copied verbatim from the specification, chapter 0 §0.2 (`docs/spec/00-backbone.md` line 73, spec version 1.0.0-rc1). The specification is normative; when the two differ, the specification wins and this file is re-copied. The full rationale is in chapter 5.

## Decision

**Container inspection:** OVT writes its **own ISOBMFF reader** (C++, schema-assisted from the MP4RA registry and the MPEG FileFormatConformance JSON) that records byte/bit spans for every box and field. **libavformat** is used for demuxing packets to decoders. GPAC/MP4Box, mp4ff, mp4box.js and MediaInfo trace are **test oracles only**.

## Why

No FOSS library exposes a field-level tree with offsets for hex linkage; GPAC is LGPL C with internal structs and no offsets in its dump; Bento4 is GPL-2-only.

## Alternatives rejected

libgpac as inspection backend (fallback if the own reader stalls).
