# ADR-012

Copied verbatim from the specification, chapter 0 §0.2 (`docs/spec/00-backbone.md` line 79, spec version 1.0.0-rc1). The specification is normative; when the two differ, the specification wins and this file is re-copied. The full rationale is in chapter 5.

## Decision

**Build system: CMake ≥ 3.25 with CMakePresets.json**, dependencies via **vcpkg manifest** where ports exist and `third_party/*.cmake` ExternalProject scripts for libaom (custom flags), libde265 (patched), libvmaf and libplacebo (Meson), FFmpeg (configure). Android via Gradle + `externalNativeBuild` + Qt's `androiddeployqt`. One `cmake --preset <name>` builds each platform.

## Why

Mixed upstream build systems; reproducibility for GPL "Corresponding Source"; a single documented entry point for agents.

## Alternatives rejected

Conan; Meson-only; Bazel (owner-confirmed 2026-11-03: rules_foreign_cc can wrap the C libraries, but Qt 6/QML and Android packaging stay on CMake/Gradle, so Bazel would mean two build systems; see chapter 13 §13.2.0 for the path to move the core later).
