// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native side of HdrSurfaceView (WI-0.052; chapter 13 §13.4.4 item 2, chapter 10 §10.4.3): tags the SurfaceView's
// ANativeWindow with the dataspace of the display's encoding before EGL touches it, creates an EGL window surface
// (RGB10A2 and EGL_EXT_gl_colorspace_bt2020_* for HDR) and draws the PQ test pattern of hdr_pattern.h with plain
// GLES. WI-A.401 replaces the pattern with libplacebo's OpenGL target on the same window; WI-A.400 adds the geometry
// callbacks. Called from the UI thread through ovt_jni.cpp; every function is noexcept and serialised by one mutex.

#pragma once

#include <android/native_window.h>

#include <cstdint>
#include <span>

#include "hdr_pattern.h"

namespace ovt::android_shell {

// What the display can show (Kotlin DisplayCaps): the Display HDR_TYPE_* values, empty for an SDR panel, and the
// luminance figures for the diagnostics line (0 = unknown). Decides the encoding of the next attach (§10.4.3 rule).
struct DisplayInfo {
  float max_nits = 0.0F;
  float min_nits = 0.0F;
  float max_fall_nits = 0.0F;
  float hdr_sdr_ratio = 1.0F;
  int api_level = 0;
};
void hdr_set_display(std::span<const std::int32_t> hdr_types, const DisplayInfo& info) noexcept;

// Takes over one reference to `window` (from ANativeWindow_fromSurface). A new window gets its dataspace, the
// `OVT_HDR dataspace=<name>` log line and an EGL surface, then the pattern; the window that is already attached is
// only redrawn (surfaceChanged, surfaceRedrawNeeded). A non-zero ANativeWindow_setBuffersDataSpace result logs
// RND-0050 once and turns HDR off for the rest of the process (§10.4.3). Returns false when nothing could be drawn.
bool hdr_surface_attach(ANativeWindow* window) noexcept;

// Destroys the EGL surface and context and releases the window (surfaceDestroyed). No-op when nothing is attached.
void hdr_surface_detach() noexcept;

// Display.hdrSdrRatio (API 34+ listener). Kept for DisplayTarget.headroom; the GLES pattern does not use it.
void hdr_set_headroom(float hdr_sdr_ratio) noexcept;

// The encoding of the attached surface (Srgb8 when none is attached).
OutputEncoding hdr_active_encoding() noexcept;

}  // namespace ovt::android_shell
