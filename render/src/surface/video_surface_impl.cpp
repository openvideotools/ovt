// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// The one concrete VideoSurface (chapter 10 §10.1.3, §10.2.7). WI-1.400 is the skeleton: an OffscreenTexture target
// on libplacebo's Vulkan backend (MoltenVK on macOS, the Vulkan loader on Linux), cleared to
// ViewState.background_rgba by render(). WI-1.401 adds picture upload (slot A, §10.3.1) and draws it with a basic
// pl_render_image pass, with the §10.3.3 colour tables of WI-1.402. The full render parameters, target frames and
// dithering (WI-1.403), the compare modes of slot B (WI-1.410), interop (WI-1.404, render/src/platform/offscreen.cpp),
// overlays, measurements
// (WI-4.452, render/src/measure/) and export arrive with their own work items; until then those entry points are
// inert. WI-1.406 adds NATIVE targets: a libplacebo swapchain on the surface of a platform NativeOutput
// (src/platform/native_target.h), retarget() EMBEDDED <-> NATIVE and the headroom poll of §10.4.1.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <libplacebo/gpu.h>
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>

#include "../overlay/overlay_layer.h"
#include "../overlay/overlay_set_impl.h"
#include "measure/measure.h"
#include "output_download.h"
#include "ovt/render/video_surface.h"
#include "pipeline/compare.h"
#include "pipeline/frame_upload.h"
#include "pipeline/render_params.h"
#include "platform/native_target.h"
#include "platform/offscreen.h"
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
    const bool native = target.kind != DisplayTarget::Kind::OffscreenTexture;
    const detail::NativeProvider& provider = detail::native_provider();
    if (native && provider.create == nullptr) {
      return fail("target", "no NATIVE target of this kind on this platform");
    }
    if (target.width <= 0 || target.height <= 0) {
      return fail(native ? "native target" : "offscreen target", "width and height must be positive");
    }
    std::unique_ptr<detail::NativeOutput> output;
    if (native) {
      std::string why;
      output = provider.create(target, why);
      if (output == nullptr) {
        return fail("target", why);
      }
    }

    struct pl_log_params log_params = {};
    log_params.log_cb = placebo_log_cb;
    log_params.log_priv = &prefix_;
    log_params.log_level = placebo_log_level();
    log_ = pl_log_create(PL_API_VER, &log_params);
    if (log_ == nullptr) {
      return fail("pl_log", "pl_log_create failed");
    }

    // Interop::None and MetalTexture render on the surface's own Vulkan device; VulkanSharedDevice and GlTexture on
    // the host's device or GL share group, which offscreen.cpp wraps (§10.2.2). A NATIVE target presents from the
    // own device; its OffscreenGpu (Interop::None on that device) serves the scope textures of the measurer.
    if (native || target.interop == DisplayTarget::Interop::None ||
        target.interop == DisplayTarget::Interop::MetalTexture) {
      if (!create_vulkan_device(output)) {
        return false;
      }
    }
    DisplayTarget gpu_target = target;
    if (native) {
      gpu_target.interop = DisplayTarget::Interop::None;
    }
    std::string stage;
    std::string reason;
    if (!offscreen_.create(log_, gpu_target, vk_, stage, reason)) {
      return fail(stage, reason);
    }
    gpu_ = offscreen_.gpu();
    renderer_ = pl_renderer_create(log_, gpu_);
    if (renderer_ == nullptr) {
      return fail("pl_renderer", "pl_renderer_create failed");
    }
    overlay_gpu_ = std::make_unique<detail::OverlayLayerGpu>(gpu_);  // WI-2.431
    compare_.init(log_, gpu_);                                       // WI-1.410

    // §10.7: measurement passes. A GPU without what they need still renders; measure() then never becomes valid.
    static_cast<void>(measurer_.init(log_, gpu_, &offscreen_));

    target_ = target;
    if (native_ != nullptr) {
      return open_native();
    }
    active_ = detail::target_encoding(target_);
    return create_texture(target.width, target.height);
  }

  // `output` (a NATIVE target's platform object) gets its VkSurfaceKHR here and moves to native_.
  bool create_vulkan_device(std::unique_ptr<detail::NativeOutput>& output) {
    // The platform's surface extensions are optional on every surface, so an OffscreenTexture surface can retarget()
    // to NATIVE later on the same instance.
    const detail::NativeProvider& provider = detail::native_provider();
    struct pl_vk_inst_params inst_params = pl_vk_inst_default_params;
    inst_params.opt_extensions = provider.instance_extensions.data();
    inst_params.num_opt_extensions = static_cast<int>(provider.instance_extensions.size());
    inst_ = pl_vk_inst_create(log_, &inst_params);
    if (inst_ == nullptr) {
      return fail("pl_vk_inst", "no Vulkan instance (Vulkan loader or MoltenVK missing)");
    }

    struct pl_vulkan_params vk_params = pl_vulkan_default_params;
    vk_params.instance = inst_->instance;
    // libplacebo loads its Vulkan functions through this wrapper, which hides timestamp support so libplacebo never
    // writes timer queries (MoltenVK resolves them after libplacebo may have freed the query pool;
    // vk_no_timestamps.cpp).
    vk_params.get_proc_addr = detail::no_timestamps_get_proc_addr(inst_->get_proc_addr);
    // Software rasterizers (lavapipe on GPU-less CI hosts) are accepted; libplacebo still prefers a hardware device.
    vk_params.allow_software = true;
    // Interop::MetalTexture needs VK_EXT_metal_objects, which libplacebo enables when the device offers it.
    // The device can present whenever the instance can (an OffscreenTexture surface may retarget() to NATIVE).
    static constexpr std::array<const char*, 1> kSwapchainExt = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    vk_params.opt_extensions = kSwapchainExt.data();
    vk_params.num_opt_extensions = static_cast<int>(kSwapchainExt.size());
    if (output != nullptr) {
      // libplacebo picks a queue family that can present to the surface (pl_vulkan_params.surface).
      std::string why;
      VkSurfaceKHR surf = output->create_surface(inst_->instance, inst_->get_proc_addr, why);
      if (surf == VK_NULL_HANDLE) {
        return fail("native target", why);
      }
      // Owned from here on: destroy() releases them if a later step fails.
      native_ = std::move(output);
      surf_ = surf;
      vk_params.surface = surf;
    }
    vk_ = pl_vulkan_create(log_, &vk_params);
    if (vk_ == nullptr) {
      return fail("pl_gpu", "pl_vulkan_create found no usable Vulkan device");
    }
    return true;
  }

  bool resize(int width, int height) override {
    if (gpu_ == nullptr) {
      return fail("resize", "surface is not initialised");
    }
    if (width <= 0 || height <= 0) {
      return fail("resize", "width and height must be positive");
    }
    if ((sw_ != nullptr || offscreen_.texture() != nullptr) && width == target_.width && height == target_.height) {
      return true;
    }
    target_.width = width;
    target_.height = height;
    if (sw_ != nullptr) {
      return resize_swapchain();
    }
    return create_texture(width, height);
  }

  // Switches between EMBEDDED (OffscreenTexture) and NATIVE targets, or updates the current one (size, format,
  // encoding, headroom), on the same pl_gpu: uploaded pictures and overlays are kept (§10.2.7). An OffscreenTexture
  // target keeps the interop of init(); a NATIVE target needs the surface's own Vulkan device.
  void retarget(const DisplayTarget& target) override {
    if (gpu_ == nullptr) {
      fail("retarget", "surface is not initialised");
      return;
    }
    if (target.width <= 0 || target.height <= 0) {
      fail("retarget", "width and height must be positive");
      return;
    }
    if (target.kind == DisplayTarget::Kind::OffscreenTexture) {
      if (target.interop != offscreen_.interop()) {
        fail("retarget", "an OffscreenTexture target keeps the interop of init()");
        return;
      }
      close_native();
      target_ = target;
      active_ = detail::target_encoding(target_);
      create_texture(target.width, target.height);
      return;
    }
    if (sw_ != nullptr && target.kind == target_.kind && target.native == target_.native &&
        target.native2 == target_.native2) {
      // Same native target: a new encoding re-picks the format pair, a new size resizes, a new headroom only changes
      // the target peak of the next render (libplacebo re-derives the tone curve; no swapchain recreation).
      const bool encoding_changed = target.encoding != target_.encoding || target.hdr_capable != target_.hdr_capable;
      const bool size_changed = target.width != target_.width || target.height != target_.height;
      target_ = target;
      if (encoding_changed) {
        open_native();  // a new swapchain: libplacebo reads the surface formats only when it creates one
      } else if (size_changed) {
        resize_swapchain();
      }
      return;
    }
    const detail::NativeProvider& provider = detail::native_provider();
    if (provider.create == nullptr) {
      fail("retarget", "no NATIVE target of this kind on this platform");
      return;
    }
    if (vk_ == nullptr || offscreen_.interop() != DisplayTarget::Interop::None) {
      fail("retarget", "a NATIVE target needs a surface initialised with Interop::None");
      return;
    }
    std::string why;
    std::unique_ptr<detail::NativeOutput> output = provider.create(target, why);
    if (output == nullptr) {
      fail("retarget", why);
      return;
    }
    VkSurfaceKHR surf = output->create_surface(vk_->instance, vk_->get_proc_addr, why);
    if (surf == VK_NULL_HANDLE) {
      fail("retarget", why);
      return;
    }
    // The offscreen output texture stays allocated until the next OffscreenTexture target re-creates it.
    close_native();
    native_ = std::move(output);
    surf_ = surf;
    target_ = target;
    open_native();
  }

  // SPEC-ASSUMPTION(#209): set_frame() is void (§10.2.7); a rejected picture leaves slot A empty and last_error()
  // reports RND-0001.
  void set_frame(const Picture& a) override {
    if (gpu_ == nullptr) {
      error_ = "RND-0001 picture rejected: surface is not initialised";
      return;
    }
    std::vector<std::string> notices;
    std::string err = detail::upload_picture(gpu_, a, slot_a_, notice_log_, notices);
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

  // Slot B and the mode (§10.2.4). SPEC-ASSUMPTION(#478): B is converted into A's grid here, so set_frame() comes
  // first when both change. RND-0010 / RND-0011 stay in last_error() while B is in that state.
  void set_compare(const Picture& b, const CompareMode& mode) override {
    if (gpu_ == nullptr) {
      error_ = "RND-0001 picture rejected: surface is not initialised";
      return;
    }
    const bool restart_flicker =
        mode.kind == CompareMode::Kind::Flicker &&
        (mode_.kind != CompareMode::Kind::Flicker || mode.flicker_period_ms != mode_.flicker_period_ms);
    mode_ = mode;
    psnr_pending_ = false;
    psnr_ = {-1.0F, -1.0F, -1.0F};
    if (mode.kind == CompareMode::Kind::Single) {
      detail::destroy_slot(gpu_, slot_b_);
      b_mismatch_ = false;
      b_converted_ = false;
      return;
    }
    if (restart_flicker) {
      flicker_start_ms_ = now_ms();
    }
    detail::PreparedB prep;
    std::string err = detail::prepare_b(b, detail::slot_format(slot_a_), mode.allow_scale_b, prep);
    if (err.empty()) {
      if (prep.converted || b_converted_) {
        slot_b_.valid = false;  // the same generation may now need another conversion
      }
      std::vector<std::string> notices;
      err = detail::upload_picture(gpu_, prep.pic, slot_b_, notice_log_, notices);
      log_lines(notices);
    }
    if (!err.empty()) {
      slot_b_.valid = false;
      error_ = std::move(err);
      log_lines({error_});
      return;
    }
    b_mismatch_ = prep.size_mismatch;
    b_converted_ = prep.converted;
    error_ = prep.message;
    if (!error_.empty()) {
      log_lines({error_});
    }
  }
  // The CPU arrays of the set are uploaded here (§10.2.5: "GPU upload happens inside set_overlays()").
  void set_overlays(std::unique_ptr<OverlaySet> overlays) override {
    overlays_ = std::move(overlays);
    if (overlay_gpu_ == nullptr) {
      return;
    }
    std::string err;
    if (!overlay_gpu_->upload(overlays_.get(), err)) {
      error_ = "RND-0002 renderer init failed: overlays: " + err;
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      (void)std::fprintf(stderr, "[%s] %s\n", prefix_.c_str(), error_.c_str());
    }
  }

  void render(const ViewState& view) override {
    if (native_ != nullptr) {
      render_native(view);
      return;
    }
    pl_tex tex = offscreen_.texture();
    if (tex == nullptr) {
      return;
    }
    offscreen_.begin_frame();  // the host hands the texture back (§10.2.2 sync rules)
    pl_tex_clear_ex(gpu_, tex, background(view));
    if (slot_a_.valid) {
      struct pl_frame target = {};
      target.num_planes = 1;
      target.planes[0].texture = tex;
      target.planes[0].components = 4;
      for (int c = 0; c < 4; ++c) {
        target.planes[0].component_mapping[c] = c;
      }
      target.color = detail::target_color(target_, active_);
      draw_view(view, target, true);
    }
    // §10.7.9: the measurement passes follow the display render in the same submission, only while measure() is
    // being called.
    if (measurer_.due()) {
      measurer_.run(slot_a_);
    }
    offscreen_.end_frame();  // submit and hand the texture to the host
  }

  [[nodiscard]] ExternalTexture output_texture() const override {
    return native_ != nullptr ? ExternalTexture{} : offscreen_.describe(target_);  // a NATIVE target presents itself
  }

  [[nodiscard]] Pick pick(int /*px*/, int /*py*/) const override { return Pick{}; }  // §10.5.7

  // §10.7.9: never blocks. SPEC-ASSUMPTION(#478): region_psnr (Difference) reduces the visible region of the last
  // render() here rather than in render(), so only a host that asks waits for the readback.
  Measurements measure() override {
    measurer_.request();
    Measurements m = measurer_.poll();
    if (psnr_pending_ && slot_a_.valid && slot_b_.valid) {
      if (!compare_.psnr(slot_a_, slot_b_, psnr_region_, psnr_)) {
        error_ = "RND-0002 renderer init failed: compare: region PSNR pass failed";
      }
      psnr_pending_ = false;
    }
    for (size_t i = 0; i < 3; ++i) {
      m.region_psnr[i] = psnr_.at(i);  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    }
    return m;
  }

  bool export_png(const char* path, ExportKind kind, const ViewState* /*view_or_null*/) override {
    switch (kind) {
      case ExportKind::ScopeWaveformPng:
        return export_scope(path, detail::ScopeKind::Waveform);
      case ExportKind::ScopeVectorscopePng:
        return export_scope(path, detail::ScopeKind::Vectorscope);
      case ExportKind::ScopeCiePng:
        return export_scope(path, detail::ScopeKind::CieXy);
      case ExportKind::HdrPng16:
      case ExportKind::SdrPng8:
      case ExportKind::ViewPng8:
        break;
    }
    error_ = "export_png is not implemented yet for this kind (chapter 10 §10.8, WI-1.412)";
    return false;
  }

  void set_scope_config(const detail::ScopeConfig& config) { measurer_.set_scope_config(config); }
  void hold_readbacks(bool hold) { measurer_.hold_readbacks(hold); }

  // The `kind` scope texture of the newest measurement, downloaded (blocking).
  bool download_scope(detail::ScopeKind kind, std::vector<uint16_t>& texels, int& width, int& height) {
    offscreen_.begin_frame();  // textures held by the host come back for the read ...
    const bool ok = read_scope(kind, texels, width, height);
    offscreen_.end_frame();  // ... and go back to it, as after render()
    return ok;
  }

  [[nodiscard]] DisplayTarget::OutputEncoding active_encoding() const override { return active_; }
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
    pl_tex tex = offscreen_.texture();
    if (native_ != nullptr || tex == nullptr || !tex->params.host_readable) {
      return false;
    }
    // A texture handed to the host comes back for the read and goes out again afterwards.
    const bool handed_out = offscreen_.begin_frame();
    const size_t texel = tex->params.format->texel_size;
    const auto w = static_cast<size_t>(target_.width);
    const auto h = static_cast<size_t>(target_.height);
    std::vector<uint8_t> bytes(w * h * texel);
    struct pl_tex_transfer_params tp = {};
    tp.tex = tex;
    tp.row_pitch = w * texel;
    tp.ptr = bytes.data();
    const bool ok = pl_tex_download(gpu_, &tp);
    pl_gpu_finish(gpu_);
    if (handed_out) {
      offscreen_.end_frame();
    }
    if (!ok) {
      return false;
    }
    out.width = target_.width;
    out.height = target_.height;
    out.texel_bytes = texel;
    out.bytes = std::move(bytes);
    return true;
  }

  void set_clock(std::function<int64_t()> clock) {
    clock_ = std::move(clock);
    flicker_start_ms_ = now_ms();
  }

 private:
  // background_rgba is 0xRRGGBBAA in the output encoding (straight alpha).
  static union pl_clear_color background(const ViewState& view) {
    union pl_clear_color color = {};
    const uint32_t bg = view.background_rgba;
    color.f[0] = static_cast<float>((bg >> 24U) & 0xFFU) / 255.0F;
    color.f[1] = static_cast<float>((bg >> 16U) & 0xFFU) / 255.0F;
    color.f[2] = static_cast<float>((bg >> 8U) & 0xFFU) / 255.0F;
    color.f[3] = static_cast<float>(bg & 0xFFU) / 255.0F;
    return color;
  }

  int64_t now_ms() const {
    if (clock_) {
      return clock_();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  void log_lines(const std::vector<std::string>& lines) const {
    for (const std::string& line : lines) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      (void)std::fprintf(stderr, "[%s] %s\n", prefix_.c_str(), line.c_str());
    }
  }

  // A frame over the whole of `tex` (the offscreen output or a compare intermediate) in the target's colour space.
  [[nodiscard]] struct pl_frame texture_frame(pl_tex tex) const {
    struct pl_frame frame = {};
    frame.num_planes = 1;
    frame.planes[0].texture = tex;
    frame.planes[0].components = 4;
    for (int c = 0; c < 4; ++c) {
      frame.planes[0].component_mapping[c] = c;
    }
    frame.color = detail::target_color(target_, active_);
    return frame;
  }

  // Draws picture A, or the §10.6.4 compare view of A and B, over the cleared `target` (whose planes and colour space
  // the caller set: the offscreen texture, or a NATIVE swapchain image).
  void draw_view(const ViewState& view, const struct pl_frame& target, bool offscreen) {
    if (mode_.kind == CompareMode::Kind::Single || !slot_b_.valid) {
      const detail::IRect full{0, 0, target_.width, target_.height};
      draw_slot(slot_a_, view, full, full, target, false, offscreen, true);
    } else {
      render_compare(view, target, offscreen);
    }
  }

  // Draws `slot` into `out` with the §10.2.3 mapping of the viewport rectangle `view_rect`, clipped to `clip` (the
  // scissor of the side-by-side and split views). `linear` renders to an FP16 intermediate in linear light (the
  // target's primaries and peak, 1.0 = 203 nits) for the overlay blend; otherwise to the target's own encoding with
  // the full parameters of §10.3.4/§10.3.5.
  void draw_slot(const detail::FrameSlot& slot, const ViewState& view, const detail::IRect& clip,
                 const detail::IRect& view_rect, struct pl_frame target, bool linear, bool offscreen,
                 bool with_overlays = false) {
    struct pl_frame image = slot.frame;
    pl_rect2df src = detail::source_rect(slot, view.show_cropped);
    pl_rect2df dst = detail::map_into(src, view, view_rect);
    const pl_rect2df unclipped = dst;  // the picture rectangle the overlay layer is laid out on
    // A scissor narrower than the target clips both rectangles; the whole target is left to libplacebo's own clipping.
    const bool whole = clip.x0 == 0 && clip.y0 == 0 && clip.x1 == target_.width && clip.y1 == target_.height;
    if (!whole && !detail::clip_rects(src, dst, clip)) {
      return;
    }
    image.crop = src;

    if (linear) {
      target.color.transfer = PL_COLOR_TRC_LINEAR;
    }
    target.crop = dst;
    params_.build(view, linear ? DisplayTarget::OutputEncoding::ScRgbLinear16F : active_, offscreen, image.color,
                  target);
    if (linear) {
      target.repr.bits = pl_bit_encoding{};  // a float texture holds normalised values; no integer depth scaling
    }
    // WI-2.431: the overlay layer, composited by libplacebo after the picture (§10.5, §10.5.6 step 1). It describes
    // slot A in the full viewport: drawn only where A is (Single, Flicker's A phase; SPEC-ASSUMPTION(#478)).
    struct pl_overlay overlay = {};
    std::string overlay_err;
    if (with_overlays && overlay_gpu_ != nullptr &&
        overlay_gpu_->draw(view, target_.width, target_.height, target_.device_pixel_ratio, unclipped, overlay,
                           overlay_err)) {
      target.overlays = &overlay;
      target.num_overlays = 1;
    } else if (!overlay_err.empty()) {
      error_ = "RND-0002 renderer init failed: overlays: " + overlay_err;
    }
    if (!pl_render_image(renderer_, &image, &target, &params_.params())) {
      error_ = "RND-0002 renderer init failed: render: pl_render_image failed";
    }
  }

  // The device-pixel bounding box of `slot` in the viewport (whole pixels, clipped to the target).
  [[nodiscard]] detail::IRect picture_box(const detail::FrameSlot& slot, const ViewState& view) const {
    const detail::IRect full{0, 0, target_.width, target_.height};
    const pl_rect2df d = detail::map_into(detail::source_rect(slot, view.show_cropped), view, full);
    return detail::intersect(full, detail::IRect{static_cast<int>(std::floor(d.x0)), static_cast<int>(std::floor(d.y0)),
                                                 static_cast<int>(std::ceil(d.x1)), static_cast<int>(std::ceil(d.y1))});
  }

  // §10.6.4 compare views of slots A and B.
  void render_compare(const ViewState& view, const struct pl_frame& out, bool offscreen) {
    pl_tex tex = out.planes[0].texture;
    const bool show_b = mode_.kind == CompareMode::Kind::Flicker &&
                        detail::flicker_shows_b(now_ms() - flicker_start_ms_, mode_.flicker_period_ms);
    const detail::CompareLayout layout = detail::compare_layout(mode_, target_.width, target_.height, show_b);
    const detail::IRect full{0, 0, target_.width, target_.height};
    const int enc = detail::shader_encoding(active_);
    bool ok = true;
    switch (mode_.kind) {
      case CompareMode::Kind::Difference: {
        detail::DiffParams p;
        p.gain = mode_.diff_gain;
        p.plane = mode_.diff_plane >= 0 ? static_cast<int>(static_cast<uint8_t>(mode_.diff_plane)) : -1;  // -1: max
        p.signed_view = mode_.diff_signed;
        p.false_color = mode_.diff_false_color;
        p.hatch = b_mismatch_;
        p.encoding = enc;
        const pl_rect2df src = detail::source_rect(slot_a_, view.show_cropped);
        ok = compare_.difference(tex, picture_box(slot_a_, view), slot_a_, slot_b_, src, view, full, p);
        psnr_pending_ = !b_mismatch_;
        psnr_region_ = detail::visible_samples(src, view, full);
        break;
      }
      case CompareMode::Kind::Overlay: {
        pl_tex ia = nullptr;
        pl_tex ib = nullptr;
        ok = compare_.intermediates(target_.width, target_.height, ia, ib);
        if (ok) {
          draw_slot(slot_a_, view, full, full, texture_frame(ia), true, offscreen);
          draw_slot(slot_b_, view, full, full, texture_frame(ib), true, offscreen);
          const detail::IRect a = picture_box(slot_a_, view);
          const detail::IRect b = picture_box(slot_b_, view);
          const detail::IRect box{std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1),
                                  std::max(a.y1, b.y1)};
          ok = compare_.blend(tex, box, mode_.overlay_alpha, enc);
        }
        break;
      }
      case CompareMode::Kind::Single:
      case CompareMode::Kind::SideBySide:
      case CompareMode::Kind::Split:
      case CompareMode::Kind::Flicker:
        if (layout.draw_a) {
          draw_slot(slot_a_, view, layout.clip_a, layout.view_a, out, false, offscreen,
                    mode_.kind == CompareMode::Kind::Flicker);
        }
        if (layout.draw_b) {
          draw_slot(slot_b_, view, layout.clip_b, layout.view_b, out, false, offscreen);
        }
        if (!layout.divider.empty()) {
          ok = compare_.fill_white(tex, layout.divider, enc);
        }
        break;
    }
    if (!ok) {
      error_ = "RND-0002 renderer init failed: compare: pass failed";
    }
  }

  // ExportKind::Scope*Png (§10.8, WI-4.454): measures picture A now (blocking, unlike measure()), then writes the scope
  // texture through the GUI-independent LUT of detail::scope_png_rgb().
  bool export_scope(const char* path, detail::ScopeKind kind) {
    if (!slot_a_.valid) {
      error_ = "export_png: no picture in slot A";
      return false;
    }
    offscreen_.begin_frame();  // textures held by the host come back for the passes
    const bool measured = measurer_.measure_now(slot_a_);
    std::vector<uint16_t> texels;
    int width = 0;
    int height = 0;
    const bool ok = measured && read_scope(kind, texels, width, height);
    offscreen_.end_frame();  // and go back to the host, as after render()
    if (!ok) {
      error_ = measured ? "export_png: the scope texture cannot be read back"
                        : "export_png: scope measurement failed: " + measurer_.error();
      return false;
    }
    const std::vector<uint8_t> rgb = detail::scope_png_rgb(texels, width, height);
    if (!detail::write_png_rgb8(path, rgb, width, height)) {
      error_ = std::string("export_png: cannot write ") + (path != nullptr ? path : "(null)");
      return false;
    }
    error_.clear();
    return true;
  }

  // Between offscreen_.begin_frame() and end_frame().
  bool read_scope(detail::ScopeKind kind, std::vector<uint16_t>& texels, int& width, int& height) {
    pl_tex tex = measurer_.scope_tex(kind);
    if (tex == nullptr || !tex->params.host_readable) {
      return false;
    }
    width = tex->params.w;
    height = tex->params.h;
    texels.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
    // Through an explicit host-readable buffer: libplacebo's own staging path for a pointer download
    // (pl_tex_download_pbo) crashes in vk_malloc_slice on MoltenVK when the device was imported (VulkanSharedDevice).
    struct pl_buf_params bp = {};
    bp.size = texels.size() * sizeof(uint16_t);
    bp.host_readable = true;
    pl_buf buf = pl_buf_create(gpu_, &bp);
    if (buf == nullptr) {
      return false;
    }
    struct pl_tex_transfer_params tp = {};
    tp.tex = tex;
    tp.row_pitch = static_cast<size_t>(width) * 4 * sizeof(uint16_t);
    tp.buf = buf;
    const bool ok = pl_tex_download(gpu_, &tp) && pl_buf_read(gpu_, buf, 0, texels.data(), bp.size);
    pl_buf_destroy(gpu_, &buf);
    return ok;
  }

  // One render of a NATIVE target: polls the headroom, acquires a swapchain image, draws and presents (§10.4.1).
  void render_native(const ViewState& view) {
    const float polled = native_->polled_headroom();
    if (active_ != DisplayTarget::OutputEncoding::Srgb8 && detail::headroom_changed(target_.headroom, polled)) {
      DisplayTarget updated = target_;
      updated.headroom = polled;
      retarget(updated);
    }
    struct pl_swapchain_frame frame = {};
    if (sw_ == nullptr || !pl_swapchain_start_frame(sw_, &frame)) {
      return;  // no swapchain, or no image now (window hidden or minimised); not an error
    }
    offscreen_.begin_frame();  // scope textures (§10.7.9)
    struct pl_frame target = {};
    pl_frame_from_swapchain(&target, &frame);
    target.color = detail::native_target_color(target_, active_);
    if (frame.fbo->params.blit_dst) {
      pl_tex_clear_ex(gpu_, frame.fbo, background(view));
    }
    if (slot_a_.valid) {
      draw_view(view, target, false);
    }
    if (measurer_.due()) {  // §10.7.9, as in render()
      measurer_.run(slot_a_);
    }
    const bool submitted = pl_swapchain_submit_frame(sw_);
    offscreen_.end_frame();
    if (!submitted) {
      error_ = "RND-0002 renderer init failed: render: pl_swapchain_submit_frame failed";
      return;
    }
    pl_swapchain_swap_buffers(sw_);
  }

  // (Re)creates the swapchain on surf_ for target_: picks the encoding from the surface's formats, lets libplacebo see
  // only that encoding's pairs, and configures the platform object before the swapchain's first present (§10.4.1).
  bool open_native() {
    if (sw_ != nullptr) {
      wait_device_idle();
      pl_swapchain_destroy(&sw_);
    }
    const detail::NativeChoice choice = detail::choose_native_encoding(target_, surface_formats());
    active_ = choice.encoding;
    if (choice.no_hdr_format) {
      error_ = "RND-0040 no HDR surface format; falling back to Srgb8";
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
      (void)std::fprintf(stderr, "[%s] %s\n", prefix_.c_str(), error_.c_str());
    }
    detail::set_surface_format_filter(surf_, choice.pairs);
    native_->configure(active_, target_.width, target_.height);
    struct pl_vulkan_swapchain_params sp = {};
    sp.surface = surf_;
    sp.present_mode = VK_PRESENT_MODE_FIFO_KHR;  // §10.4.1
    sp.disable_10bit_sdr = true;                 // Srgb8 is an 8-bit target (§10.3.2)
    sw_ = pl_vulkan_create_swapchain(vk_, &sp);
    if (sw_ == nullptr) {
      close_native();
      return fail("swapchain", "pl_vulkan_create_swapchain failed");
    }
    pl_swapchain_colorspace_hint(sw_, &choice.hint);
    return resize_swapchain();
  }

  // Not an error when it fails: libplacebo cannot size a swapchain on a 0x0 extent (hidden window, layer not laid out)
  // and creates it at the first pl_swapchain_start_frame() with an extent; render() draws nothing until then.
  bool resize_swapchain() {
    native_->configure(active_, target_.width, target_.height);
    int w = target_.width;
    int h = target_.height;
    static_cast<void>(pl_swapchain_resize(sw_, &w, &h));
    return true;
  }

  [[nodiscard]] std::vector<VkSurfaceFormatKHR> surface_formats() const {
    std::vector<VkSurfaceFormatKHR> formats;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Vulkan's generic PFN_vkVoidFunction
    const auto get_formats = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(  // unfiltered: the loader's
        inst_->get_proc_addr(inst_->instance, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
    uint32_t n = 0;
    if (get_formats == nullptr || get_formats(vk_->phys_device, surf_, &n, nullptr) != VK_SUCCESS) {
      return formats;
    }
    formats.resize(n);
    if (get_formats(vk_->phys_device, surf_, &n, formats.data()) != VK_SUCCESS) {
      formats.clear();
    }
    formats.resize(n);
    return formats;
  }

  void destroy_vk_surface(VkSurfaceKHR surf) const {
    if (surf == VK_NULL_HANDLE || inst_ == nullptr) {
      return;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Vulkan's generic PFN_vkVoidFunction
    const auto destroy_surface =
        reinterpret_cast<PFN_vkDestroySurfaceKHR>(inst_->get_proc_addr(inst_->instance, "vkDestroySurfaceKHR"));
    if (destroy_surface != nullptr) {
      destroy_surface(inst_->instance, surf, nullptr);
    }
  }

  // Destroys the swapchain, its surface and the platform object (the device stays).
  void close_native() {
    if (sw_ != nullptr) {
      wait_device_idle();
      pl_swapchain_destroy(&sw_);
    }
    detail::set_surface_format_filter(surf_, {});
    destroy_vk_surface(surf_);
    surf_ = VK_NULL_HANDLE;
    native_.reset();
  }

  bool create_texture(int /*width*/, int /*height*/) {
    std::string reason;
    if (!offscreen_.create_texture(target_, reason)) {
      return fail("offscreen target", reason);
    }
    return true;
  }

  bool fail(const std::string& stage, const std::string& reason) {
    error_ = "RND-0002 renderer init failed: " + stage + ": " + reason;  // §10.10
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    (void)std::fprintf(stderr, "[%s] %s\n", prefix_.c_str(), error_.c_str());
    return false;
  }

  // Waits until the driver has finished with every submission, including its own completion callbacks, before
  // pl_vulkan_destroy() frees the VkDevice.
  //
  // pl_vulkan_destroy() only waits on libplacebo's timeline semaphores. On MoltenVK those are MTLEvents that the GPU
  // signals from inside the command buffer, before Metal runs the command buffer's completed handlers on its
  // completion thread. MoltenVK registers several such handlers per submission (MVKQueueCommandBufferSubmission::
  // commitActiveMTLCommandBuffer records performance stats through the MVKDevice and then calls finish(); command
  // encoders return temporary MTLBuffer allocations to device-owned pools), and they dereference the MVKDevice
  // without holding a reference to it. A surface destroyed right after a download could free the device under a
  // handler that was still running: the intermittent SIGSEGV in teardown (#304, and again after it).
  //
  // vkDeviceWaitIdle alone does not close that window on MoltenVK 1.2.x: MVKQueue::waitIdle commits an empty command
  // buffer and returns from -waitUntilCompleted, which wakes when the buffer's status becomes completed, not when the
  // completion thread has run the earlier buffers' handlers. A fence does: a fence-only vkQueueSubmit becomes an
  // MVKQueueCommandBufferSubmission whose fence is signalled by finish(), from inside that buffer's own completed
  // handler. Metal runs a queue's completed handlers one after another in commit order, so once that fence is
  // signalled every handler of every earlier submission on the queue has returned. What the signalling handler still
  // does afterwards only releases objects it retained itself (the submission's fence and queue), never the device.
  //
  // On other drivers (lavapipe, desktop GPUs) this is a plain full wait: an empty submission with a fence is
  // signalled once all work previously submitted to the queue has completed (Vulkan 1.0 vkQueueSubmit).
  //
  // Every queue is locked through libplacebo for the whole wait: vkDeviceWaitIdle and vkQueueSubmit need external
  // synchronisation of the queues, and nothing may submit between the fence and the destruction.
  void wait_device_idle() {
    if (vk_ == nullptr || vk_->device == VK_NULL_HANDLE || vk_->get_proc_addr == nullptr) {
      return;
    }
    pl_gpu_finish(vk_->gpu);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): Vulkan's generic PFN_vkVoidFunction
    const auto get_device_proc =
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(vk_->get_proc_addr(vk_->instance, "vkGetDeviceProcAddr"));
    if (get_device_proc == nullptr) {
      return;
    }
    VkDevice dev = vk_->device;
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): Vulkan's generic PFN_vkVoidFunction
    const auto device_wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(get_device_proc(dev, "vkDeviceWaitIdle"));
    const auto get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(get_device_proc(dev, "vkGetDeviceQueue"));
    const auto queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(get_device_proc(dev, "vkQueueSubmit"));
    const auto create_fence = reinterpret_cast<PFN_vkCreateFence>(get_device_proc(dev, "vkCreateFence"));
    const auto wait_fences = reinterpret_cast<PFN_vkWaitForFences>(get_device_proc(dev, "vkWaitForFences"));
    const auto destroy_fence = reinterpret_cast<PFN_vkDestroyFence>(get_device_proc(dev, "vkDestroyFence"));
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    if (device_wait_idle == nullptr || get_queue == nullptr || queue_submit == nullptr || create_fence == nullptr ||
        wait_fences == nullptr || destroy_fence == nullptr) {
      return;
    }
    // Every queue is locked while the device drains, then a fence submitted behind each queue's work is waited for.
    // Nothing here allocates: ~VideoSurfaceImpl reaches this through destroy() and must not throw.
    for_each_queue([this](uint32_t qf, uint32_t qi) { vk_->lock_queue(vk_, qf, qi); });
    static_cast<void>(device_wait_idle(dev));
    for_each_queue([&](uint32_t qf, uint32_t qi) {
      VkQueue queue = VK_NULL_HANDLE;
      get_queue(dev, qf, qi, &queue);
      VkFenceCreateInfo info = {};
      info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      VkFence fence = VK_NULL_HANDLE;
      if (queue == VK_NULL_HANDLE || create_fence(dev, &info, nullptr, &fence) != VK_SUCCESS) {
        return;
      }
      // submitCount 0 with a fence: the fence alone goes into the queue, behind everything submitted before it.
      if (queue_submit(queue, 0, nullptr, fence) == VK_SUCCESS) {
        static_cast<void>(wait_fences(dev, 1, &fence, VK_TRUE, UINT64_MAX));
      }
      destroy_fence(dev, fence, nullptr);
    });
    for_each_queue([this](uint32_t qf, uint32_t qi) { vk_->unlock_queue(vk_, qf, qi); });
  }

  // Calls fn(family, index) once for every queue of the graphics, compute and transfer families, which may coincide.
  template <typename Fn>
  void for_each_queue(const Fn& fn) const {
    const pl_vulkan_queue& graphics = vk_->queue_graphics;
    const pl_vulkan_queue& compute = vk_->queue_compute;
    const pl_vulkan_queue& transfer = vk_->queue_transfer;
    const auto in = [](const pl_vulkan_queue& q, uint32_t family, uint32_t index) {
      return q.index == family && index < q.count;
    };
    for (uint32_t i = 0; i < graphics.count; ++i) {
      fn(graphics.index, i);
    }
    for (uint32_t i = 0; i < compute.count; ++i) {
      if (!in(graphics, compute.index, i)) {
        fn(compute.index, i);
      }
    }
    for (uint32_t i = 0; i < transfer.count; ++i) {
      if (!in(graphics, transfer.index, i) && !in(compute, transfer.index, i)) {
        fn(transfer.index, i);
      }
    }
  }

  void destroy() {
    overlays_.reset();
    overlay_gpu_.reset();
    measurer_.destroy();
    if (gpu_ != nullptr) {
      detail::destroy_slot(gpu_, slot_a_);
      detail::destroy_slot(gpu_, slot_b_);
    }
    compare_.destroy();
    pl_renderer_destroy(&renderer_);
    close_native();
    offscreen_.destroy();  // the output texture, and the host's device or GL context wrapper
    gpu_ = nullptr;
    wait_device_idle();
    pl_vulkan_destroy(&vk_);
    pl_vk_inst_destroy(&inst_);
    pl_log_destroy(&log_);
  }

  std::string prefix_;
  std::string error_;
  pl_log log_ = nullptr;
  pl_vk_inst inst_ = nullptr;
  pl_vulkan vk_ = nullptr;  // own Vulkan device (Interop::None, MetalTexture)
  // The output texture and the §10.2.2 interop. mutable: the const CPU download takes a texture held by the host back
  // for the read (begin_frame/end_frame).
  mutable detail::OffscreenGpu offscreen_;
  pl_gpu gpu_ = nullptr;  // offscreen_.gpu()
  pl_renderer renderer_ = nullptr;
  detail::FrameSlot slot_a_;
  detail::FrameSlot slot_b_;  // compare picture, converted into A's grid (WI-1.410)
  CompareMode mode_;
  bool b_mismatch_ = false;   // RND-0010: B keeps its own size
  bool b_converted_ = false;  // slot B holds a converted copy
  detail::ComparePasses compare_;
  std::function<int64_t()> clock_;  // ms; steady_clock unless a test sets one (the host drives redraws, §10.6.4)
  int64_t flicker_start_ms_ = 0;
  bool psnr_pending_ = false;
  detail::IRect psnr_region_;
  std::array<float, 3> psnr_ = {-1.0F, -1.0F, -1.0F};
  detail::RenderParamSet params_;     // rebuilt for every render (§10.3.4)
  detail::H273NoticeLog notice_log_;  // RND-0020/RND-0021 once per colour description (§10.3.3)
  DisplayTarget target_{};
  DisplayTarget::OutputEncoding active_ = DisplayTarget::OutputEncoding::Srgb8;
  std::unique_ptr<OverlaySet> overlays_;
  std::unique_ptr<detail::OverlayLayerGpu> overlay_gpu_;  // GPU side of overlays_ (WI-2.431)
  detail::Measurer measurer_;                             // §10.7 (WI-4.452)
  // NATIVE targets (WI-1.406): the platform object, its surface and libplacebo's swapchain on it.
  std::unique_ptr<detail::NativeOutput> native_;
  VkSurfaceKHR surf_ = VK_NULL_HANDLE;
  pl_swapchain sw_ = nullptr;
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

void set_compare_clock(VideoSurface& surface, std::function<int64_t()> clock_ms) {
  auto* impl = dynamic_cast<VideoSurfaceImpl*>(&surface);
  if (impl != nullptr) {
    impl->set_clock(std::move(clock_ms));
  }
}

bool set_scope_config(VideoSurface& surface, const ScopeConfig& config) {
  auto* impl = dynamic_cast<VideoSurfaceImpl*>(&surface);
  if (impl == nullptr) {
    return false;
  }
  impl->set_scope_config(config);
  return true;
}

bool hold_readbacks(VideoSurface& surface, bool hold) {
  auto* impl = dynamic_cast<VideoSurfaceImpl*>(&surface);
  if (impl == nullptr) {
    return false;
  }
  impl->hold_readbacks(hold);
  return true;
}

bool download_scope(VideoSurface& surface, ScopeKind kind, std::vector<uint16_t>& rgba16hf, int& width, int& height) {
  auto* impl = dynamic_cast<VideoSurfaceImpl*>(&surface);
  return impl != nullptr && impl->download_scope(kind, rgba16hf, width, height);
}

bool slot_a_chroma_shift(const VideoSurface& surface, float& shift_x, float& shift_y) {
  const auto* impl = dynamic_cast<const VideoSurfaceImpl*>(&surface);
  return impl != nullptr && impl->chroma_shift(shift_x, shift_y);
}

}  // namespace detail
}  // namespace ovt::render
