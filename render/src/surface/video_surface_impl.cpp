// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The one concrete VideoSurface (chapter 10 §10.1.3, §10.2.7). WI-1.400 is the skeleton: an OffscreenTexture target
// on libplacebo's Vulkan backend (MoltenVK on macOS, the Vulkan loader on Linux), cleared to
// ViewState.background_rgba by render(). Picture upload (WI-1.401), colour mapping (WI-1.402), the render pipeline
// (WI-1.403), interop (WI-1.404), native targets (WI-1.406 and later), overlays, measurements and export arrive with
// their own work items; until then those entry points are inert.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/vulkan.h>

#include "../overlay/overlay_set_impl.h"
#include "output_download.h"
#include "ovt/render/video_surface.h"

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

    struct pl_log_params log_params = {};
    log_params.log_cb = placebo_log_cb;
    log_params.log_priv = &prefix_;
    log_params.log_level = placebo_log_level();
    log_ = pl_log_create(PL_API_VER, &log_params);
    if (log_ == nullptr) {
      return fail("pl_log", "pl_log_create failed");
    }

    const struct pl_vk_inst_params inst_params = pl_vk_inst_default_params;
    inst_ = pl_vk_inst_create(log_, &inst_params);
    if (inst_ == nullptr) {
      return fail("pl_vk_inst", "no Vulkan instance (Vulkan loader or MoltenVK missing)");
    }

    struct pl_vulkan_params vk_params = pl_vulkan_default_params;
    vk_params.instance = inst_->instance;
    vk_params.get_proc_addr = inst_->get_proc_addr;
    // Software rasterizers (lavapipe on GPU-less CI hosts) are accepted; libplacebo still prefers a hardware device.
    vk_params.allow_software = true;
    vk_ = pl_vulkan_create(log_, &vk_params);
    if (vk_ == nullptr) {
      return fail("pl_gpu", "pl_vulkan_create found no usable Vulkan device");
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

  void set_frame(const Picture& /*a*/) override {}                                 // WI-1.401
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

  void destroy() {
    overlays_.reset();
    if (vk_ != nullptr) {
      pl_tex_destroy(vk_->gpu, &tex_);
    }
    pl_vulkan_destroy(&vk_);
    pl_vk_inst_destroy(&inst_);
    pl_log_destroy(&log_);
  }

  std::string prefix_;
  std::string error_;
  pl_log log_ = nullptr;
  pl_vk_inst inst_ = nullptr;
  pl_vulkan vk_ = nullptr;
  pl_tex tex_ = nullptr;
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

}  // namespace detail
}  // namespace ovt::render
