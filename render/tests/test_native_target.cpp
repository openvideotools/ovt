// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.406 NATIVE targets (§10.4.1): encoding rules, layer table, headroom rule, and the swapchain path on a
// VK_EXT_headless_surface behind set_native_provider_for_tests() (the CAMetalLayer itself: test_macos_metal.mm).

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

#include <libplacebo/colorspace.h>
#include <vulkan/vulkan.h>

#include "ovt/render/video_surface.h"
#include "platform/native_target.h"
#include "render_test_support.h"
#include "surface/output_download.h"

namespace {

using ovt::render::DisplayTarget;
using ovt::render::VideoSurface;
using ovt::render::ViewState;
using ovt::render::detail::MetalLayerConfig;
using Encoding = DisplayTarget::OutputEncoding;

// MoltenVK 1.2.11's surface formats on macOS 11+ (every format x every colour space), abridged.
std::vector<VkSurfaceFormatKHR> moltenvk_formats() {
  const std::array<VkFormat, 4> formats = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
                                           VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32};
  const std::array<VkColorSpaceKHR, 5> spaces = {
      VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, VK_COLOR_SPACE_BT2020_LINEAR_EXT,
      VK_COLOR_SPACE_HDR10_HLG_EXT, VK_COLOR_SPACE_HDR10_ST2084_EXT};
  std::vector<VkSurfaceFormatKHR> out;
  for (const VkFormat f : formats) {
    for (const VkColorSpaceKHR c : spaces) {
      out.push_back({f, c});
    }
  }
  return out;
}

DisplayTarget hdr_target(Encoding encoding) {
  DisplayTarget t;
  t.kind = DisplayTarget::Kind::MetalLayer;
  t.width = 64;
  t.height = 32;
  t.hdr_capable = true;
  t.headroom = 4.0F;
  t.encoding = encoding;
  return t;
}

}  // namespace

TEST_CASE("render_test_native: encoding choice from the surface formats (§10.4.1, RND-0040)", "[fast][render]") {
  using ovt::render::detail::choose_native_encoding;
  const auto mvk = moltenvk_formats();

  const auto scrgb = choose_native_encoding(hdr_target(Encoding::ScRgbLinear16F), mvk);
  CHECK(scrgb.encoding == Encoding::ScRgbLinear16F);
  CHECK_FALSE(scrgb.no_hdr_format);
  CHECK(scrgb.hint.transfer == PL_COLOR_TRC_LINEAR);
  CHECK(scrgb.hint.primaries == PL_COLOR_PRIM_BT_2020);

  const auto pq = choose_native_encoding(hdr_target(Encoding::Pq10), mvk);
  CHECK(pq.encoding == Encoding::Pq10);
  CHECK(pq.hint.transfer == PL_COLOR_TRC_PQ);
  const auto hlg = choose_native_encoding(hdr_target(Encoding::Hlg10), mvk);
  CHECK(hlg.encoding == Encoding::Hlg10);
  CHECK(hlg.hint.transfer == PL_COLOR_TRC_HLG);

  // Acceptance test 2 (rule): an SDR display (display_probe() reported hdr_capable == false) falls back to Srgb8
  // without RND-0040, whatever the surface offers.
  DisplayTarget sdr = hdr_target(Encoding::ScRgbLinear16F);
  sdr.hdr_capable = false;
  sdr.headroom = 1.0F;
  const auto fallback = choose_native_encoding(sdr, mvk);
  CHECK(fallback.encoding == Encoding::Srgb8);
  CHECK_FALSE(fallback.no_hdr_format);
  CHECK(fallback.hint.transfer == PL_COLOR_TRC_SRGB);

  CHECK(choose_native_encoding(hdr_target(Encoding::Srgb8), mvk).encoding == Encoding::Srgb8);

  // A surface with only SDR pairs, or with the scRGB space only on an 8-bit format: RND-0040, Srgb8.
  const std::vector<VkSurfaceFormatKHR> sdr_only = {{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                                    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_BT2020_LINEAR_EXT}};
  const auto none = choose_native_encoding(hdr_target(Encoding::ScRgbLinear16F), sdr_only);
  CHECK(none.encoding == Encoding::Srgb8);
  CHECK(none.no_hdr_format);
  CHECK(choose_native_encoding(hdr_target(Encoding::Pq10), sdr_only).no_hdr_format);
  CHECK(choose_native_encoding(hdr_target(Encoding::Pq10), {}).no_hdr_format);
}

TEST_CASE("render_test_native: CAMetalLayer properties per encoding (§10.4.1)", "[fast][render]") {
  using ovt::render::detail::metal_layer_config;
  const MetalLayerConfig scrgb = metal_layer_config(Encoding::ScRgbLinear16F);
  CHECK(scrgb.wants_edr);
  CHECK(scrgb.pixel_format == MetalLayerConfig::PixelFormat::RGBA16Float);
  CHECK(scrgb.color_space == MetalLayerConfig::ColorSpace::ExtendedLinearITUR_2020);
  CHECK(scrgb.edr_metadata == MetalLayerConfig::EdrMetadata::None);

  const MetalLayerConfig pq = metal_layer_config(Encoding::Pq10);
  CHECK(pq.wants_edr);
  CHECK(pq.pixel_format == MetalLayerConfig::PixelFormat::BGR10A2Unorm);
  CHECK(pq.color_space == MetalLayerConfig::ColorSpace::ITUR_2100_PQ);
  CHECK(pq.edr_metadata == MetalLayerConfig::EdrMetadata::HDR10);

  const MetalLayerConfig hlg = metal_layer_config(Encoding::Hlg10);
  CHECK(hlg.color_space == MetalLayerConfig::ColorSpace::ITUR_2100_HLG);
  CHECK(hlg.edr_metadata == MetalLayerConfig::EdrMetadata::HLG);

  const MetalLayerConfig sdr = metal_layer_config(Encoding::Srgb8);
  CHECK_FALSE(sdr.wants_edr);
  CHECK(sdr.pixel_format == MetalLayerConfig::PixelFormat::BGRA8Unorm);
  CHECK(sdr.color_space == MetalLayerConfig::ColorSpace::SRGB);
  CHECK(sdr.edr_metadata == MetalLayerConfig::EdrMetadata::None);
}

TEST_CASE("render_test_native: headroom threshold and target colour (§10.4.1, §10.3.2)", "[fast][render]") {
  using ovt::render::detail::headroom_changed;
  using ovt::render::detail::native_target_color;
  CHECK_FALSE(headroom_changed(4.0F, 4.0F));
  CHECK_FALSE(headroom_changed(4.0F, 4.19F));  // 4.75 %
  CHECK(headroom_changed(4.0F, 4.21F));        // 5.25 %
  CHECK(headroom_changed(4.0F, 3.79F));
  CHECK_FALSE(headroom_changed(4.0F, 0.0F));  // no poll yet
  CHECK(headroom_changed(0.0F, 2.0F));

  DisplayTarget t = hdr_target(Encoding::ScRgbLinear16F);
  t.headroom = 2.0F;
  const auto scrgb = native_target_color(t, Encoding::ScRgbLinear16F);
  CHECK(scrgb.transfer == PL_COLOR_TRC_LINEAR);
  CHECK(scrgb.primaries == PL_COLOR_PRIM_BT_2020);  // the BT2020_LINEAR_EXT pair
  CHECK(scrgb.hdr.max_luma == 406.0F);              // headroom x SDR white (1.0 = 203 nits)
  const auto pq = native_target_color(t, Encoding::Pq10);
  CHECK(pq.transfer == PL_COLOR_TRC_PQ);
  CHECK(pq.primaries == PL_COLOR_PRIM_BT_2020);
  CHECK(native_target_color(t, Encoding::Srgb8).transfer == PL_COLOR_TRC_SRGB);
}

// ---- Swapchain path on a headless surface ------------------------------------------------------------------------
namespace {

struct HeadlessLog {
  std::vector<Encoding> configured;
  std::vector<int> widths;
  float headroom = 0.0F;
};
HeadlessLog g_log;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): the provider is a plain function

class HeadlessOutput final : public ovt::render::detail::NativeOutput {
 public:
  VkSurfaceKHR create_surface(VkInstance instance, PFN_vkGetInstanceProcAddr gipa, std::string& error) override {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Vulkan's generic PFN_vkVoidFunction
    const auto create = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(gipa(instance, "vkCreateHeadlessSurfaceEXT"));
    if (create == nullptr) {
      error = "VK_EXT_headless_surface unavailable";
      return VK_NULL_HANDLE;
    }
    VkHeadlessSurfaceCreateInfoEXT info = {};
    info.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    if (create(instance, &info, nullptr, &surf) != VK_SUCCESS) {
      error = "vkCreateHeadlessSurfaceEXT failed";
    }
    return surf;
  }
  void configure(Encoding encoding, int width, int /*height*/) override {
    g_log.configured.push_back(encoding);
    g_log.widths.push_back(width);
  }
  [[nodiscard]] float polled_headroom() const override { return g_log.headroom; }
};

std::unique_ptr<ovt::render::detail::NativeOutput> create_headless(const DisplayTarget& /*target*/,
                                                                   std::string& /*error*/) {
  return std::make_unique<HeadlessOutput>();
}

// Installs the headless provider for one test case and restores the platform's on exit.
struct HeadlessProvider {
  ovt::render::detail::NativeProvider provider{{VK_KHR_SURFACE_EXTENSION_NAME, "VK_EXT_headless_surface"},
                                               &create_headless};
  HeadlessProvider() {
    g_log = HeadlessLog{};
    ovt::render::detail::set_native_provider_for_tests(&provider);
  }
  HeadlessProvider(const HeadlessProvider&) = delete;
  HeadlessProvider& operator=(const HeadlessProvider&) = delete;
  HeadlessProvider(HeadlessProvider&&) = delete;
  HeadlessProvider& operator=(HeadlessProvider&&) = delete;
  ~HeadlessProvider() { ovt::render::detail::set_native_provider_for_tests(nullptr); }
};

int g_fake_layer = 0;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): any non-null native handle

DisplayTarget headless_target(Encoding encoding, bool hdr_capable) {
  DisplayTarget t = hdr_target(encoding);
  t.native = &g_fake_layer;
  t.hdr_capable = hdr_capable;
  return t;
}

// init() of a NATIVE headless target; skips where the host has no Vulkan device or no headless WSI.
std::unique_ptr<VideoSurface> native_or_skip(const DisplayTarget& t) {
  ovt::render::test::install_crash_backtrace();
  auto surface = VideoSurface::create("render-test");
  if (!surface->init(t)) {
    const std::string err = surface->last_error();
    if (err.find(": pl_gpu: ") != std::string::npos || err.find(": pl_vk_inst: ") != std::string::npos ||
        err.find("headless") != std::string::npos) {
      SKIP("no Vulkan device with headless WSI on this host: " << err);
    }
    FAIL("VideoSurface::init failed: " << err);
  }
  ovt::render::test::pin_vulkan_icds();
  return surface;
}

}  // namespace

TEST_CASE("render_test_native: SDR display falls back to Srgb8 on a NATIVE swapchain (acceptance test 2)",
          "[fast][render][vulkan]") {
  const HeadlessProvider seam;
  auto surface = native_or_skip(headless_target(Encoding::ScRgbLinear16F, false));
  CHECK(surface->active_encoding() == Encoding::Srgb8);
  CHECK(std::string(surface->last_error()).empty());  // not RND-0040: the display, not the surface, is SDR
  REQUIRE_FALSE(g_log.configured.empty());
  CHECK(g_log.configured.front() == Encoding::Srgb8);  // layer configured before the swapchain exists
  surface->render(ViewState{});
  CHECK(std::string(surface->last_error()).empty());
}

TEST_CASE("render_test_native: HDR on a headless surface gets its pair, or RND-0040 without one",
          "[fast][render][vulkan]") {
  const HeadlessProvider seam;
  auto surface = native_or_skip(headless_target(Encoding::Pq10, true));
#if defined(__APPLE__)
  // MoltenVK's headless surface offers every format x colour space pair, the PQ pair included.
  CHECK(surface->active_encoding() == Encoding::Pq10);
  CHECK(std::string(surface->last_error()).empty());
#else
  // Mesa's headless WSI offers sRGB pairs only.
  CHECK(surface->active_encoding() == Encoding::Srgb8);
  CHECK(std::string(surface->last_error()) == "RND-0040 no HDR surface format; falling back to Srgb8");
#endif
  // A polled headroom > 5 % away retargets without changing the encoding.
  const Encoding chosen = surface->active_encoding();
  g_log.headroom = 8.0F;
  surface->render(ViewState{});
  CHECK(surface->active_encoding() == chosen);
  // resize() reconfigures the platform object with the new drawable size.
  CHECK(surface->resize(48, 32));
  CHECK(g_log.widths.back() == 48);
  surface->render(ViewState{});
}

TEST_CASE("render_test_native: retarget() between EMBEDDED and NATIVE keeps the uploaded picture (§10.2.7)",
          "[fast][render][vulkan]") {
  using ovt::render::detail::download_output;
  using ovt::render::detail::OutputPixels;
  const HeadlessProvider seam;
  constexpr int kW = 32;
  constexpr int kH = 16;
  auto surface = ovt::render::test::surface_or_skip(kW, kH);
  ovt::render::test::OwnedPicture op(kW, kH, 8, 1, 1);
  op.fill_plane(0, 180);
  op.fill_plane(1, 128);
  op.fill_plane(2, 128);
  op.pic.generation = 1;
  surface->set_frame(op.pic);
  ViewState view;
  view.pan_x = kW / 2.0;
  view.pan_y = kH / 2.0;
  surface->render(view);
  OutputPixels before;
  REQUIRE(download_output(*surface, before));

  DisplayTarget native = headless_target(Encoding::Srgb8, false);
  native.width = kW;
  native.height = kH;
  surface->retarget(native);
  const std::string err = surface->last_error();
  if (err.find("headless") != std::string::npos) {
    SKIP("no headless WSI on this host: " << err);
  }
  REQUIRE(err.empty());
  CHECK(surface->active_encoding() == Encoding::Srgb8);
  surface->render(view);
  CHECK(std::string(surface->last_error()).empty());

  DisplayTarget embedded;
  embedded.width = kW;
  embedded.height = kH;
  surface->retarget(embedded);
  REQUIRE(std::string(surface->last_error()).empty());
  surface->render(view);
  OutputPixels after;
  REQUIRE(download_output(*surface, after));
  CHECK(after.bytes == before.bytes);  // slot A survived both retargets without a new set_frame()
}
