// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Portable rules of the NATIVE targets (chapter 10 §10.4.1, §10.4.2; WI-1.406): encoding choice from the surface's
// formats, the target colour space, the headroom threshold and the CAMetalLayer properties per encoding. The provider
// table is per platform: macos_metal.mm defines native_platform_provider() on Apple; elsewhere none exists yet
// (Linux Vulkan targets arrive with WI-4.450).

#include "native_target.h"

#include <algorithm>
#include <cmath>

#include "pipeline/render_params.h"

namespace ovt::render::detail {

#if defined(__APPLE__)
const NativeProvider& native_platform_provider();  // macos_metal.mm
#else
namespace {
const NativeProvider& native_platform_provider() {
  static const NativeProvider none{};
  return none;
}
}  // namespace
#endif

namespace {
const NativeProvider* g_test_provider = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

using Encoding = DisplayTarget::OutputEncoding;

// The colour space (and the transfer libplacebo needs for it) that carries `encoding` in a swapchain.
VkColorSpaceKHR pair_color_space(Encoding encoding) {
  switch (encoding) {
    case Encoding::ScRgbLinear16F:
      return VK_COLOR_SPACE_BT2020_LINEAR_EXT;  // SPEC-ASSUMPTION(#470), see native_target.h
    case Encoding::Pq10:
      return VK_COLOR_SPACE_HDR10_ST2084_EXT;
    case Encoding::Hlg10:
      return VK_COLOR_SPACE_HDR10_HLG_EXT;
    case Encoding::Srgb8:
      break;
  }
  return VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
}

// The formats libplacebo 7.349's swapchain accepts with each colour space (src/vulkan/swapchain.c pick_surf_format():
// float formats only for linear curves, 10-bit and 16-bit UNORM for the non-linear HDR curves).
bool pair_format_ok(Encoding encoding, VkFormat f) {
  switch (encoding) {
    case Encoding::ScRgbLinear16F:
      return f == VK_FORMAT_R16G16B16A16_SFLOAT;
    case Encoding::Pq10:
    case Encoding::Hlg10:
      return f == VK_FORMAT_A2R10G10B10_UNORM_PACK32 || f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
             f == VK_FORMAT_R16G16B16A16_UNORM;
    case Encoding::Srgb8:
      break;
  }
  return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM;
}

// The pl_swapchain_colorspace_hint() that makes libplacebo pick the pair of `encoding`: its primaries and transfer,
// no HDR metadata (the content's static metadata is WI-1.411's, §10.3.6).
struct pl_color_space hint_for(const DisplayTarget& target, Encoding encoding) {
  struct pl_color_space c = native_target_color(target, encoding);
  c.hdr = pl_hdr_metadata_empty;
  return c;
}

}  // namespace

const NativeProvider& native_provider() {
  return g_test_provider != nullptr ? *g_test_provider : native_platform_provider();
}

void set_native_provider_for_tests(const NativeProvider* provider) {
  g_test_provider = provider;
}

NativeChoice choose_native_encoding(const DisplayTarget& target, const std::vector<VkSurfaceFormatKHR>& formats) {
  NativeChoice choice;
  choice.encoding = Encoding::Srgb8;
  choice.hint = hint_for(target, Encoding::Srgb8);
  if (target.encoding == Encoding::Srgb8 || !target.hdr_capable) {
    return choice;
  }
  const VkColorSpaceKHR space = pair_color_space(target.encoding);
  const bool offered = std::any_of(formats.begin(), formats.end(), [&](const VkSurfaceFormatKHR& f) {
    return f.colorSpace == space && pair_format_ok(target.encoding, f.format);
  });
  if (!offered) {
    choice.no_hdr_format = true;
    return choice;
  }
  choice.encoding = target.encoding;
  choice.hint = hint_for(target, target.encoding);
  return choice;
}

struct pl_color_space native_target_color(const DisplayTarget& target, Encoding encoding) {
  struct pl_color_space c = target_color(target, encoding);
  if (encoding == Encoding::ScRgbLinear16F) {
    c.primaries = PL_COLOR_PRIM_BT_2020;
  }
  return c;
}

bool headroom_changed(float current, float polled) {
  if (!(polled > 0.0F) || !std::isfinite(polled)) {
    return false;
  }
  if (!(current > 0.0F)) {
    return true;
  }
  return std::fabs(polled - current) > 0.05F * current;
}

MetalLayerConfig metal_layer_config(Encoding encoding) {
  MetalLayerConfig c;
  switch (encoding) {
    case Encoding::ScRgbLinear16F:
      c.pixel_format = MetalLayerConfig::PixelFormat::RGBA16Float;
      c.color_space = MetalLayerConfig::ColorSpace::ExtendedLinearITUR_2020;
      c.wants_edr = true;
      break;  // no EDR metadata: libplacebo tone-maps to the headroom itself
    case Encoding::Pq10:
      c.pixel_format = MetalLayerConfig::PixelFormat::BGR10A2Unorm;
      c.color_space = MetalLayerConfig::ColorSpace::ITUR_2100_PQ;
      c.edr_metadata = MetalLayerConfig::EdrMetadata::HDR10;
      c.wants_edr = true;
      break;
    case Encoding::Hlg10:
      c.pixel_format = MetalLayerConfig::PixelFormat::BGR10A2Unorm;
      c.color_space = MetalLayerConfig::ColorSpace::ITUR_2100_HLG;
      c.edr_metadata = MetalLayerConfig::EdrMetadata::HLG;
      c.wants_edr = true;
      break;
    case Encoding::Srgb8:
      break;
  }
  return c;
}

}  // namespace ovt::render::detail
