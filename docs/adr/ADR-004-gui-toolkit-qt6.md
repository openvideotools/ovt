# ADR-004

Copied verbatim from the specification, chapter 0 §0.2 (`docs/spec/00-backbone.md` line 71, spec version 1.0.0-rc1). The specification is normative; when the two differ, the specification wins and this file is re-copied. The full rationale is in chapter 5.

## Decision

**GUI toolkit: Qt 6 (Qt Quick/QML, Qt 6.8.3 LTS; Wayland colour management needs a later Qt 6.x, 6.10 is not an LTS release — D5)** with `QQuickRhiItem`/`QSGRenderNode` for GPU items. Android/ChromeOS: Qt Quick embedded via `QtQuickView` in a conventional Android `Activity`, with a native `SurfaceView` sibling for HDR output.

## Why

Only toolkit with HDR swapchain formats today, merged Wayland `color-management-v1`, a mature Android port (API 28–36, arm64 + x86_64), model-virtualised trees, and desktop idioms (hover, right-click, shortcuts, docking) required by ChromeOS/Android desktop windowing.

## Alternatives rejected

Flutter (no HDR surface, platform views immature on desktop), egui/wgpu (no HDR renderer, weak Android), ImGui+SDL3 (not a product UI), GTK4, Avalonia, Compose.
