// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.407 display_probe() (chapter 10 §10.4.5): the portable rules with fake screens (acceptance tests 1 and 2), the
// surface-format check, and the "not implemented" stubs of the other platforms. A real screen is Karl's XDR record
// (docs/acceptance/wi/WI-1.407.md); the NSScreen glue itself has no automated test.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "ovt/render/display_caps.h"
#include "platform/display_caps_rules.h"

namespace {

using ovt::render::DisplayCaps;
using ovt::render::DisplayTarget;
using ovt::render::detail::caps_from_screen;
using ovt::render::detail::ScreenInfo;

// MoltenVK's surface formats (every format x every colour space), as in test_native_target.cpp, abridged.
std::vector<VkSurfaceFormatKHR> moltenvk_formats() {
  const std::array<VkFormat, 3> formats = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
                                           VK_FORMAT_A2B10G10R10_UNORM_PACK32};
  const std::array<VkColorSpaceKHR, 4> spaces = {VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, VK_COLOR_SPACE_BT2020_LINEAR_EXT,
                                                 VK_COLOR_SPACE_HDR10_HLG_EXT, VK_COLOR_SPACE_HDR10_ST2084_EXT};
  std::vector<VkSurfaceFormatKHR> out;
  for (const VkFormat f : formats) {
    for (const VkColorSpaceKHR c : spaces) {
      out.push_back({f, c});
    }
  }
  return out;
}

ScreenInfo xdr_screen() {
  ScreenInfo s;
  s.have_screen = true;
  s.current_edr = 1.0F;  // no EDR content on screen yet
  s.potential_edr = 16.0F;
  s.primaries_name = "P3";
  s.os_name = "macOS 15.5";
  s.surface_check = ScreenInfo::SurfaceCheck::Done;
  s.surface_formats = moltenvk_formats();
  return s;
}

ScreenInfo sdr_screen() {
  ScreenInfo s = xdr_screen();
  s.potential_edr = 1.0F;
  s.primaries_name = "BT.709";
  s.surface_check = ScreenInfo::SurfaceCheck::NotRun;  // not needed: no headroom
  s.surface_formats.clear();
  return s;
}

}  // namespace

TEST_CASE("render_test_display_caps: SDR-only Mac is not HDR capable (acceptance test 1)", "[fast][render]") {
  const DisplayCaps caps = caps_from_screen(sdr_screen());
  CHECK_FALSE(caps.hdr_capable);
  CHECK_FALSE(caps.hdr_active);
  CHECK(std::string(caps.reason) == "display has no EDR headroom");
  CHECK(caps.headroom == 1.0F);
  CHECK(caps.potential_headroom == 1.0F);
  CHECK(std::string(caps.source) == "NSScreen");
  CHECK(std::string(caps.primaries_name) == "BT.709");
  CHECK_FALSE(caps.wide_gamut);
  CHECK(std::string(caps.platform_detail) == "macOS 15.5; SDR");
}

TEST_CASE("render_test_display_caps: XDR Mac reports potential headroom (acceptance test 2)", "[fast][render]") {
  const DisplayCaps caps = caps_from_screen(xdr_screen());
  CHECK(caps.potential_headroom >= 1.5F);
  CHECK(caps.hdr_capable);
  CHECK(std::string(caps.reason).empty());
  CHECK(std::string(caps.platform_detail) == "macOS 15.5; XDR");
  CHECK(std::string(caps.primaries_name) == "P3");
  CHECK(caps.wide_gamut);
  CHECK(caps.supports_pq);
  CHECK(caps.supports_hlg);
  CHECK(caps.supports_scrgb);
  CHECK(std::isnan(caps.sdr_white_nits));
  CHECK(caps.max_nits == 0.0F);
}

TEST_CASE("render_test_display_caps: capability uses the potential value, headroom the current one", "[fast][render]") {
  // maximumExtendedDynamicRangeColorComponentValue is 1.0 until EDR content is on screen (spec pitfall).
  ScreenInfo idle = xdr_screen();
  DisplayCaps caps = caps_from_screen(idle);
  CHECK(caps.hdr_capable);
  CHECK(caps.headroom == 1.0F);
  CHECK_FALSE(caps.hdr_active);

  ScreenInfo active = xdr_screen();
  active.current_edr = 4.0F;
  caps = caps_from_screen(active);
  CHECK(caps.headroom == 4.0F);
  CHECK(caps.hdr_active);

  ScreenInfo near_one = xdr_screen();
  near_one.current_edr = 1.05F;  // hdr_active needs headroom > 1.05
  CHECK_FALSE(caps_from_screen(near_one).hdr_active);

  ScreenInfo mid = xdr_screen();
  mid.potential_edr = 2.0F;
  CHECK(std::string(caps_from_screen(mid).platform_detail) == "macOS 15.5; EDR");
}

TEST_CASE("render_test_display_caps: the probe surface decides between formats and no format", "[fast][render]") {
  ScreenInfo none = xdr_screen();
  none.surface_formats = {{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};  // sRGB only
  DisplayCaps caps = caps_from_screen(none);
  CHECK_FALSE(caps.hdr_capable);
  CHECK(std::string(caps.reason) == "no HDR surface format");
  CHECK(caps.headroom == 1.0F);
  CHECK(caps.potential_headroom == 16.0F);  // the display could, the surface cannot
  CHECK_FALSE(caps.supports_pq);

  ScreenInfo pq_only = xdr_screen();
  pq_only.surface_formats = {{VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT}};
  caps = caps_from_screen(pq_only);
  CHECK(caps.hdr_capable);
  CHECK(caps.supports_pq);
  CHECK_FALSE(caps.supports_hlg);
  CHECK_FALSE(caps.supports_scrgb);

  ScreenInfo failed = xdr_screen();
  failed.surface_check = ScreenInfo::SurfaceCheck::Failed;
  failed.surface_error = "vkCreateInstance failed";
  caps = caps_from_screen(failed);
  CHECK_FALSE(caps.hdr_capable);
  CHECK(std::string(caps.reason) == "HDR surface probe failed: vkCreateInstance failed");
}

TEST_CASE("render_test_display_caps: no screen, odd values and long strings", "[fast][render]") {
  ScreenInfo gone;
  gone.os_name = "macOS 15.5";
  DisplayCaps caps = caps_from_screen(gone);
  CHECK_FALSE(caps.hdr_capable);
  CHECK(std::string(caps.reason) == "no screen");
  CHECK(std::string(caps.source) == "none");

  ScreenInfo nan = xdr_screen();
  nan.potential_edr = std::nanf("");
  CHECK_FALSE(caps_from_screen(nan).hdr_capable);

  ScreenInfo longer = xdr_screen();
  longer.os_name = std::string(500, 'x');
  caps = caps_from_screen(longer);
  CHECK(std::string(caps.platform_detail).size() == sizeof caps.platform_detail - 1);  // truncated and terminated
}

#if defined(__APPLE__)
namespace {
ScreenInfo fake_query(const DisplayTarget& /*where*/) {
  return xdr_screen();
}
}  // namespace

TEST_CASE("render_test_display_caps: display_probe() goes through the screen provider on macOS", "[fast][render]") {
  ovt::render::detail::set_screen_query_for_tests(&fake_query);
  const DisplayCaps caps = ovt::render::display_probe(DisplayTarget{});
  ovt::render::detail::set_screen_query_for_tests(nullptr);
  CHECK(caps.hdr_capable);
  CHECK(caps.potential_headroom >= 1.5F);
}
#else
TEST_CASE("render_test_display_caps: other platforms return not-implemented reasons", "[fast][render]") {
  const DisplayCaps caps = ovt::render::display_probe(DisplayTarget{});
  CHECK_FALSE(caps.hdr_capable);
  CHECK_FALSE(caps.hdr_active);
  CHECK(std::string(caps.reason).rfind("not implemented", 0) == 0);
  CHECK(std::string(caps.source) == "none");
  CHECK(caps.headroom == 1.0F);
  CHECK(ovt::render::detail::watch_display(DisplayTarget{}, [] {}) == nullptr);
}
#endif
