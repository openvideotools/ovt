# ADR-015

Copied verbatim from the specification, chapter 0 §0.2 (`docs/spec/00-backbone.md` line 82, spec version 1.0.0-rc1). The specification is normative; when the two differ, the specification wins and this file is re-copied. The full rationale is in chapter 5.

## Decision

**Default download for AVC/HEVC decoding uses runtime-loaded decoder plugins** (`dlopen`/`QLibrary`), so official binaries can be built with or without pooled-patent codecs (Fedora-style split). AV1 (libaom, dav1d) and libvmaf ship in every official build with the AOM PATENTS notice.

## Why

Patent posture identical to VLC/HandBrake/YUView; lets companies substitute licensed decoders.

## Alternatives rejected

Always-bundled decoders.
