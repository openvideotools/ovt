# ADR-014

Copied verbatim from the specification, chapter 0 §0.2 (`docs/spec/00-backbone.md` line 81, spec version 1.0.0-rc1). The specification is normative; when the two differ, the specification wins and this file is re-copied. The full rationale is in chapter 5.

## Decision

**Android HDR output uses `SurfaceView` + `ANativeWindow_setBuffersDataSpace` (BT2020_PQ / BT2020_HLG) via EGL or `AHardwareBuffer`,** not Vulkan swapchain colourspaces (OEM drivers often hide them). `TextureView` is forbidden (forces SDR on API ≥ 33). Android 15+: `setDesiredHdrHeadroom`. ChromeOS HDR pass-through is **unverified** and is a week-one hardware spike (WI-0.055).

## Why

Platform facts from Android docs and Arm driver reports.

## Alternatives rejected

Vulkan-only HDR.
