// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// AppKit window of the manual EDR tool (manual_edr_pattern.cpp): a CAMetalLayer view above a strip of UI white.

#ifndef OVT_RENDER_TESTS_MANUAL_EDR_WINDOW_H
#define OVT_RENDER_TESTS_MANUAL_EDR_WINDOW_H

namespace ovt::render::manual {

struct EdrWindow {
  void* layer = nullptr;            // CAMetalLayer*
  int width_px = 0, height_px = 0;  // drawable size in device pixels
  float device_pixel_ratio = 1.0F;
  float potential_headroom = 1.0F;  // NSScreen maximumPotentialExtendedDynamicRangeColorComponentValue
};

bool open_edr_window(int width_pt, int height_pt, EdrWindow& out);  // false without a window server
void pump_events(double seconds);  // AppKit event loop (the surface's headroom poll needs the main run loop)
float current_headroom();          // maximumExtendedDynamicRangeColorComponentValue of the window's screen

}  // namespace ovt::render::manual

#endif  // OVT_RENDER_TESTS_MANUAL_EDR_WINDOW_H
