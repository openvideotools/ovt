// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Portable rules of display_probe() (chapter 10 §10.4.5; WI-1.407), see display_caps_rules.h.

#include "display_caps_rules.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "platform/native_target.h"

namespace ovt::render::detail {

namespace {
ScreenQuery g_screen_query = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

constexpr float kActiveHeadroom = 1.05F;  // §10.4.5 step 5
constexpr float kXdrPotential = 4.0F;     // macOS: Pro Display XDR / mini-LED panels report ~8..16

using Encoding = DisplayTarget::OutputEncoding;

bool offers(const std::vector<VkSurfaceFormatKHR>& formats, Encoding encoding) {
  DisplayTarget t;
  t.hdr_capable = true;
  t.encoding = encoding;
  const NativeChoice choice = choose_native_encoding(t, formats);
  return !choice.no_hdr_format && choice.encoding == encoding;
}
}  // namespace

EncodingSupport encoding_support(const std::vector<VkSurfaceFormatKHR>& formats) {
  return {offers(formats, Encoding::Pq10), offers(formats, Encoding::Hlg10), offers(formats, Encoding::ScRgbLinear16F)};
}

void set_screen_query_for_tests(ScreenQuery query) {
  g_screen_query = query;
}

ScreenQuery screen_query_override() {
  return g_screen_query;
}

DisplayCaps unimplemented_caps(const std::string& reason, const std::string& platform_detail) {
  DisplayCaps caps = {};
  caps.headroom = 1.0F;
  caps.potential_headroom = 1.0F;
  caps.sdr_white_nits = 203.0F;
  copy_text(caps.primaries_name, "unknown");
  copy_text(caps.source, "none");
  copy_text(caps.reason, reason);
  copy_text(caps.platform_detail, platform_detail);
  return caps;
}

DisplayCaps caps_from_screen(const ScreenInfo& screen) {
  DisplayCaps caps = {};
  caps.headroom = 1.0F;
  caps.potential_headroom = 1.0F;
  caps.sdr_white_nits = std::numeric_limits<float>::quiet_NaN();  // macOS reports "relative" (§10.4.5)
  copy_text(caps.primaries_name, screen.primaries_name);
  caps.wide_gamut = screen.primaries_name == "P3" || screen.primaries_name == "BT.2020";
  if (!screen.have_screen) {
    copy_text(caps.source, "none");
    copy_text(caps.reason, "no screen");
    copy_text(caps.platform_detail, screen.os_name);
    return caps;
  }
  copy_text(caps.source, "NSScreen");
  const bool finite = std::isfinite(screen.potential_edr) && std::isfinite(screen.current_edr);
  caps.potential_headroom = finite ? std::max(screen.potential_edr, 1.0F) : 1.0F;
  const float current = finite ? std::max(screen.current_edr, 1.0F) : 1.0F;
  std::string tier = "SDR";
  std::string reason;
  if (!(caps.potential_headroom > 1.0F)) {
    reason = "display has no EDR headroom";
  } else {
    tier = caps.potential_headroom >= kXdrPotential ? "XDR" : "EDR";
    if (screen.surface_check == ScreenInfo::SurfaceCheck::Failed) {
      reason = "HDR surface probe failed: " + screen.surface_error;
    } else if (screen.surface_check == ScreenInfo::SurfaceCheck::Done) {
      const EncodingSupport s = encoding_support(screen.surface_formats);
      caps.supports_pq = s.pq;
      caps.supports_hlg = s.hlg;
      caps.supports_scrgb = s.scrgb;
      if (!s.pq && !s.hlg && !s.scrgb) {
        reason = "no HDR surface format";
      }
    }
  }
  caps.hdr_capable = reason.empty();
  caps.headroom = caps.hdr_capable ? current : 1.0F;
  caps.hdr_active = caps.hdr_capable && caps.headroom > kActiveHeadroom;
  copy_text(caps.reason, reason);
  copy_text(caps.platform_detail, screen.os_name + "; " + tier);
  return caps;
}

}  // namespace ovt::render::detail
