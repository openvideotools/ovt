// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PictureHost and its renderer (WI-1.405); see picture_host.h for the design. The renderer lives on Qt Quick's render
// thread, which is also the VideoSurface's thread (§10.2.7 threading rule): every VideoSurface call happens in
// initialize(), render() or the destructor, all on that thread.

#include "picture_host.h"

#include <rhi/qrhi.h>
#include <rhi/qshader.h>
#include <QByteArray>
#include <QFile>
#include <QMetaObject>
#include <QPoint>
#include <QPointer>
#include <QQuickRhiItemRenderer>
#include <QQuickWindow>
#include <QRect>
#include <QSize>
#include <QString>
#include <QTimerEvent>
#include <QtGlobal>

#if QT_CONFIG(vulkan)
#include <QVulkanInstance>
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "overlay/overlay_set_impl.h"  // the complete OverlaySet: the host owns built sets until set_overlays()
#include "ovt/render/overlay_set.h"
#include "ovt/render/video_surface.h"
#include "picture_host_android.h"
#include "render_stub.h"
#include "surface/output_download.h"  // ovt::render::detail::download_output (exported, for Interop::None)

namespace ovt::app {

namespace {

using ovt::render::DisplayTarget;
using ovt::render::ExternalTexture;
using ovt::render::VideoSurface;
using ovt::render::ViewState;

// What the GUI thread hands over in synchronize().
struct HostState {
  std::shared_ptr<const PictureFrame> frame;
  double zoom = 0.0;
  double pan_x = 0.0;
  double pan_y = 0.0;
  int plane_mode = 0;
  bool show_cropped = true;
  std::uint32_t background_rgba = 0x202020FFU;
  int tone_map = 0;
  int gamut_map = 0;
  double clip_nits = 1000.0;
  double target_peak_override = 0.0;
  bool false_color = false;
  bool gamut_clip_overlay = false;
  bool dither = true;
  std::shared_ptr<const PictureFrame> frame_b;
  int compare_kind = 0;
  int split_axis = 0;
  double split_position = 0.5;
  double overlay_alpha = 0.5;
  int diff_gain = 1;
  bool diff_false_colour = false;
  int flicker_period_ms = 500;
  QPoint probe{-1, -1};
  QRect selection_rect;
};

// The CompareMode of §10.2.4 for the state (allow_scale_b stays false: a size mismatch is RND-0010).
ovt::render::CompareMode compare_for(const HostState& s) {
  using ovt::render::CompareMode;
  CompareMode m;
  m.kind = static_cast<CompareMode::Kind>(std::clamp(s.compare_kind, 0, static_cast<int>(CompareMode::Kind::Flicker)));
  m.split_axis = s.split_axis == 1 ? CompareMode::Axis::Horizontal : CompareMode::Axis::Vertical;
  m.split_position = static_cast<float>(s.split_position);
  m.overlay_alpha = static_cast<float>(s.overlay_alpha);
  m.diff_gain = static_cast<float>(s.diff_gain);
  m.diff_false_color = s.diff_false_colour;
  m.flicker_period_ms = static_cast<std::uint16_t>(std::clamp(s.flicker_period_ms, 1, 65535));
  return m;
}

std::uint32_t to_rgba(const QColor& c) {
  return (static_cast<std::uint32_t>(c.red()) << 24U) | (static_cast<std::uint32_t>(c.green()) << 16U) |
         (static_cast<std::uint32_t>(c.blue()) << 8U) | static_cast<std::uint32_t>(c.alpha());
}

// The ViewState of §10.2.3 for a viewport of w x h device pixels. zoom 0 = fit: the visible picture (the crop
// rectangle when shown cropped, else the coded picture) scaled to fit and centred.
ViewState view_for(const HostState& s, int w, int h) {
  ViewState v;
  v.plane_mode = s.plane_mode;
  v.show_cropped = s.show_cropped;
  v.background_rgba = s.background_rgba;
  v.tone_map =
      static_cast<ovt::render::ToneMap>(std::clamp(s.tone_map, 0, static_cast<int>(ovt::render::ToneMap::None)));
  v.gamut_map = static_cast<ovt::render::GamutMap>(
      std::clamp(s.gamut_map, 0, static_cast<int>(ovt::render::GamutMap::Highlight)));
  v.clip_nits = static_cast<float>(s.clip_nits);
  v.target_peak_override = static_cast<float>(s.target_peak_override);
  v.false_color = s.false_color;
  v.gamut_clip_overlay = s.gamut_clip_overlay;
  v.dither = s.dither;
  v.zoom = s.zoom;
  v.pan_x = s.pan_x;
  v.pan_y = s.pan_y;
  if (s.zoom <= 0.0) {
    double x0 = 0.0;
    double y0 = 0.0;
    double pw = 1.0;
    double ph = 1.0;
    if (s.frame != nullptr) {
      const ovt::render::Picture& pic = s.frame->picture;
      const ovt::fb::Rect& crop = pic.crop;
      if (s.show_cropped && crop.w() > 0 && crop.h() > 0) {
        x0 = crop.x();
        y0 = crop.y();
        pw = crop.w();
        ph = crop.h();
      } else {
        pw = pic.coded_width;
        ph = pic.coded_height;
      }
    }
    v.zoom = std::min(static_cast<double>(w) / pw, static_cast<double>(h) / ph);
    v.pan_x = x0 + (pw / 2.0);
    v.pan_y = y0 + (ph / 2.0);
  }
  return v;
}

QShader load_shader(const char* path) {
  QFile f(QString::fromLatin1(path));
  if (!f.open(QIODevice::ReadOnly)) {
    return {};
  }
  return QShader::fromSerialized(f.readAll());
}

}  // namespace

// ---- Renderer (render thread) ----------------------------------------------------------------------------------

class PictureHostRenderer : public QQuickRhiItemRenderer {
 public:
  PictureHostRenderer() = default;
  PictureHostRenderer(const PictureHostRenderer&) = delete;
  PictureHostRenderer& operator=(const PictureHostRenderer&) = delete;
  PictureHostRenderer(PictureHostRenderer&&) = delete;
  PictureHostRenderer& operator=(PictureHostRenderer&&) = delete;

  ~PictureHostRenderer() override {
    // Qt may still be sampling the surface's texture in a frame in flight; the texture dies with the surface.
    if (surface_ != nullptr && rhi_ != nullptr) {
      rhi_->finish();
    }
    release_qt_resources();
    {
      const SurfaceGlScope scope(gl_);  // the surface's GL objects live in the shared context (Android)
      surface_.reset();
    }
    gl_.reset();
  }

 protected:
  void initialize(QRhiCommandBuffer* /*cb*/) override {
    rhi_ = rhi();
    QRhiTexture* color = colorTexture();
    if (rhi_ == nullptr || color == nullptr) {
      return;
    }
    const QSize size = color->pixelSize();
    if (surface_ == nullptr) {
      if (!init_failed_) {
        create_surface(size);
      }
    } else if (size != surface_size_) {
      // §10.11 WI-1.405 step 4 / acceptance test 2: one VideoSurface::resize() per new size. The old texture is
      // destroyed by the resize while Qt may still sample it in a frame in flight, so wait for the GPU first.
      rhi_->finish();
      src_texture_ = nullptr;
      wrapped_.reset();
      cpu_texture_.reset();
      const SurfaceGlScope scope(gl_);
      if (surface_->resize(size.width(), size.height())) {
        surface_size_ = size;
        ++stats_.surface_resizes;
        stats_.surface_width = size.width();
        stats_.surface_height = size.height();
      } else {
        stats_.error = QString::fromUtf8(surface_->last_error());
      }
    }
    QRhiRenderPassDescriptor* rp = renderTarget() != nullptr ? renderTarget()->renderPassDescriptor() : nullptr;
    if (rp != nullptr && rp != pipeline_rp_) {
      create_pipeline(rp);
    }
  }

  void synchronize(QQuickRhiItem* item) override {
    auto* host = static_cast<PictureHost*>(item);
    state_.frame = host->frame_;
    state_.zoom = host->zoom_;
    state_.pan_x = host->pan_x_;
    state_.pan_y = host->pan_y_;
    state_.plane_mode = host->plane_mode_;
    state_.show_cropped = host->show_cropped_;
    state_.background_rgba = to_rgba(host->background_);
    state_.tone_map = host->tone_map_;
    state_.gamut_map = host->gamut_map_;
    state_.clip_nits = host->clip_nits_;
    state_.target_peak_override = host->target_peak_override_;
    state_.false_color = host->false_color_;
    state_.gamut_clip_overlay = host->gamut_clip_overlay_;
    state_.dither = host->dither_;
    state_.frame_b = host->frame_b_;
    state_.compare_kind = host->compare_kind_;
    state_.split_axis = host->split_axis_;
    state_.split_position = host->split_position_;
    state_.overlay_alpha = host->overlay_alpha_;
    state_.diff_gain = host->diff_gain_;
    state_.diff_false_colour = host->diff_false_colour_;
    state_.flicker_period_ms = host->flicker_period_ms_;
    state_.probe = host->probe_;
    state_.selection_rect = host->selection_rect_;
    if (host->window() != nullptr) {
      device_pixel_ratio_ = host->window()->effectiveDevicePixelRatio();
    }
    host->report_stats(stats_);
  }

  void render(QRhiCommandBuffer* cb) override {
    QRhiResourceUpdateBatch* updates = rhi_->nextResourceUpdateBatch();
    if (!vertices_uploaded_ && vbuf_ != nullptr) {
      updates->uploadStaticBuffer(vbuf_.get(), quad_.data());
      vertices_uploaded_ = true;
    }
    QRhiTexture* src = render_surface(updates);
    const std::uint32_t bg = state_.background_rgba;
    const QColor clear(static_cast<int>((bg >> 24U) & 0xFFU), static_cast<int>((bg >> 16U) & 0xFFU),
                       static_cast<int>((bg >> 8U) & 0xFFU), static_cast<int>(bg & 0xFFU));
    cb->beginPass(renderTarget(), clear, {1.0F, 0}, updates);
    if (src != nullptr && pipeline_ != nullptr && update_bindings(src)) {
      const QSize size = renderTarget()->pixelSize();
      cb->setGraphicsPipeline(pipeline_.get());
      cb->setViewport(QRhiViewport(0, 0, static_cast<float>(size.width()), static_cast<float>(size.height())));
      cb->setShaderResources(bindings_.get());
      const QRhiCommandBuffer::VertexInput input(vbuf_.get(), 0);
      cb->setVertexInput(0, 1, &input);
      cb->draw(4);
      ++stats_.frames_rendered;
    }
    cb->endPass();
  }

 private:
  void create_surface(QSize size) {
    if (render_stub_enabled()) {
      // OVT_RENDER_STUB=1 (chapter 14 §14.10.2, WI-1.465): no surface, so render() draws the background colour only.
      stats_.interop = QStringLiteral("stub");
      stats_.surface_width = size.width();
      stats_.surface_height = size.height();
      init_failed_ = true;
      return;
    }
    DisplayTarget target;
    target.kind = DisplayTarget::Kind::OffscreenTexture;
    target.width = size.width();
    target.height = size.height();
    target.offscreen_format = DisplayTarget::OffscreenFormat::Rgba8Srgb;  // the Qt Quick window is SDR (§10.1.2)
    target.device_pixel_ratio = static_cast<float>(device_pixel_ratio_);
    choose_interop(target);
    bool ok = false;
    {
      // Interop::GlTexture: libplacebo takes the GL context current at init() (§10.2.2 Android row).
      const SurfaceGlScope scope(gl_);
      surface_ = VideoSurface::create("studio");
      ok = surface_->init(target);
      if (!ok && target.interop != DisplayTarget::Interop::None) {
        stats_.error = QString::fromUtf8(surface_->last_error());
        surface_.reset();
      }
    }
    if (!ok && target.interop != DisplayTarget::Interop::None) {
      // The zero-copy row failed (for instance a Vulkan device without timelineSemaphore): copy through the CPU.
      gl_.reset();
      target.interop = DisplayTarget::Interop::None;
      target.interop_device = nullptr;
      surface_ = VideoSurface::create("studio");
      ok = surface_->init(target);
    }
    if (ok) {
      interop_ = target.interop;
      surface_size_ = size;
      stats_.interop = interop_name(interop_);
      stats_.surface_width = size.width();
      stats_.surface_height = size.height();
      return;
    }
    stats_.error = QString::fromUtf8(surface_->last_error());
    surface_.reset();
    init_failed_ = true;  // no retry every frame; the panel shows lastError
  }

  // §10.2.2 interop table, by the RHI backend in use.
  void choose_interop(DisplayTarget& target) {
    target.interop = DisplayTarget::Interop::None;
    switch (rhi_->backend()) {
#if QT_CONFIG(vulkan)
      case QRhi::Vulkan: {
        const auto* handles = static_cast<const QRhiVulkanNativeHandles*>(rhi_->nativeHandles());
        if (handles == nullptr || handles->inst == nullptr) {
          return;
        }
        vk_info_.instance = handles->inst->vkInstance();
        vk_info_.physical_device = handles->physDev;
        vk_info_.device = handles->dev;
        vk_info_.queue_family = handles->gfxQueueFamilyIdx;
        vk_info_.queue_index = handles->gfxQueueIdx;
        vk_info_.get_instance_proc_addr =
            reinterpret_cast<void*>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast): void* in the C++ API
                handles->inst->getInstanceProcAddr("vkGetInstanceProcAddr"));
        target.interop = DisplayTarget::Interop::VulkanSharedDevice;
        target.interop_device = &vk_info_;
        return;
      }
#endif
      case QRhi::Metal:
        target.interop = DisplayTarget::Interop::MetalTexture;
        return;
#if defined(__ANDROID__)
      case QRhi::OpenGLES2: {
        // Android (EMBEDDED): a context shared with Qt's, made here on the render thread (WI-A.403).
        QString reason;
        gl_ = AndroidGlInterop::create(rhi_, reason);
        if (gl_ == nullptr) {
          stats_.error = reason;
          return;
        }
        target.interop = DisplayTarget::Interop::GlTexture;
        return;
      }
#endif
      default:
        return;
    }
  }

  static QString interop_name(DisplayTarget::Interop interop) {
    switch (interop) {
      case DisplayTarget::Interop::VulkanSharedDevice:
        return QStringLiteral("vulkan-shared-device");
      case DisplayTarget::Interop::MetalTexture:
        return QStringLiteral("metal-texture");
      case DisplayTarget::Interop::GlTexture:
        return QStringLiteral("gl-texture");
      case DisplayTarget::Interop::None:
        break;
    }
    return QStringLiteral("cpu-copy");
  }

  // Slot B and the compare mode (§10.2.4), after set_frame() (the order SPEC-ASSUMPTION(#478) fixes). Kind::Single
  // clears B in the surface; the same B generation is not uploaded twice (§10.2.1).
  void apply_compare() {
    const bool want = state_.compare_kind != 0 && state_.frame_b != nullptr;
    const ovt::render::CompareMode mode = compare_for(state_);
    if (want) {
      const ovt::render::Picture& b = state_.frame_b->picture;
      const bool same = compare_active_ && b.generation == uploaded_b_generation_ && mode.kind == applied_.kind &&
                        mode.split_axis == applied_.split_axis && mode.split_position == applied_.split_position &&
                        mode.overlay_alpha == applied_.overlay_alpha && mode.diff_gain == applied_.diff_gain &&
                        mode.diff_false_color == applied_.diff_false_color &&
                        mode.flicker_period_ms == applied_.flicker_period_ms && !a_changed_;
      if (!same) {
        surface_->set_compare(b, mode);
        uploaded_b_generation_ = b.generation;
        applied_ = mode;
        compare_active_ = true;
      }
    } else if (compare_active_) {
      surface_->set_compare(state_.frame->picture, ovt::render::CompareMode{});
      compare_active_ = false;
    }
    a_changed_ = false;
  }

  // The probe crosshair and the selection rectangle are overlay passes of the surface (§10.5.6 pass 12, WI-2.436,
  // issue #650). No FrameAnalysis reaches the Picture panel yet, so the overlay set carries only these two draw-time
  // fields; it is rebuilt when one of them changes.
  void apply_marks() {
    if (marks_applied_ && state_.probe == applied_probe_ && state_.selection_rect == applied_rect_) {
      return;
    }
    ovt::render::OverlayConfig cfg;
    const QRect& r = state_.selection_rect;
    if (state_.probe.x() >= 0 && state_.probe.y() >= 0) {
      cfg.layers |= static_cast<std::uint32_t>(ovt::render::OverlayLayer::Probe);
      cfg.probe_x = state_.probe.x();
      cfg.probe_y = state_.probe.y();
    }
    if (r.width() > 0 && r.height() > 0) {
      cfg.layers |= static_cast<std::uint32_t>(ovt::render::OverlayLayer::SelectionRect);
      cfg.selection_rect = ovt::fb::Rect(r.x(), r.y(), r.width(), r.height());
    }
    if (cfg.layers != 0) {
      ovt::render::OverlayBuilder builder(*surface_);
      surface_->set_overlays(builder.build(nullptr, cfg));
    } else {
      surface_->set_overlays(nullptr);
    }
    applied_probe_ = state_.probe;
    applied_rect_ = r;
    marks_applied_ = true;
  }

  // Renders the current frame into the surface's texture and returns the QRhiTexture that shows it (nullptr when
  // there is no surface).
  QRhiTexture* render_surface(QRhiResourceUpdateBatch* updates) {
    if (surface_ == nullptr) {
      return nullptr;
    }
    ExternalTexture ext{};
    {
      const SurfaceGlScope scope(gl_);  // Android: the surface's calls on the shared context, Qt's restored after
      if (state_.frame != nullptr && state_.frame->picture.generation != uploaded_generation_) {
        surface_->set_frame(state_.frame->picture);  // uploads now; the frame outlives the call (§10.2.1)
        uploaded_generation_ = state_.frame->picture.generation;
        a_changed_ = true;
      }
      // No picture: background only, which the pass clears to (slot A may still hold an earlier frame).
      if (state_.frame == nullptr) {
        return nullptr;
      }
      apply_compare();
      apply_marks();
      surface_->render(view_for(state_, surface_size_.width(), surface_size_.height()));
      const std::string_view err = surface_->last_error();
      stats_.error = QString::fromUtf8(err.data(), static_cast<qsizetype>(err.size()));
      ext = surface_->output_texture();
    }
    if (ext.width <= 0 || ext.height <= 0) {
      return nullptr;
    }
    const QSize size(ext.width, ext.height);
    switch (interop_) {
      case DisplayTarget::Interop::GlTexture:
        // §10.2.2 fence rule: Qt's context waits for the surface's GLsync before it samples the texture.
        if (gl_ != nullptr) {
          gl_->wait(rhi_, ext.handle2);
        }
        [[fallthrough]];
      case DisplayTarget::Interop::VulkanSharedDevice:
      case DisplayTarget::Interop::MetalTexture: {
        // The handle is a VkImage, an id<MTLTexture> or a GL texture name; QRhiTexture::createFrom takes it as an
        // integer.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto object = reinterpret_cast<quintptr>(ext.handle);
        const int layout = static_cast<int>(ext.layout);
        if (wrapped_ == nullptr || wrapped_object_ != object || wrapped_->pixelSize() != size) {
          src_texture_ = nullptr;
          wrapped_.reset(rhi_->newTexture(QRhiTexture::RGBA8, size, 1));
          if (!wrapped_->createFrom({object, layout})) {
            stats_.error = QStringLiteral("QRhiTexture::createFrom failed for the surface's output texture");
            wrapped_.reset();
            return nullptr;
          }
          wrapped_object_ = object;
        } else {
          wrapped_->setNativeLayout(layout);  // where the surface left the image (§10.2.2 Linux row)
        }
        return wrapped_.get();
      }
      case DisplayTarget::Interop::None:
        break;
    }
    // Interop::None: copy through the CPU (rows top first, RGBA8 like the texture).
    ovt::render::detail::OutputPixels px;
    if (!ovt::render::detail::download_output(*surface_, px) || px.texel_bytes != 4) {
      return nullptr;
    }
    if (cpu_texture_ == nullptr || cpu_texture_->pixelSize() != size) {
      src_texture_ = nullptr;
      cpu_texture_.reset(rhi_->newTexture(QRhiTexture::RGBA8, size, 1));
      if (!cpu_texture_->create()) {
        cpu_texture_.reset();
        return nullptr;
      }
    }
    // QByteArray copies the RGBA8 bytes; it takes them as char.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    cpu_bytes_ = QByteArray(reinterpret_cast<const char*>(px.bytes.data()), static_cast<qsizetype>(px.bytes.size()));
    QRhiTextureSubresourceUploadDescription sub(cpu_bytes_);
    sub.setDataStride(static_cast<quint32>(px.width) * 4U);
    updates->uploadTexture(cpu_texture_.get(), QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, sub)));
    return cpu_texture_.get();
  }

  void create_pipeline(QRhiRenderPassDescriptor* rp) {
    bindings_.reset();
    pipeline_.reset();
    src_texture_ = nullptr;
    pipeline_rp_ = rp;
    if (sampler_ == nullptr) {
      // Nearest: the surface's texture has the item's pixel size, so every fragment samples one texel centre.
      sampler_.reset(rhi_->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                      QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
      if (!sampler_->create()) {
        sampler_.reset();
        return;
      }
    }
    if (vbuf_ == nullptr) {
      // A triangle strip covering the viewport; uv (0, 0) is the texture's first row, which is the top of the picture.
      // In normalised device coordinates the top is y = +1 where Y points up (OpenGL, Metal) and -1 on Vulkan.
      const float top = rhi_->isYUpInNDC() ? 1.0F : -1.0F;
      quad_ = {-1.0F, top, 0.0F, 0.0F, -1.0F, -top, 0.0F, 1.0F, 1.0F, top, 1.0F, 0.0F, 1.0F, -top, 1.0F, 1.0F};
      vbuf_.reset(rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                                  static_cast<quint32>(sizeof(float) * quad_.size())));
      if (!vbuf_->create()) {
        vbuf_.reset();
        return;
      }
      vertices_uploaded_ = false;
    }
    const QShader vs = load_shader(":/qt/qml/OvtStudio/shaders/picture_quad.vert.qsb");
    const QShader fs = load_shader(":/qt/qml/OvtStudio/shaders/picture_quad.frag.qsb");
    if (!vs.isValid() || !fs.isValid()) {
      stats_.error = QStringLiteral("the picture shaders (picture_quad.*.qsb) are missing from the resources");
      return;
    }
    // A layout-only binding set; the real one is made per source texture (update_bindings).
    bindings_.reset(rhi_->newShaderResourceBindings());
    bindings_->setBindings({QRhiShaderResourceBinding::sampledTexture(0, QRhiShaderResourceBinding::FragmentStage,
                                                                      nullptr, sampler_.get())});
    if (!bindings_->create()) {
      bindings_.reset();
      return;
    }
    pipeline_.reset(rhi_->newGraphicsPipeline());
    pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    pipeline_->setShaderStages({{QRhiShaderStage::Vertex, vs}, {QRhiShaderStage::Fragment, fs}});
    QRhiVertexInputLayout layout;
    layout.setBindings({{4 * sizeof(float)}});
    layout.setAttributes(
        {{0, 0, QRhiVertexInputAttribute::Float2, 0}, {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)}});
    pipeline_->setVertexInputLayout(layout);
    pipeline_->setShaderResourceBindings(bindings_.get());
    pipeline_->setRenderPassDescriptor(rp);
    if (!pipeline_->create()) {
      stats_.error = QStringLiteral("could not create the picture pipeline");
      pipeline_.reset();
    }
  }

  // Points the binding set at `src`; recreated only when the source texture object changes.
  bool update_bindings(QRhiTexture* src) {
    if (bindings_ == nullptr) {
      return false;
    }
    if (src == src_texture_) {
      return true;
    }
    bindings_->setBindings(
        {QRhiShaderResourceBinding::sampledTexture(0, QRhiShaderResourceBinding::FragmentStage, src, sampler_.get())});
    if (!bindings_->create()) {
      return false;
    }
    src_texture_ = src;
    return true;
  }

  void release_qt_resources() {
    pipeline_.reset();
    bindings_.reset();
    sampler_.reset();
    vbuf_.reset();
    wrapped_.reset();
    cpu_texture_.reset();
    src_texture_ = nullptr;
  }

  QRhi* rhi_ = nullptr;
  std::unique_ptr<VideoSurface> surface_;
  QSize surface_size_;
  DisplayTarget::Interop interop_ = DisplayTarget::Interop::None;
#if QT_CONFIG(vulkan)
  ovt::render::VulkanDeviceInfo vk_info_{};  // the device Qt shares with libplacebo (VulkanSharedDevice)
#endif
  std::unique_ptr<AndroidGlInterop> gl_;  // the context shared with Qt's (GlTexture, Android only)
  bool init_failed_ = false;
  qreal device_pixel_ratio_ = 1.0;
  std::uint64_t uploaded_generation_ = 0;
  std::uint64_t uploaded_b_generation_ = 0;
  ovt::render::CompareMode applied_;
  bool compare_active_ = false;
  bool a_changed_ = false;
  bool marks_applied_ = false;
  QPoint applied_probe_{-1, -1};
  QRect applied_rect_;
  HostState state_;
  PictureHostStats stats_;

  std::unique_ptr<QRhiTexture> wrapped_;  // the surface's VkImage / MTLTexture, not owned by Qt
  quint64 wrapped_object_ = 0;
  std::unique_ptr<QRhiTexture> cpu_texture_;
  QByteArray cpu_bytes_;
  QRhiTexture* src_texture_ = nullptr;  // what bindings_ samples
  std::unique_ptr<QRhiSampler> sampler_;
  std::unique_ptr<QRhiBuffer> vbuf_;
  std::array<float, 16> quad_{};
  bool vertices_uploaded_ = false;
  std::unique_ptr<QRhiShaderResourceBindings> bindings_;
  std::unique_ptr<QRhiGraphicsPipeline> pipeline_;
  QRhiRenderPassDescriptor* pipeline_rp_ = nullptr;
};

// ---- Item (GUI thread) -----------------------------------------------------------------------------------------

PictureHost::PictureHost(QQuickItem* parent) : QQuickRhiItem(parent) {}

PictureHost::~PictureHost() = default;

QQuickRhiItemRenderer* PictureHost::createRenderer() {
  return new PictureHostRenderer;
}

void PictureHost::set_picture(std::shared_ptr<const PictureFrame> frame) {
  if (frame == frame_) {
    return;
  }
  frame_ = std::move(frame);
  Q_EMIT pictureChanged();
  update();
}

namespace {

// A deep copy of `src` under a new generation: the planes are copied and the Picture's pointers re-aimed at them.
std::shared_ptr<const PictureFrame> clone_frame(const PictureFrame& src) {
  auto copy = std::make_shared<PictureFrame>();
  copy->picture = src.picture;
  copy->planes = src.planes;
  for (std::size_t i = 0; i < copy->picture.num_planes && i < copy->planes.size(); ++i) {
    copy->picture.planes[i].data = copy->planes.at(i).data();
  }
  copy->picture.generation = next_picture_generation();
  return copy;
}

}  // namespace

bool PictureHost::snapshotAsB() {
  if (frame_ == nullptr) {
    return false;
  }
  set_picture_b(clone_frame(*frame_));
  return true;
}

void PictureHost::set_picture_b(std::shared_ptr<const PictureFrame> frame) {
  if (frame == frame_b_) {
    return;
  }
  frame_b_ = std::move(frame);
  Q_EMIT compareChanged();
  update_flicker_timer();
  update();
}

void PictureHost::clearB() {
  set_picture_b(nullptr);
}

void PictureHost::swapAB() {
  if (frame_b_ == nullptr) {
    return;
  }
  std::swap(frame_, frame_b_);
  Q_EMIT pictureChanged();
  Q_EMIT compareChanged();
  update();
}

void PictureHost::set_compare(int& field, int value) {
  if (field == value) {
    return;
  }
  field = value;
  Q_EMIT compareChanged();
  update_flicker_timer();
  update();
}

void PictureHost::set_compare_kind(int kind) {
  set_compare(compare_kind_, std::clamp(kind, 0, 5));
}

void PictureHost::set_split_axis(int axis) {
  set_compare(split_axis_, std::clamp(axis, 0, 1));
}

void PictureHost::set_diff_gain(int gain) {
  set_compare(diff_gain_, std::clamp(gain, 1, 64));
}

void PictureHost::set_flicker_period_ms(int ms) {
  set_compare(flicker_period_ms_, std::clamp(ms, 1, 65535));
}

void PictureHost::set_split_position(double fraction) {
  const double v = std::clamp(fraction, 0.0, 1.0);
  if (qFuzzyCompare(v + 1.0, split_position_ + 1.0)) {
    return;
  }
  split_position_ = v;
  Q_EMIT compareChanged();
  update();
}

void PictureHost::set_overlay_alpha(double alpha) {
  const double v = std::clamp(alpha, 0.0, 1.0);
  if (qFuzzyCompare(v + 1.0, overlay_alpha_ + 1.0)) {
    return;
  }
  overlay_alpha_ = v;
  Q_EMIT compareChanged();
  update();
}

void PictureHost::set_diff_false_colour(bool on) {
  if (on == diff_false_colour_) {
    return;
  }
  diff_false_colour_ = on;
  Q_EMIT compareChanged();
  update();
}

// Flicker alternates A and B by the clock (§10.6.4), so the item repaints on a timer while it is on screen.
void PictureHost::update_flicker_timer() {
  const bool want = compare_kind_ == 5 && frame_b_ != nullptr;
  if (want && flicker_timer_ == 0) {
    flicker_timer_ = startTimer(16);
  } else if (!want && flicker_timer_ != 0) {
    killTimer(flicker_timer_);
    flicker_timer_ = 0;
  }
}

void PictureHost::timerEvent(QTimerEvent* event) {
  if (event->timerId() == flicker_timer_) {
    update();
    return;
  }
  QQuickRhiItem::timerEvent(event);
}

int PictureHost::picture_width() const noexcept {
  return frame_ != nullptr ? static_cast<int>(frame_->picture.coded_width) : 0;
}

int PictureHost::picture_height() const noexcept {
  return frame_ != nullptr ? static_cast<int>(frame_->picture.coded_height) : 0;
}

QRect PictureHost::crop() const {
  if (frame_ == nullptr) {
    return {};
  }
  const ovt::fb::Rect& c = frame_->picture.crop;
  if (c.w() <= 0 || c.h() <= 0) {
    return {};
  }
  return {c.x(), c.y(), c.w(), c.h()};  // ovt.fb.Rect fields are int
}

QVariantList PictureHost::codeValues(int x, int y) const {
  QVariantList codes;
  if (frame_ == nullptr || x < 0 || y < 0) {
    return codes;
  }
  const ovt::render::Picture& pic = frame_->picture;
  if (static_cast<std::uint32_t>(x) >= pic.coded_width || static_cast<std::uint32_t>(y) >= pic.coded_height) {
    return codes;
  }
  for (std::size_t i = 0; i < pic.num_planes && i < 3; ++i) {
    const ovt::render::PlaneData& p = pic.planes[i];
    if (p.data == nullptr || p.width == 0 || p.height == 0) {
      break;
    }
    // The plane sample covering luma (x, y): scale by the plane's size relative to the luma plane.
    const std::span<const std::uint8_t> bytes(static_cast<const std::uint8_t*>(p.data),
                                              static_cast<std::size_t>(p.stride_bytes) * p.height);
    const std::size_t px = (static_cast<std::size_t>(x) * p.width) / pic.coded_width;
    const std::size_t py = (static_cast<std::size_t>(y) * p.height) / pic.coded_height;
    const std::size_t at = (py * p.stride_bytes) + (px * p.sample_bytes);
    if (at + p.sample_bytes > bytes.size()) {
      break;
    }
    std::uint32_t v = bytes[at];
    if (p.sample_bytes == 2) {
      v |= static_cast<std::uint32_t>(bytes[at + 1]) << 8U;
    }
    codes.append(static_cast<int>(v));
  }
  return codes;
}

void PictureHost::set_zoom(double zoom) {
  if (qFuzzyCompare(zoom + 1.0, zoom_ + 1.0)) {
    return;
  }
  zoom_ = std::max(0.0, zoom);
  Q_EMIT viewChanged();
  update();
}

void PictureHost::set_pan_x(double x) {
  if (qFuzzyCompare(x + 1.0, pan_x_ + 1.0)) {
    return;
  }
  pan_x_ = x;
  Q_EMIT viewChanged();
  update();
}

void PictureHost::set_pan_y(double y) {
  if (qFuzzyCompare(y + 1.0, pan_y_ + 1.0)) {
    return;
  }
  pan_y_ = y;
  Q_EMIT viewChanged();
  update();
}

void PictureHost::set_plane_mode(int mode) {
  mode = std::clamp(mode, 0, 4);
  if (mode == plane_mode_) {
    return;
  }
  plane_mode_ = mode;
  Q_EMIT viewChanged();
  update();
}

void PictureHost::set_show_cropped(bool on) {
  if (on == show_cropped_) {
    return;
  }
  show_cropped_ = on;
  Q_EMIT viewChanged();
  update();
}

void PictureHost::set_background_color(const QColor& color) {
  if (color == background_) {
    return;
  }
  background_ = color;
  Q_EMIT viewChanged();
  update();
}

void PictureHost::report_stats(const PictureHostStats& stats) {
  const bool changed = stats.interop != stats_.interop || stats.error != stats_.error ||
                       stats.surface_resizes != stats_.surface_resizes ||
                       stats.frames_rendered != stats_.frames_rendered;
  stats_ = stats;
  if (changed) {
    // By name: the functor overload trips clang-analyzer's NewDeleteLeaks inside Qt's header.
    QMetaObject::invokeMethod(this, "statsChanged", Qt::QueuedConnection);
  }
}

}  // namespace ovt::app
