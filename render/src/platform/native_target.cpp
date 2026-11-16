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
#include <iterator>
#include <string>

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
// no HDR metadata (swapchain_hint() adds the content's, §10.3.6).
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
  const auto pairs_of = [&](Encoding encoding) {
    std::vector<VkSurfaceFormatKHR> pairs;
    std::copy_if(formats.begin(), formats.end(), std::back_inserter(pairs), [&](const VkSurfaceFormatKHR& f) {
      return f.colorSpace == pair_color_space(encoding) && pair_format_ok(encoding, f.format);
    });
    return pairs;
  };
  NativeChoice choice;
  if (target.encoding != Encoding::Srgb8 && target.hdr_capable) {
    choice.pairs = pairs_of(target.encoding);
    choice.no_hdr_format = choice.pairs.empty();
    choice.encoding = choice.no_hdr_format ? Encoding::Srgb8 : target.encoding;
  }
  if (choice.encoding == Encoding::Srgb8) {
    choice.pairs = pairs_of(Encoding::Srgb8);
  }
  choice.hint = hint_for(target, choice.encoding);
  return choice;
}

struct pl_color_space native_target_color(const DisplayTarget& target, Encoding encoding) {
  struct pl_color_space c = target_color(target, encoding);
  if (encoding == Encoding::ScRgbLinear16F) {
    c.primaries = PL_COLOR_PRIM_BT_2020;
  }
  return c;
}

struct pl_color_space swapchain_hint(const struct pl_color_space& base, Encoding encoding,
                                     const struct pl_hdr_metadata& content) {
  struct pl_color_space c = base;
  c.hdr = pl_hdr_metadata_empty;
  if (encoding == Encoding::Pq10 || encoding == Encoding::Hlg10) {
    c.hdr.prim = content.prim;
    c.hdr.min_luma = content.min_luma;
    c.hdr.max_luma = content.max_luma;
    c.hdr.max_cll = content.max_cll;
    c.hdr.max_fall = content.max_fall;
  }
  return c;
}

std::string hint_mismatch(const struct pl_color_space& hinted, const struct pl_color_space& actual) {
  // An unknown value is no disagreement: libplacebo reports an SRGB_NONLINEAR swapchain as pl_color_space_monitor
  // (BT.709, transfer unknown) whatever SDR curve was hinted.
  std::string out;
  if (actual.primaries != PL_COLOR_PRIM_UNKNOWN && actual.primaries != hinted.primaries) {
    out += "primaries " + std::to_string(static_cast<int>(actual.primaries)) + " (hinted " +
           std::to_string(static_cast<int>(hinted.primaries)) + ")";
  }
  if (actual.transfer != PL_COLOR_TRC_UNKNOWN && actual.transfer != hinted.transfer) {
    out += out.empty() ? "" : ", ";
    out += "transfer " + std::to_string(static_cast<int>(actual.transfer)) + " (hinted " +
           std::to_string(static_cast<int>(hinted.transfer)) + ")";
  }
  return out;
}

Hdr10Range edr_hdr10_range(const struct pl_hdr_metadata& hdr) {
  Hdr10Range r;
  if (hdr.max_luma > 0.0F && std::isfinite(hdr.max_luma)) {
    r.max_nits = hdr.max_luma;
    r.min_nits = (hdr.min_luma >= 0.0F && hdr.min_luma < hdr.max_luma) ? hdr.min_luma : 0.0F;
  }
  return r;
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
