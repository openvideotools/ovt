# ADR-005

Copied verbatim from the specification, chapter 0 §0.2 (`docs/spec/00-backbone.md` line 72, spec version 1.0.0-rc1). The specification is normative; when the two differ, the specification wins and this file is re-copied. The full rationale is in chapter 5.

## Decision

**Frame rendering and colour management: libplacebo** (LGPL-2.1+) does upload, YUV→RGB, EOTF, tone mapping, gamut mapping, dithering and HDR metadata signalling for all platforms (Vulkan; MoltenVK on macOS; GLES/Vulkan on Android). Overlay geometry (grids, arrows, heatmaps, labels) is drawn by `libovt-render` in the same pass.

## Why

Only FOSS implementation of the complete HDR display toolbox, proven on all three platforms via mpv/VLC.

## Alternatives rejected

Hand-written shaders (months of colour-science work); Qt scene graph alone (SDR-only).
