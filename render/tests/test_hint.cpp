// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// WI-1.411 (§10.3.6): the pl_swapchain_colorspace_hint() of a NATIVE target carries the content's static HDR
// metadata. The hint is built by swapchain_hint() from the picture's mapped colour space; the surface sends it on init
// and when the source's metadata changes (checked on a VK_EXT_headless_surface behind set_native_provider_for_tests()).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <memory>
#include <string>
#include <vector>

#include <libplacebo/colorspace.h>
#include <vulkan/vulkan.h>

#include "color/h273_map.h"
#include "ovt/render/video_surface.h"
#include "platform/native_target.h"
#include "render_test_support.h"

namespace {

using Catch::Matchers::WithinRel;
using ovt::render::DisplayTarget;
using ovt::render::VideoSurface;
using ovt::render::ViewState;
using Encoding = DisplayTarget::OutputEncoding;

// A BT.2020 PQ description with ST 2086 = P3-D65 primaries, 1000 / 0.005 cd/m², and MaxCLL / MaxFALL 800 / 400.
ovt_ColorDescription pq_description(uint32_t max_luminance_units, uint32_t min_luminance_units) {
  ovt_ColorDescription c = {};
  c.struct_size = sizeof(ovt_ColorDescription);
  c.bit_depth = 10;
  c.chroma_format = 1;
  c.primaries = 9;
  c.transfer = 16;
  c.matrix = 9;
  c.chroma_siting = 2;
  c.mdcv_primaries[0][0] = 34000;  // R (0.680, 0.320), 0.00002 units
  c.mdcv_primaries[0][1] = 16000;
  c.mdcv_primaries[1][0] = 13250;  // G (0.265, 0.690)
  c.mdcv_primaries[1][1] = 34500;
  c.mdcv_primaries[2][0] = 7500;  // B (0.150, 0.060)
  c.mdcv_primaries[2][1] = 3000;
  c.mdcv_white_point[0] = 15635;  // D65 (0.3127, 0.3290)
  c.mdcv_white_point[1] = 16450;
  c.mdcv_max_luminance = max_luminance_units;  // 0.0001 cd/m² units
  c.mdcv_min_luminance = min_luminance_units;
  c.max_cll = 800;
  c.max_fall = 400;
  return c;
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

// The hint of `encoding` on a surface offering every pair (MoltenVK-like), without metadata.
struct pl_color_space base_hint(Encoding encoding) {
  const std::vector<VkSurfaceFormatKHR> all = {{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                               {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_BT2020_LINEAR_EXT},
                                               {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT},
                                               {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_HLG_EXT}};
  const auto choice = ovt::render::detail::choose_native_encoding(hdr_target(encoding), all);
  REQUIRE(choice.encoding == encoding);
  return choice.hint;
}

}  // namespace

TEST_CASE("render_test_hint: PQ source with MDCV 1000/0.005 is hinted with its metadata (acceptance test 1)",
          "[fast][render]") {
  using ovt::render::detail::map_color_description;
  using ovt::render::detail::swapchain_hint;
  const auto mapped = map_color_description(pq_description(10000000, 50), 3840, 2160);

  const struct pl_color_space hint = swapchain_hint(base_hint(Encoding::Pq10), Encoding::Pq10, mapped.space.hdr);
  CHECK(hint.primaries == PL_COLOR_PRIM_BT_2020);  // the pair of the encoding, not the content's
  CHECK(hint.transfer == PL_COLOR_TRC_PQ);
  CHECK(hint.hdr.max_luma == 1000.0F);
  CHECK_THAT(hint.hdr.min_luma, WithinRel(0.005F, 1e-6F));
  CHECK(hint.hdr.max_cll == 800.0F);
  CHECK(hint.hdr.max_fall == 400.0F);
  CHECK_THAT(hint.hdr.prim.red.x, WithinRel(0.680F, 1e-6F));
  CHECK_THAT(hint.hdr.prim.green.y, WithinRel(0.690F, 1e-6F));
  CHECK_THAT(hint.hdr.prim.blue.x, WithinRel(0.150F, 1e-6F));
  CHECK_THAT(hint.hdr.prim.white.x, WithinRel(0.3127F, 1e-6F));

  // HLG output carries the same static metadata.
  const struct pl_color_space hlg = swapchain_hint(base_hint(Encoding::Hlg10), Encoding::Hlg10, mapped.space.hdr);
  CHECK(hlg.transfer == PL_COLOR_TRC_HLG);
  CHECK(hlg.hdr.max_luma == 1000.0F);
  CHECK(hlg.hdr.max_cll == 800.0F);
}

TEST_CASE("render_test_hint: SDR and scRGB targets and dynamic metadata are not hinted (§10.3.6)", "[fast][render]") {
  using ovt::render::detail::swapchain_hint;
  struct pl_hdr_metadata content = pl_hdr_metadata_empty;
  content.max_luma = 4000.0F;
  content.min_luma = 0.0050F;
  content.max_cll = 1200.0F;
  content.scene_max[0] = 900.0F;  // HDR10+ (ST 2094-40) per-scene values: not static metadata
  content.scene_avg = 120.0F;
  content.ootf.target_luma = 1000.0F;

  for (const Encoding e : {Encoding::Srgb8, Encoding::ScRgbLinear16F}) {
    const struct pl_color_space hint = swapchain_hint(base_hint(e), e, content);
    CHECK(pl_hdr_metadata_equal(&hint.hdr, &pl_hdr_metadata_empty));
  }
  const struct pl_color_space pq = swapchain_hint(base_hint(Encoding::Pq10), Encoding::Pq10, content);
  CHECK(pq.hdr.max_luma == 4000.0F);
  CHECK(pq.hdr.scene_max[0] == 0.0F);
  CHECK(pq.hdr.scene_avg == 0.0F);
  CHECK(pq.hdr.ootf.target_luma == 0.0F);

  // A PQ source without MDCV or CLL: the hint carries no metadata (libplacebo then assumes its defaults).
  const struct pl_color_space bare = swapchain_hint(base_hint(Encoding::Pq10), Encoding::Pq10, pl_hdr_metadata_empty);
  CHECK(pl_hdr_metadata_equal(&bare.hdr, &pl_hdr_metadata_empty));
  CHECK(bare.transfer == PL_COLOR_TRC_PQ);
}

TEST_CASE("render_test_hint: CAEDRMetadata.hdr10 range and the hint check (§10.4.1, §10.3.6)", "[fast][render]") {
  using ovt::render::detail::edr_hdr10_range;
  using ovt::render::detail::hint_mismatch;
  struct pl_hdr_metadata hdr = pl_hdr_metadata_empty;
  auto r = edr_hdr10_range(hdr);  // no MDCV: the HDR10 range assumed without one
  CHECK(r.min_nits == 0.005F);
  CHECK(r.max_nits == 1000.0F);
  hdr.max_luma = 4000.0F;
  hdr.min_luma = 0.0001F;
  r = edr_hdr10_range(hdr);
  CHECK(r.max_nits == 4000.0F);
  CHECK(r.min_nits == 0.0001F);
  hdr.min_luma = 5000.0F;  // inconsistent MDCV: keep the maximum, minimum 0
  CHECK(edr_hdr10_range(hdr).min_nits == 0.0F);

  struct pl_color_space hinted = pl_color_space_bt2020_hlg;
  hinted.transfer = PL_COLOR_TRC_PQ;
  CHECK(hint_mismatch(hinted, hinted).empty());
  struct pl_color_space actual = hinted;
  actual.hdr.max_luma = 123.0F;  // metadata is not part of the check
  CHECK(hint_mismatch(hinted, actual).empty());
  actual.transfer = PL_COLOR_TRC_UNKNOWN;  // how libplacebo reports an SRGB_NONLINEAR swapchain
  CHECK(hint_mismatch(hinted, actual).empty());
  CHECK(hint_mismatch(pl_color_space_srgb, pl_color_space_monitor).empty());
  actual.transfer = PL_COLOR_TRC_SRGB;
  actual.primaries = PL_COLOR_PRIM_BT_709;
  const std::string why = hint_mismatch(hinted, actual);
  CHECK(why.find("primaries") != std::string::npos);
  CHECK(why.find("transfer") != std::string::npos);
}

// ---- The surface sends the hint on init and on source change ----------------------------------------------------
namespace {

struct HintLog {
  std::vector<struct pl_hdr_metadata> sent;  // set_content_metadata() calls, one per hint
};
HintLog g_hints;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): the provider is a plain function

class RecordingOutput final : public ovt::render::detail::NativeOutput {
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
  void configure(Encoding /*encoding*/, int /*width*/, int /*height*/) override {}
  void set_content_metadata(const struct pl_hdr_metadata& hdr) override { g_hints.sent.push_back(hdr); }
  [[nodiscard]] float polled_headroom() const override { return 0.0F; }
};

std::unique_ptr<ovt::render::detail::NativeOutput> create_recording(const DisplayTarget& /*target*/,
                                                                    std::string& /*error*/) {
  return std::make_unique<RecordingOutput>();
}

struct RecordingProvider {
  ovt::render::detail::NativeProvider provider{{VK_KHR_SURFACE_EXTENSION_NAME, "VK_EXT_headless_surface"},
                                               &create_recording};
  RecordingProvider() {
    g_hints = HintLog{};
    ovt::render::detail::set_native_provider_for_tests(&provider);
  }
  RecordingProvider(const RecordingProvider&) = delete;
  RecordingProvider& operator=(const RecordingProvider&) = delete;
  RecordingProvider(RecordingProvider&&) = delete;
  RecordingProvider& operator=(RecordingProvider&&) = delete;
  ~RecordingProvider() { ovt::render::detail::set_native_provider_for_tests(nullptr); }
};

int g_fake_layer = 0;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): any non-null native handle

// A 10-bit 4:2:0 PQ picture with the metadata of pq_description().
void make_pq(ovt::render::test::OwnedPicture& op, uint32_t max_luminance_units, uint64_t generation) {
  const ovt_ColorDescription keep = op.pic.color;
  op.pic.color = pq_description(max_luminance_units, 50);
  op.pic.color.bit_depth = keep.bit_depth;
  op.pic.color.chroma_format = keep.chroma_format;
  op.fill_plane(0, 512);
  op.fill_plane(1, 512);
  op.fill_plane(2, 512);
  op.pic.generation = generation;
}

}  // namespace

TEST_CASE("render_test_hint: the hint is sent on init and re-sent when the metadata changes (§10.3.6)",
          "[fast][render][vulkan]") {
  ovt::render::test::install_crash_backtrace();
  const RecordingProvider seam;
  DisplayTarget t = hdr_target(Encoding::Pq10);
  t.native = &g_fake_layer;
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
  REQUIRE(g_hints.sent.size() == 1);  // init, before any picture: no metadata
  CHECK(pl_hdr_metadata_equal(&g_hints.sent.back(), &pl_hdr_metadata_empty));

  constexpr int kW = 64;
  constexpr int kH = 32;
  ovt::render::test::OwnedPicture op(kW, kH, 10, 1, 2);
  make_pq(op, 10000000, 1);
  surface->set_frame(op.pic);
  REQUIRE(std::string(surface->last_error()).find("RND-0001") == std::string::npos);
  surface->render(ViewState{});
  const bool hdr = surface->active_encoding() == Encoding::Pq10;  // MoltenVK offers the PQ pair; Mesa's WSI does not
  if (!hdr) {
    // Srgb8 fallback (RND-0040): its hint has no metadata, so a new source changes nothing.
    CHECK(g_hints.sent.size() == 1);
    return;
  }
  REQUIRE(g_hints.sent.size() == 2);
  CHECK(g_hints.sent.back().max_luma == 1000.0F);
  CHECK_THAT(g_hints.sent.back().min_luma, WithinRel(0.005F, 1e-6F));

  make_pq(op, 10000000, 2);  // a new picture with the same metadata: no new hint
  surface->set_frame(op.pic);
  CHECK(g_hints.sent.size() == 2);

  make_pq(op, 40000000, 3);  // the stream changes to a 4000-nit master: re-sent
  surface->set_frame(op.pic);
  REQUIRE(g_hints.sent.size() == 3);
  CHECK(g_hints.sent.back().max_luma == 4000.0F);
  surface->render(ViewState{});
  CHECK(std::string(surface->last_error()).empty());
}
