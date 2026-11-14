// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// A bare AppKit window for the manual EDR tool (manual_edr_pattern.cpp, WI-1.406 step 5): a layer-backed view whose
// layer is a CAMetalLayer, above a strip of plain system-UI white for comparison. macOS only; not part of ctest.

#ifndef OVT_RENDER_TESTS_MANUAL_EDR_WINDOW_H
#define OVT_RENDER_TESTS_MANUAL_EDR_WINDOW_H

namespace ovt::render::manual {

struct EdrWindow {
  void* layer = nullptr;            // CAMetalLayer*
  int width_px = 0, height_px = 0;  // drawable size in device pixels
  float device_pixel_ratio = 1.0F;
  float potential_headroom = 1.0F;  // NSScreen maximumPotentialExtendedDynamicRangeColorComponentValue
};

// Opens the window (points), centred on the main screen. Returns false without a window server.
bool open_edr_window(int width_pt, int height_pt, EdrWindow& out);
// Runs the AppKit event loop for `seconds` (the EDR headroom poll of the surface needs the main run loop).
void pump_events(double seconds);
// NSScreen maximumExtendedDynamicRangeColorComponentValue of the window's screen, now.
float current_headroom();

}  // namespace ovt::render::manual

#endif  // OVT_RENDER_TESTS_MANUAL_EDR_WINDOW_H
