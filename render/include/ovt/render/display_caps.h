// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Display capabilities (chapter 10 §10.4.5, verbatim). display_probe() is declared here and implemented per platform
// by WI-1.407; WI-1.400 only provides the declaration.

#ifndef OVT_RENDER_DISPLAY_CAPS_H
#define OVT_RENDER_DISPLAY_CAPS_H

#include "ovt/render/render_api.h"
#include "ovt/render/video_surface.h"  // DisplayTarget

namespace ovt::render {

// NOLINTBEGIN(modernize-avoid-c-arrays,readability-isolate-declaration): verbatim from §10.4.5
struct DisplayCaps {
  bool hdr_capable;          // can enter NATIVE mode now
  bool hdr_active;           // output is currently HDR (user setting / headroom > 1)
  float headroom;            // current, 1.0 if SDR
  float potential_headroom;  // maximum the display can reach (macOS "potential"; others = max_nits/sdr_white)
  float sdr_white_nits;      // 203 unless the platform reports it; macOS: NaN ("relative")
  float max_nits, min_nits, max_fall_nits;  // 0 = unknown
  bool supports_pq, supports_hlg, supports_scrgb;
  bool wide_gamut;            // primaries >= P3
  char primaries_name[16];    // "BT.709", "P3", "BT.2020", "unknown"
  char source[32];            // "NSScreen", "wp_color_manager_v1", "Display.Mode.supportedHdrTypes", "none"
  char reason[96];            // why not capable: "X11 session", "compositor lacks wp_color_manager_v1", ...
  char platform_detail[128];  // e.g. "Mesa 25.1.3 radv; KWin 6.4.2", "Android 15 API 35; HDR10,HLG", "macOS 15.5; XDR"
};
// NOLINTEND(modernize-avoid-c-arrays,readability-isolate-declaration)

OVT_RENDER_API DisplayCaps display_probe(const DisplayTarget& where /* kind + native identify the screen */);

}  // namespace ovt::render

#endif  // OVT_RENDER_DISPLAY_CAPS_H
