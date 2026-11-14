// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Screenshot and export kinds of VideoSurface::export_png() (chapter 10 §10.8, verbatim). The export itself is
// implemented by its own work item; WI-1.400 only declares the kinds.

#ifndef OVT_RENDER_EXPORT_H
#define OVT_RENDER_EXPORT_H

#include <cstdint>

namespace ovt::render {

enum class ExportKind : uint8_t {
  HdrPng16,  // 16-bit RGB PNG, BT.2020 primaries, PQ transfer, full range, cICP chunk (9,16,0,1); no overlays; crop
             // applied; native resolution
  SdrPng8,   // 8-bit sRGB PNG, tone-mapped with the supplied ViewState (tone_map, gamut_map), native resolution, no
             // overlays
  ViewPng8,  // 8-bit sRGB PNG of the current viewport exactly as displayed (zoom, pan, overlays, compare),
             // EMBEDDED-mode rendering
  ScopeWaveformPng,
  ScopeVectorscopePng,
  ScopeCiePng  // 8-bit PNG of the scope textures with the GUI-independent LUT
};

}  // namespace ovt::render

#endif  // OVT_RENDER_EXPORT_H
