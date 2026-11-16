// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Portable rules of display_probe() (chapter 10 §10.4.5; WI-1.407): what a platform's screen query reports (ScreenInfo)
// becomes a DisplayCaps here, so the rules run in every test without a screen. The platform files
// (display_caps_{macos.mm,linux.cpp,android.cpp}) only query the system and define display_probe(). Internal to
// libovt-render.

#ifndef OVT_RENDER_SRC_PLATFORM_DISPLAY_CAPS_RULES_H
#define OVT_RENDER_SRC_PLATFORM_DISPLAY_CAPS_RULES_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "ovt/render/display_caps.h"
#include "ovt/render/render_api.h"
#include "ovt/render/video_surface.h"

namespace ovt::render::detail {

// What the platform reported about one screen, before the §10.4.5 rules are applied.
struct ScreenInfo {
  bool have_screen = false;                // false: no screen could be found for the target
  float current_edr = 1.0F;                // maximumExtendedDynamicRangeColorComponentValue (macOS); 1.0 = SDR
  float potential_edr = 1.0F;              // maximumPotentialExtendedDynamicRangeColorComponentValue
  std::string primaries_name = "unknown";  // "BT.709", "P3", "BT.2020", "unknown"
  std::string os_name;                     // "macOS 15.5"
  // Result of the probe-surface format check (§10.4.5 step 3).
  enum class SurfaceCheck : uint8_t { NotRun, Failed, Done } surface_check = SurfaceCheck::NotRun;
  std::string surface_error;                        // set when surface_check == Failed
  std::vector<VkSurfaceFormatKHR> surface_formats;  // set when surface_check == Done
};

// Applies the §10.4.5 algorithm (macOS source "NSScreen") to `screen`:
//  - no screen -> not capable, reason "no screen", source "none";
//  - potential <= 1 -> reason "display has no EDR headroom";
//  - the probe surface failed -> reason "HDR surface probe failed: <error>"; offers no pair -> "no HDR surface format";
//  - otherwise capable, headroom = max(current, 1) (the current value is 1.0 until EDR content is on screen, so only
//    the potential value decides hdr_capable), hdr_active = capable && headroom > 1.05.
OVT_RENDER_API DisplayCaps caps_from_screen(const ScreenInfo& screen);

// Which of the three HDR output encodings `formats` can carry (the pairs choose_native_encoding() would pick).
struct EncodingSupport {
  bool pq = false, hlg = false, scrgb = false;  // NOLINT(readability-isolate-declaration)
};
OVT_RENDER_API EncodingSupport encoding_support(const std::vector<VkSurfaceFormatKHR>& formats);

// Test seam: the platform's screen query is replaced by `query` for display_probe() calls afterwards (nullptr
// restores it). Used by the macOS display_probe(); not thread-safe, tests call it before probing.
using ScreenQuery = ScreenInfo (*)(const DisplayTarget& where);
OVT_RENDER_API void set_screen_query_for_tests(ScreenQuery query);
ScreenQuery screen_query_override();

// Re-probe triggers (§10.4.5 step 4): while the object lives, `on_change` is called (on the platform's UI thread) when
// the screen changes, or its potential or current EDR headroom changes by more than 5 % (headroom_changed()), seen by
// screen-parameter notifications and a 500 ms timer. The host then calls display_probe() again. nullptr where the
// platform has no such trigger (Linux: WI-4.451, Android: WI-4.458). Destruction never blocks on the UI thread.
class DisplayWatcher {
 public:
  DisplayWatcher() = default;
  DisplayWatcher(const DisplayWatcher&) = delete;
  DisplayWatcher& operator=(const DisplayWatcher&) = delete;
  DisplayWatcher(DisplayWatcher&&) = delete;
  DisplayWatcher& operator=(DisplayWatcher&&) = delete;
  virtual ~DisplayWatcher() = default;
};
OVT_RENDER_API std::unique_ptr<DisplayWatcher> watch_display(const DisplayTarget& where,
                                                             std::function<void()> on_change);

// Copies `text` into a fixed char array, truncating (the arrays of DisplayCaps are 16..128 bytes) and terminating.
void copy_text(char* dest, std::size_t capacity, const std::string& text);

// A "not implemented" caps for a platform whose probe is a stub: not capable, source "none", the given reason.
DisplayCaps unimplemented_caps(const std::string& reason, const std::string& platform_detail);

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_PLATFORM_DISPLAY_CAPS_RULES_H
