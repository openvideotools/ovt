# ADR-007

Copied verbatim from the specification, chapter 0 §0.2 (`docs/spec/00-backbone.md` line 74, spec version 1.0.0-rc1). The specification is normative; when the two differ, the specification wins and this file is re-copied. The full rationale is in chapter 5.

## Decision

**Header-level syntax parsing (NAL/OBU tree, parameter sets, slice/frame headers, SEI/metadata):** OVT writes its **own position-recording parsers** for H.264, HEVC and AV1 in `core/src/syntax/`, validated against FFmpeg `trace_headers` and GStreamer `codecparsers` as oracles.

## Why

Field↔bit-offset linkage for the hex view is not available from any upstream API (FFmpeg CBS is private and offsets exist only in its trace text). Header syntax is frozen and small (2–3 kLOC per codec).

## Alternatives rejected

Vendoring FFmpeg CBS (private API, no struct-level offsets); GStreamer codecparsers (no offsets).
