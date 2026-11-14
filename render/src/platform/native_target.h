// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// NATIVE display targets (chapter 10 §10.4.1; WI-1.406): the platform seam that turns a DisplayTarget into a
// VkSurfaceKHR, and the portable rules of the output encoding. Internal to libovt-render.

#ifndef OVT_RENDER_SRC_PLATFORM_NATIVE_TARGET_H
#define OVT_RENDER_SRC_PLATFORM_NATIVE_TARGET_H

#include <memory>
#include <string>
#include <vector>

#include <libplacebo/colorspace.h>
#include <vulkan/vulkan.h>

#include "ovt/render/render_api.h"
#include "ovt/render/video_surface.h"

namespace ovt::render::detail {

// The window-system side of one NATIVE target (macOS: the host's CAMetalLayer). Lives on the render thread with the
// VideoSurface; implementations hop to the main thread themselves where the platform requires it.
class NativeOutput {
 public:
  NativeOutput() = default;
  NativeOutput(const NativeOutput&) = delete;
  NativeOutput& operator=(const NativeOutput&) = delete;
  NativeOutput(NativeOutput&&) = delete;
  NativeOutput& operator=(NativeOutput&&) = delete;
  virtual ~NativeOutput() = default;

  // Creates the VkSurfaceKHR on `instance` (returns VK_NULL_HANDLE and sets `error` on failure). Called once.
  virtual VkSurfaceKHR create_surface(VkInstance instance, PFN_vkGetInstanceProcAddr gipa, std::string& error) = 0;
  // Applies the properties of `encoding` that must be in place before the first present of a swapchain (§10.4.1:
  // EDR flag, pixel format, colour space, EDR metadata) and the drawable size in device pixels.
  virtual void configure(DisplayTarget::OutputEncoding encoding, int width, int height) = 0;
  // The display's current EDR headroom from the platform's latest poll (§10.4.1), or 0 when it reports none.
  [[nodiscard]] virtual float polled_headroom() const = 0;
};

// How libovt-render reaches the platform's native targets. `create` is null on platforms without one (a NATIVE
// target then fails with RND-0002). The instance extensions are requested (optionally) by every VideoSurface, so an
// OffscreenTexture surface can later retarget() to NATIVE on the same VkInstance.
struct NativeProvider {
  std::vector<const char*> instance_extensions;
  std::unique_ptr<NativeOutput> (*create)(const DisplayTarget& target, std::string& error) = nullptr;
};

// This platform's provider (macOS: CAMetalLayer targets of Kind::MetalLayer, macos_metal.mm), or the one a test set.
const NativeProvider& native_provider();
// Test seam: replaces the platform provider for VideoSurfaces initialised afterwards (nullptr restores it). Not
// thread-safe; render tests call it before creating a surface.
OVT_RENDER_API void set_native_provider_for_tests(const NativeProvider* provider);

// The output a NATIVE target gets on a surface that offers `formats` (chapter 10 §10.4.1, §10.4.2, RND-0040).
struct NativeChoice {
  DisplayTarget::OutputEncoding encoding = DisplayTarget::OutputEncoding::Srgb8;
  // Handed to pl_swapchain_colorspace_hint() so libplacebo picks the surface format pair of `encoding`.
  struct pl_color_space hint = {};
  // True when an HDR encoding was requested on an HDR-capable display but the surface lacks its format pair:
  // last_error() reports "RND-0040 no HDR surface format; falling back to Srgb8".
  bool no_hdr_format = false;
};

// Rules: Srgb8 requested, or the display is not HDR capable (DisplayTarget.hdr_capable from display_probe(),
// §10.4.5) -> Srgb8. Otherwise the requested encoding when the surface offers a pair libplacebo can drive for it,
// else Srgb8 with no_hdr_format.
//
// SPEC-ASSUMPTION(#470): ScRgbLinear16F uses {R16G16B16A16_SFLOAT, BT2020_LINEAR_EXT}: libplacebo 7.349's swapchain
// refuses EXTENDED_SRGB_LINEAR_EXT; MoltenVK maps BT2020_LINEAR_EXT to kCGColorSpaceExtendedLinearITUR_2020, the same
// extended-range linear EDR encoding (1.0 = SDR white) with BT.2020 primaries.
OVT_RENDER_API NativeChoice choose_native_encoding(const DisplayTarget& target,
                                                   const std::vector<VkSurfaceFormatKHR>& formats);

// The target colour space a NATIVE swapchain renders to: target_color() of §10.3.2 with the primaries of the chosen
// pair (BT.2020 for the ScRgbLinear16F pair above).
OVT_RENDER_API struct pl_color_space native_target_color(const DisplayTarget& target,
                                                         DisplayTarget::OutputEncoding encoding);

// §10.4.1: a polled headroom differing from the current one by more than 5 % triggers retarget().
OVT_RENDER_API bool headroom_changed(float current, float polled);

// §10.4.1 layer properties per encoding, as portable names (macos_metal.mm maps them to Metal / Core Graphics).
struct MetalLayerConfig {
  enum class PixelFormat : uint8_t { BGRA8Unorm, RGBA16Float, BGR10A2Unorm } pixel_format = PixelFormat::BGRA8Unorm;
  enum class ColorSpace : uint8_t { SRGB, ExtendedLinearITUR_2020, ITUR_2100_PQ, ITUR_2100_HLG };
  ColorSpace color_space = ColorSpace::SRGB;
  enum class EdrMetadata : uint8_t { None, HDR10, HLG } edr_metadata = EdrMetadata::None;
  bool wants_edr = false;
};
OVT_RENDER_API MetalLayerConfig metal_layer_config(DisplayTarget::OutputEncoding encoding);

}  // namespace ovt::render::detail

#endif  // OVT_RENDER_SRC_PLATFORM_NATIVE_TARGET_H
