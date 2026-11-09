// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The one concrete VideoSurface (chapter 10 §10.1.3, §10.2.7). WI-1.400 is the skeleton: an OffscreenTexture target
// on libplacebo's Vulkan backend (MoltenVK on macOS, the Vulkan loader on Linux), cleared to
// ViewState.background_rgba by render(). WI-1.401 adds picture upload (slot A, §10.3.1) and draws it with a basic
// pl_render_image pass, with the §10.3.3 colour tables of WI-1.402. The full render parameters, target frames and
// dithering (WI-1.403), interop (WI-1.404), native targets (WI-1.406 and later), compare modes, overlays, measurements
// and export arrive with their own work items; until then those entry points are inert.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

#include "../overlay/overlay_set_impl.h"
#include "output_download.h"
#include "ovt/render/video_surface.h"
#include "pipeline/frame_upload.h"
#include "vk_no_timestamps.h"

namespace ovt::render {
namespace {

// libplacebo's messages go to stderr with the surface's log prefix.
// SPEC-ASSUMPTION(#209): libovt_core exports no log emit function, so RND codes reach the host through last_error()
// and libplacebo's own messages through stderr until a C entry point exists.
void placebo_log_cb(void* priv, enum pl_log_level level, const char* msg) {
  const auto* prefix = static_cast<const std::string*>(priv);
  const char* tag = level <= PL_LOG_ERR ? "error" : (level == PL_LOG_WARN ? "warn" : "info");
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  (void)std::fprintf(stderr, "[%s] libplacebo %s: %s\n", prefix->c_str(), tag, msg);
}

enum pl_log_level placebo_log_level() {
  // §10.11 WI-1.400 notes: PL_LOG_WARN normally, PL_LOG_DEBUG behind OVT_RENDER_DEBUG=1.
  const char* debug = std::getenv("OVT_RENDER_DEBUG");  // NOLINT(concurrency-mt-unsafe): read once per init()
  return (debug != nullptr && std::strcmp(debug, "1") == 0) ? PL_LOG_DEBUG : PL_LOG_WARN;
}

const char* offscreen_format_name(DisplayTarget::OffscreenFormat f) {
  switch (f) {
    case DisplayTarget::OffscreenFormat::Rgba8Srgb:
      return "rgba8";  // UNORM storage; the sRGB encoding is applied by the pipeline (§10.3.2), not the format
    case DisplayTarget::OffscreenFormat::Rgba16F:
      return "rgba16hf";  // IEEE half float ("rgba16f" is libplacebo's emulated 32-bit-host variant)
    case DisplayTarget::OffscreenFormat::Rgb10A2:
      return "rgb10a2";
  }
  return "rgba8";
}

// §10.3.2 target colour per offscreen format (Rgba8Srgb -> Srgb8 row, Rgba16F -> ScRgbLinear16F row,
// Rgb10A2 -> Pq10 row).
struct pl_color_space target_color(const DisplayTarget& t) {
  struct pl_color_space c = pl_color_space_unknown;
  switch (t.offscreen_format) {
    case DisplayTarget::OffscreenFormat::Rgba8Srgb:
      c.primaries = PL_COLOR_PRIM_BT_709;
      c.transfer = PL_COLOR_TRC_SRGB;
      c.hdr.max_luma = PL_COLOR_SDR_WHITE;
      break;
    case DisplayTarget::OffscreenFormat::Rgba16F:
      c.primaries = PL_COLOR_PRIM_BT_709;
      c.transfer = PL_COLOR_TRC_LINEAR;
      c.hdr.max_luma = t.headroom * t.sdr_white_nits;
      break;
    case DisplayTarget::OffscreenFormat::Rgb10A2:
      c.primaries = PL_COLOR_PRIM_BT_2020;
      c.transfer = PL_COLOR_TRC_PQ;
      c.hdr.max_luma = 1000.0F;  // display max from DisplayCaps when known (WI-1.403/1.407)
      break;
  }
  return c;
}

// SPEC-ASSUMPTION(#343): the Vulkan instance and device that every offscreen surface of the process renders with. It
// is created by the first successful init() and deliberately never destroyed.
//
// Destroying a device is not safe on MoltenVK: Metal delivers a command buffer's completion notification on its own
// IOGPU notification thread, and that thread keeps using the MTLCommandQueue (-[_MTLCommandQueue
// commandBufferDidComplete:...] -> MTLDispatchListApply) after the last completed handler has run. Every signal a
// Vulkan application can wait on (timeline semaphores, fences, vkDeviceWaitIdle, which ends in -waitUntilCompleted)
// fires before or inside that processing, so vkDestroyDevice, which releases the MTLCommandQueue, can always overtake
// it. Short-lived surfaces that each owned a device crashed there intermittently (SIGSEGV at 0x60 in
// MTLDispatchListApply on the Mac Mini, #304, #330); waiting for the device to idle first did not help. With one
// device per process no queue is ever released while the process runs, and at exit nothing is torn down.
//
// libplacebo's pl_gpu is thread-safe, so surfaces on different threads can share it; each surface keeps its own
// pl_renderer, textures and log. A future native or interop target that must use the host's device imports it with
// pl_vulkan_import (§10.4) instead.
struct SharedVulkan {
  pl_log log = nullptr;
  pl_vk_inst inst = nullptr;
  pl_vulkan vk = nullptr;
};

// Returns the process-wide device, creating it on first use. On failure it releases what it created, sets `stage` and
// `reason` for RND-0002, returns nullptr, and tries again on the next call.
const SharedVulkan* shared_vulkan(std::string& stage, std::string& reason) {
  static std::mutex mutex;
  static SharedVulkan* shared = nullptr;  // never deleted: see above
  static std::string prefix = "render";   // log_priv of the shared pl_log, which also lives forever
  const std::lock_guard<std::mutex> lock(mutex);
  if (shared != nullptr) {
    return shared;
  }
  SharedVulkan s;
  struct pl_log_params log_params = {};
  log_params.log_cb = placebo_log_cb;
  log_params.log_priv = &prefix;
  log_params.log_level = placebo_log_level();
  s.log = pl_log_create(PL_API_VER, &log_params);
  if (s.log == nullptr) {
    stage = "pl_log";
    reason = "pl_log_create failed";
    return nullptr;
  }
  const struct pl_vk_inst_params inst_params = pl_vk_inst_default_params;
  s.inst = pl_vk_inst_create(s.log, &inst_params);
  if (s.inst == nullptr) {
    pl_log_destroy(&s.log);
    stage = "pl_vk_inst";
    reason = "no Vulkan instance (Vulkan loader or MoltenVK missing)";
    return nullptr;
  }
  struct pl_vulkan_params vk_params = pl_vulkan_default_params;
  vk_params.instance = s.inst->instance;
  // libplacebo loads its Vulkan functions through this wrapper, which hides timestamp support so libplacebo never
  // writes timer queries (MoltenVK resolves them after libplacebo may have freed the query pool; vk_no_timestamps.cpp).
  vk_params.get_proc_addr = detail::no_timestamps_get_proc_addr(s.inst->get_proc_addr);
  // Software rasterizers (lavapipe on GPU-less CI hosts) are accepted; libplacebo still prefers a hardware device.
  vk_params.allow_software = true;
  s.vk = pl_vulkan_create(s.log, &vk_params);
  if (s.vk == nullptr) {
    // Nothing was submitted on a device that never existed, so tearing down the instance here is safe.
    pl_vk_inst_destroy(&s.inst);
    pl_log_destroy(&s.log);
    stage = "pl_gpu";
    reason = "pl_vulkan_create found no usable Vulkan device";
    return nullptr;
  }
  shared = new SharedVulkan(s);  // NOLINT(cppcoreguidelines-owning-memory): process lifetime, never deleted
  return shared;
}

class VideoSurfaceImpl final : public VideoSurface {
 public:
  explicit VideoSurfaceImpl(const char* log_prefix) : prefix_(log_prefix != nullptr ? log_prefix : "render") {}
  VideoSurfaceImpl(const VideoSurfaceImpl&) = delete;
  VideoSurfaceImpl& operator=(const VideoSurfaceImpl&) = delete;
  VideoSurfaceImpl(VideoSurfaceImpl&&) = delete;
  VideoSurfaceImpl& operator=(VideoSurfaceImpl&&) = delete;
  ~VideoSurfaceImpl() override { destroy(); }

  bool init(const DisplayTarget& target) override {
    destroy();
    error_.clear();
    if (target.kind != DisplayTarget::Kind::OffscreenTexture) {
      return fail("target", "only DisplayTarget::Kind::OffscreenTexture is implemented");
    }
    if (target.interop != DisplayTarget::Interop::None) {
      return fail("target", "offscreen interop is not implemented (WI-1.404)");
    }
    if (target.width <= 0 || target.height <= 0) {
      return fail("offscreen target", "width and height must be positive");
    }

    // The surface's own log (its prefix) serves its renderer; the shared device logs as "render".
    struct pl_log_params log_params = {};
    log_params.log_cb = placebo_log_cb;
    log_params.log_priv = &prefix_;
    log_params.log_level = placebo_log_level();
    log_ = pl_log_create(PL_API_VER, &log_params);
    if (log_ == nullptr) {
      return fail("pl_log", "pl_log_create failed");
    }

    std::string stage;
    std::string reason;
    const SharedVulkan* shared = shared_vulkan(stage, reason);
    if (shared == nullptr) {
      return fail(stage, reason);
    }
    vk_ = shared->vk;

    renderer_ = pl_renderer_create(log_, vk_->gpu);
    if (renderer_ == nullptr) {
      return fail("pl_renderer", "pl_renderer_create failed");
    }

    target_ = target;
    return create_texture(target.width, target.height);
  }

  bool resize(int width, int height) override {
    if (vk_ == nullptr) {
      return fail("resize", "surface is not initialised");
    }
    if (width <= 0 || height <= 0) {
      return fail("resize", "width and height must be positive");
    }
    if (tex_ != nullptr && width == target_.width && height == target_.height) {
      return true;
    }
    target_.width = width;
    target_.height = height;
    return create_texture(width, height);
  }

  void retarget(const DisplayTarget& target) override {
    // Only OffscreenTexture exists so far (WI-1.406 adds NATIVE targets): a new size or format re-creates the output
    // texture on the same pl_gpu.
    if (vk_ == nullptr || target.kind != DisplayTarget::Kind::OffscreenTexture ||
        target.interop != DisplayTarget::Interop::None) {
      fail("retarget", "only OffscreenTexture without interop is implemented");
      return;
    }
    target_ = target;
    create_texture(target.width, target.height);
  }

  // SPEC-ASSUMPTION(#209): set_frame() is void (§10.2.7); a rejected picture leaves slot A empty and last_error()
  // reports RND-0001.
  void set_frame(const Picture& a) override {
    if (vk_ == nullptr) {
      error_ = "RND-0001 picture rejected: surface is not initialised";
      return;
    }
    std::vector<std::string> notices;
    std::string err = detail::upload_picture(vk_->gpu, a, slot_a_, notice_log_, notices);
    for (const std::string& line : notices) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      (void)std::fprintf(stderr, "[%s] %s\n", prefix_.c_str(), line.c_str());
    }
    if (!err.empty()) {
      error_ = std::move(err);
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      (void)std::fprintf(stderr, "[%s] %s\n", prefix_.c_str(), error_.c_str());
      return;
    }
    error_.clear();
  }
  void set_compare(const Picture& /*b*/, const CompareMode& /*mode*/) override {}  // WI-1.410
  void set_overlays(std::unique_ptr<OverlaySet> overlays) override { overlays_ = std::move(overlays); }

  void render(const ViewState& view) override {
    if (tex_ == nullptr) {
      return;
    }
    // background_rgba is 0xRRGGBBAA in the output encoding (straight alpha).
    union pl_clear_color color = {};
    const uint32_t bg = view.background_rgba;
    color.f[0] = static_cast<float>((bg >> 24U) & 0xFFU) / 255.0F;
    color.f[1] = static_cast<float>((bg >> 16U) & 0xFFU) / 255.0F;
    color.f[2] = static_cast<float>((bg >> 8U) & 0xFFU) / 255.0F;
    color.f[3] = static_cast<float>(bg & 0xFFU) / 255.0F;
    pl_tex_clear_ex(vk_->gpu, tex_, color);
    if (slot_a_.valid) {
      draw_slot_a(view);
    }
    pl_gpu_flush(vk_->gpu);
  }

  [[nodiscard]] ExternalTexture output_texture() const override {
    // Interop::None: the handle is the pl_tex itself (only libovt-render dereferences it); WI-1.404 returns the
    // platform handles.
    ExternalTexture t{};
    t.interop = DisplayTarget::Interop::None;
    t.handle = const_cast<void*>(static_cast<const void*>(tex_));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    t.handle2 = nullptr;
    t.width = tex_ != nullptr ? target_.width : 0;
    t.height = tex_ != nullptr ? target_.height : 0;
    t.format = target_.offscreen_format;
    t.layout = 0;
    return t;
  }

  [[nodiscard]] Pick pick(int /*px*/, int /*py*/) const override { return Pick{}; }  // §10.5.7
  Measurements measure() override { return Measurements{}; }                         // §10.7.9

  bool export_png(const char* /*path*/, ExportKind /*kind*/, const ViewState* /*view_or_null*/) override {
    error_ = "export_png is not implemented yet (chapter 10 §10.8)";
    return false;
  }

  [[nodiscard]] DisplayTarget::OutputEncoding active_encoding() const override { return target_.encoding; }
  [[nodiscard]] const char* last_error() const override { return error_.c_str(); }

  bool chroma_shift(float& sx, float& sy) const {
    if (!slot_a_.valid || slot_a_.frame.num_planes < 3) {
      return false;
    }
    sx = slot_a_.frame.planes[1].shift_x;
    sy = slot_a_.frame.planes[1].shift_y;
    return true;
  }

  bool download(detail::OutputPixels& out) const {
    out = detail::OutputPixels{};
    if (tex_ == nullptr) {
      return false;
    }
    const size_t texel = tex_->params.format->texel_size;
    const auto w = static_cast<size_t>(target_.width);
    const auto h = static_cast<size_t>(target_.height);
    std::vector<uint8_t> bytes(w * h * texel);
    struct pl_tex_transfer_params tp = {};
    tp.tex = tex_;
    tp.row_pitch = w * texel;
    tp.ptr = bytes.data();
    if (!pl_tex_download(vk_->gpu, &tp)) {
      return false;
    }
    pl_gpu_finish(vk_->gpu);
    out.width = target_.width;
    out.height = target_.height;
    out.texel_bytes = texel;
    out.bytes = std::move(bytes);
    return true;
  }

 private:
  // Draws slot A over the cleared target with a basic parameter set: nearest upscaling at zoom >= 1, the ViewState
  // chroma upscaler and downscaler, no dithering, no anti-ringing (§10.3.1 "Scaling filters"). WI-1.403 replaces this
  // with the full pl_render_params of §10.3.4/§10.3.5 and the target frames of every encoding.
  void draw_slot_a(const ViewState& view) {
    struct pl_frame image = slot_a_.frame;
    image.crop = detail::source_rect(slot_a_, view.show_cropped);

    struct pl_frame target = {};
    target.num_planes = 1;
    target.planes[0].texture = tex_;
    target.planes[0].components = 4;
    for (int c = 0; c < 4; ++c) {
      target.planes[0].component_mapping[c] = c;
    }
    target.repr = pl_color_repr_rgb;
    target.color = target_color(target_);
    // §10.2.3 viewport mapping: device = (image - pan) * zoom + viewport / 2.
    const double zoom = view.zoom > 0.0 ? view.zoom : 1.0;
    const double half_w = target_.width / 2.0;
    const double half_h = target_.height / 2.0;
    target.crop.x0 = static_cast<float>(((static_cast<double>(image.crop.x0) - view.pan_x) * zoom) + half_w);
    target.crop.y0 = static_cast<float>(((static_cast<double>(image.crop.y0) - view.pan_y) * zoom) + half_h);
    target.crop.x1 = static_cast<float>(((static_cast<double>(image.crop.x1) - view.pan_x) * zoom) + half_w);
    target.crop.y1 = static_cast<float>(((static_cast<double>(image.crop.y1) - view.pan_y) * zoom) + half_h);

    struct pl_render_params params = {};
    params.upscaler = zoom >= 1.0 ? &pl_filter_nearest : nullptr;
    switch (view.downscale) {
      case Downscale::Box:
        params.downscaler = &pl_filter_box;
        break;
      case Downscale::Lanczos:
        params.downscaler = &pl_filter_lanczos;
        break;
      case Downscale::Bilinear:
        params.downscaler = &pl_filter_bilinear;
        break;
    }
    params.plane_upscaler = view.chroma_upscale == ChromaUpscale::Nearest ? &pl_filter_nearest : &pl_filter_bilinear;
    params.skip_anti_aliasing = zoom >= 1.0;
    params.antiringing_strength = 0.0F;
    // The target was cleared to background_rgba above; keep libplacebo from clearing the border again.
#if PL_API_VER >= 346
    params.border = PL_CLEAR_SKIP;
#else
    params.skip_target_clearing = true;
#endif
    if (!pl_render_image(renderer_, &image, &target, &params)) {
      error_ = "RND-0002 renderer init failed: render: pl_render_image failed";
    }
  }

  bool create_texture(int width, int height) {
    pl_gpu gpu = vk_->gpu;
    const char* name = offscreen_format_name(target_.offscreen_format);
    pl_fmt fmt = pl_find_named_fmt(gpu, name);
    constexpr auto kNeeded = static_cast<enum pl_fmt_caps>(PL_FMT_CAP_RENDERABLE | PL_FMT_CAP_SAMPLEABLE |
                                                           PL_FMT_CAP_BLITTABLE | PL_FMT_CAP_HOST_READABLE);
    if (fmt == nullptr || (fmt->caps & kNeeded) != kNeeded) {
      pl_tex_destroy(gpu, &tex_);
      return fail("offscreen target", std::string("texture format ") + name + " is not renderable/blittable/readable");
    }
    struct pl_tex_params params = {};
    params.w = width;
    params.h = height;
    params.format = fmt;
    params.sampleable = true;
    params.renderable = true;
    params.blit_dst = true;
    params.host_readable = true;
    if (!pl_tex_recreate(gpu, &tex_, &params)) {
      return fail("offscreen target", std::string("pl_tex_recreate failed for ") + name);
    }
    return true;
  }

  bool fail(const std::string& stage, const std::string& reason) {
    error_ = "RND-0002 renderer init failed: " + stage + ": " + reason;  // §10.10
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    (void)std::fprintf(stderr, "[%s] %s\n", prefix_.c_str(), error_.c_str());
    return false;
  }

  // Releases what this surface owns. The device is shared and stays (see SharedVulkan); libplacebo defers freeing
  // textures and shaders that pending GPU work still uses until that work completes.
  void destroy() {
    overlays_.reset();
    if (vk_ != nullptr) {
      detail::destroy_slot(vk_->gpu, slot_a_);
      pl_tex_destroy(vk_->gpu, &tex_);
    }
    pl_renderer_destroy(&renderer_);
    vk_ = nullptr;
    pl_log_destroy(&log_);
  }

  std::string prefix_;
  std::string error_;
  pl_log log_ = nullptr;
  pl_vulkan vk_ = nullptr;  // the process-wide device (shared_vulkan()); not owned
  pl_tex tex_ = nullptr;
  pl_renderer renderer_ = nullptr;
  detail::FrameSlot slot_a_;
  detail::H273NoticeLog notice_log_;  // RND-0020/RND-0021 once per colour description (§10.3.3)
  DisplayTarget target_{};
  std::unique_ptr<OverlaySet> overlays_;
};

}  // namespace

std::unique_ptr<VideoSurface> VideoSurface::create(const char* log_prefix) {
  return std::make_unique<VideoSurfaceImpl>(log_prefix);
}

namespace detail {

bool download_output(const VideoSurface& surface, OutputPixels& out) {
  const auto* impl = dynamic_cast<const VideoSurfaceImpl*>(&surface);
  if (impl == nullptr) {
    out = OutputPixels{};
    return false;
  }
  return impl->download(out);
}

bool slot_a_chroma_shift(const VideoSurface& surface, float& shift_x, float& shift_y) {
  const auto* impl = dynamic_cast<const VideoSurfaceImpl*>(&surface);
  return impl != nullptr && impl->chroma_shift(shift_x, shift_y);
}

}  // namespace detail
}  // namespace ovt::render
