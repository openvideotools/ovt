// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PictureHost (chapter 10 §10.1.2 EMBEDDED mode, §10.2.2; chapter 11 §11.3.2; WI-1.405): the QQuickRhiItem of the
// Picture panel. Its renderer, on Qt Quick's render thread, owns one libovt-render VideoSurface with an
// OffscreenTexture target of the item's pixel size, renders the current PictureFrame with the item's ViewState and
// draws the surface's output texture over the whole item with a passthrough textured quad. The interop follows the
// §10.2.2 table for the RHI backend in use:
//
//   Vulkan (Linux)  Interop::VulkanSharedDevice: libplacebo imports Qt's VkDevice; Qt wraps the VkImage.
//   Metal (macOS)   Interop::MetalTexture: the surface exports an id<MTLTexture>; Qt wraps it.
//   anything else   Interop::None: the surface renders on its own device and the renderer uploads a CPU copy (no
//                   zero-copy row exists for it in phase 1; Android's GlTexture host is WI-A.403).
//
// Video and overlays are never drawn with scene-graph primitives (FR-GUI-030). Requires Qt 6.7 (QQuickRhiItem); the
// build compiles this file only then (app/CMakeLists.txt, OVT_HAVE_PICTURE_HOST).

#ifndef OVT_APP_PICTURE_HOST_H
#define OVT_APP_PICTURE_HOST_H

#include <QtQml/qqmlregistration.h>
#include <QColor>
#include <QQuickRhiItem>
#include <QRect>
#include <QString>
#include <QVariantList>

#include <cstdint>
#include <memory>

#include "picture_frame.h"

namespace ovt::app {

// What the renderer last reported to the item (copied in QQuickRhiItemRenderer::synchronize, when the GUI thread
// is blocked).
struct PictureHostStats {
  QString interop;          // "vulkan-shared-device", "metal-texture", "cpu-copy" or "" before the first frame
  QString error;            // VideoSurface::last_error() or the renderer's own reason; "" when none
  int surface_resizes = 0;  // VideoSurface::resize() calls since the surface was created
  int frames_rendered = 0;  // render() calls that drew the surface's texture
  int surface_width = 0;    // the OffscreenTexture target size in device pixels
  int surface_height = 0;
};

class PictureHost : public QQuickRhiItem {
  Q_OBJECT
  QML_NAMED_ELEMENT(PictureHost)
  // ViewState (§10.2.3) from the panel. zoom = device pixels per luma sample; 0 (the default) fits the picture into
  // the item and centres it, ignoring panX/panY.
  Q_PROPERTY(double zoom READ zoom WRITE set_zoom NOTIFY viewChanged)
  Q_PROPERTY(double panX READ pan_x WRITE set_pan_x NOTIFY viewChanged)
  Q_PROPERTY(double panY READ pan_y WRITE set_pan_y NOTIFY viewChanged)
  Q_PROPERTY(int planeMode READ plane_mode WRITE set_plane_mode NOTIFY viewChanged)  // 0 YUV, 1 Y, 2 U, 3 V, 4 gray
  Q_PROPERTY(bool showCropped READ show_cropped WRITE set_show_cropped NOTIFY viewChanged)
  Q_PROPERTY(QColor backgroundColor READ background_color WRITE set_background_color NOTIFY viewChanged)
  Q_PROPERTY(bool hasPicture READ has_picture NOTIFY pictureChanged)
  Q_PROPERTY(int pictureWidth READ picture_width NOTIFY pictureChanged)
  Q_PROPERTY(int pictureHeight READ picture_height NOTIFY pictureChanged)
  // Picture::crop (conformance window / render_size, luma samples); a null rect when the picture has none.
  Q_PROPERTY(QRect crop READ crop NOTIFY pictureChanged)
  // Renderer state for the status strip and the tests.
  Q_PROPERTY(QString interop READ interop NOTIFY statsChanged)
  Q_PROPERTY(QString lastError READ last_error NOTIFY statsChanged)
  Q_PROPERTY(int surfaceResizes READ surface_resizes NOTIFY statsChanged)
  Q_PROPERTY(int framesRendered READ frames_rendered NOTIFY statsChanged)

 public:
  explicit PictureHost(QQuickItem* parent = nullptr);
  ~PictureHost() override;
  PictureHost(const PictureHost&) = delete;
  PictureHost& operator=(const PictureHost&) = delete;
  PictureHost(PictureHost&&) = delete;
  PictureHost& operator=(PictureHost&&) = delete;

  // The picture to show; nullptr clears it (the item then shows the background). GUI thread.
  void set_picture(std::shared_ptr<const PictureFrame> frame);
  [[nodiscard]] std::shared_ptr<const PictureFrame> picture() const { return frame_; }

  [[nodiscard]] double zoom() const noexcept { return zoom_; }
  void set_zoom(double zoom);
  [[nodiscard]] double pan_x() const noexcept { return pan_x_; }
  void set_pan_x(double x);
  [[nodiscard]] double pan_y() const noexcept { return pan_y_; }
  void set_pan_y(double y);
  [[nodiscard]] int plane_mode() const noexcept { return plane_mode_; }
  void set_plane_mode(int mode);
  [[nodiscard]] bool show_cropped() const noexcept { return show_cropped_; }
  void set_show_cropped(bool on);
  [[nodiscard]] QColor background_color() const { return background_; }
  void set_background_color(const QColor& color);

  [[nodiscard]] bool has_picture() const noexcept { return frame_ != nullptr; }
  [[nodiscard]] int picture_width() const noexcept;
  [[nodiscard]] int picture_height() const noexcept;
  [[nodiscard]] QRect crop() const;
  // The [Y, U, V] code values of the picture at luma sample (x, y) (chroma: the sample covering it), for the Picture
  // panel's read-out (WI-1.456); [] outside the picture or without one. Read from the CPU copy of the frame, so it
  // works before VideoSurface::pick() exists (WI-2.435).
  Q_INVOKABLE [[nodiscard]] QVariantList codeValues(int x, int y) const;

  [[nodiscard]] QString interop() const { return stats_.interop; }
  [[nodiscard]] QString last_error() const { return stats_.error; }
  [[nodiscard]] int surface_resizes() const noexcept { return stats_.surface_resizes; }
  [[nodiscard]] int frames_rendered() const noexcept { return stats_.frames_rendered; }
  [[nodiscard]] const PictureHostStats& stats() const noexcept { return stats_; }

  // Called by the renderer during synchronize() (GUI thread blocked); the change signal is queued to the GUI thread.
  void report_stats(const PictureHostStats& stats);

 Q_SIGNALS:
  void viewChanged();
  void pictureChanged();
  void statsChanged();

 protected:
  QQuickRhiItemRenderer* createRenderer() override;

 private:
  friend class PictureHostRenderer;

  std::shared_ptr<const PictureFrame> frame_;
  double zoom_ = 0.0;
  double pan_x_ = 0.0;
  double pan_y_ = 0.0;
  int plane_mode_ = 0;
  bool show_cropped_ = true;
  QColor background_{0x20, 0x20, 0x20};  // ViewState::background_rgba default 0x202020FF
  PictureHostStats stats_;
};

}  // namespace ovt::app

#endif  // OVT_APP_PICTURE_HOST_H
