// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// PictureController (chapter 11 §11.3.2, §11.4.4; WI-1.456): the input side of the Picture panel. It owns the
// panel's ViewState (§10.2.3: zoom, pan, plane mode, show_cropped) and turns the panel's mouse and key input into
// ViewState changes and Selection changes (probe, selection rectangle). It never renders and never calls the core:
// the PictureHost item binds to its ViewState and draws (FR-GUI-030), and the decoder path (WI-2.171) listens to
// frameWanted().
//
// Coordinates. "Device" coordinates are the host item's pixels (item coordinates × devicePixelRatio), with (0, 0)
// at the top-left of the viewport; "image" coordinates are luma samples of the coded picture. The mapping is the one
// libovt-render uses (render/src/pipeline/render_params.cpp): device = (image − pan) × zoom + viewport / 2.
//
// The class needs neither a renderer nor a picture, so it is built on every platform and tested without a GPU.

#ifndef OVT_APP_PICTURE_CONTROLLER_H
#define OVT_APP_PICTURE_CONTROLLER_H

#include <QtQml/qqmlregistration.h>
#include <QJSValue>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QString>

#include <optional>

#include "models/selection_model.h"

namespace ovt::app {

class PictureController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  // The Selection singleton (SelectionModel); the probe and the selection rectangle are written there.
  Q_PROPERTY(ovt::app::SelectionModel* selection READ selection WRITE set_selection NOTIFY selectionChanged)
  // The panel id used as the Selection origin (echo suppression, §11.4.1).
  Q_PROPERTY(QString origin READ origin WRITE set_origin NOTIFY selectionChanged)
  // The viewport in device pixels.
  Q_PROPERTY(double viewportWidth READ viewport_width WRITE set_viewport_width NOTIFY viewChanged)
  Q_PROPERTY(double viewportHeight READ viewport_height WRITE set_viewport_height NOTIFY viewChanged)
  // The picture being shown: coded size and conformance window / render_size (FrameInfo.crop; 0 x 0 = none).
  Q_PROPERTY(int pictureWidth READ picture_width NOTIFY pictureChanged)
  Q_PROPERTY(int pictureHeight READ picture_height NOTIFY pictureChanged)
  Q_PROPERTY(QRect crop READ crop NOTIFY pictureChanged)
  Q_PROPERTY(bool hasPicture READ has_picture NOTIFY pictureChanged)
  // ViewState. `zoom` is always the effective value (device pixels per luma sample), also while `fit` is true.
  Q_PROPERTY(bool fit READ fit NOTIFY viewChanged)
  Q_PROPERTY(double zoom READ zoom NOTIFY viewChanged)
  Q_PROPERTY(double panX READ pan_x NOTIFY viewChanged)
  Q_PROPERTY(double panY READ pan_y NOTIFY viewChanged)
  Q_PROPERTY(int planeMode READ plane_mode WRITE set_plane_mode NOTIFY planeChanged)  // 0 YUV, 1 Y, 2 U, 3 V, 4 Gray
  Q_PROPERTY(bool showCropped READ show_cropped WRITE set_show_cropped NOTIFY viewChanged)
  // PlaneStage of the Stage combo (§11.3.2): 0 Reconstruction (default), 1 Prediction, 2 Residual, 3 PreDeblock,
  // 4 PreSaoOrCdef, 5 PreLoopRestoration. The combo is shown only while `stageAvailable` (a FrameAnalysis with
  // planes exists for the frame).
  Q_PROPERTY(int stage READ stage WRITE set_stage NOTIFY planeChanged)
  Q_PROPERTY(bool stageAvailable READ stage_available WRITE set_stage_available NOTIFY planeChanged)
  // The image sample under the pointer, (-1, -1) when the pointer is not over the picture.
  Q_PROPERTY(QPoint hover READ hover NOTIFY hoverChanged)
  // The pinned probe and the committed selection rectangle (both from Selection; (-1, -1) / empty = none).
  Q_PROPERTY(QPoint probe READ probe NOTIFY probeChanged)
  Q_PROPERTY(QRect selectionRect READ selection_rect NOTIFY rectChanged)
  // The rectangle being drawn by Shift+drag (luma samples, snapped to 4 x 4 cells); empty when none.
  Q_PROPERTY(QRect dragRect READ drag_rect NOTIFY rectChanged)
  Q_PROPERTY(bool panning READ panning NOTIFY dragChanged)
  // function(x, y) returning the [Y, U, V] code values at a luma sample ([] when unknown), for the read-out.
  Q_PROPERTY(QJSValue sampleReader READ sample_reader WRITE set_sample_reader NOTIFY hoverChanged)
  // The status-strip read-out (§11.3.2 Pixel probe): position, code values, zoom and plane.
  Q_PROPERTY(QString readout READ readout NOTIFY readoutChanged)

 public:
  static constexpr double kMinZoom = 1.0 / 32.0;  // §11.3.2: powers of two 1/32 … 64
  static constexpr double kMaxZoom = 64.0;
  static constexpr double kNotch = 120.0;      // QWheelEvent::angleDelta() of one wheel notch (§11.4.4)
  static constexpr double kMinVisible = 64.0;  // device pixels of picture that panning keeps visible
  static constexpr int kCell = 4;              // selection rectangles snap to 4 x 4 cells (Pick.cell_x/y)

  explicit PictureController(QObject* parent = nullptr);
  ~PictureController() override;
  PictureController(const PictureController&) = delete;
  PictureController& operator=(const PictureController&) = delete;
  PictureController(PictureController&&) = delete;
  PictureController& operator=(PictureController&&) = delete;

  // ---- Picture --------------------------------------------------------------------------------------------------
  // The coded size of the picture shown and its crop rectangle (a null rect = no crop). 0 x 0 clears the picture.
  // A new size refits the view; the same size (the next frame) keeps zoom and pan.
  Q_INVOKABLE void setPicture(int coded_width, int coded_height, const QRect& crop = QRect());

  // ---- Zoom and pan (§11.3.2 Zoom/pan rules) ----------------------------------------------------------------------
  // One wheel event at device (px, py). Notches multiply the zoom by 2^(delta / 120) about the cursor, so a notch is
  // x2 and smooth-scrolling deltas accumulate to the same result.
  Q_INVOKABLE void wheel(double px, double py, double angle_delta_y);
  // Zooms to `zoom` keeping the image point under device (px, py) fixed: pan' = ix − (px − w/2) / zoom'.
  Q_INVOKABLE void zoomAbout(double zoom, double px, double py);
  // `+` / `−`: the next power of two above / below the current zoom, about the viewport centre.
  Q_INVOKABLE void zoomIn();
  Q_INVOKABLE void zoomOut();
  // Ctrl+1/2/4: an exact zoom about the viewport centre.
  Q_INVOKABLE void zoomTo(double zoom);
  // `R`, double-click: fit the visible picture into the viewport and centre it.
  Q_INVOKABLE void fitToView();
  // Pans by a pointer movement of (dx, dy) device pixels (the picture follows the pointer).
  Q_INVOKABLE void panBy(double dx, double dy);
  Q_INVOKABLE void beginPan(double px, double py);
  Q_INVOKABLE void movePan(double px, double py);
  Q_INVOKABLE void endPan();

  // ---- Mapping ------------------------------------------------------------------------------------------------------
  // The image point (luma samples, fractional) at device (px, py).
  Q_INVOKABLE [[nodiscard]] QPointF imagePoint(double px, double py) const;
  // The luma sample at device (px, py), or (-1, -1) outside the picture (Pick.image_x/y and Pick.hit).
  Q_INVOKABLE [[nodiscard]] QPoint pick(double px, double py) const;
  // The device position of image point (ix, iy).
  Q_INVOKABLE [[nodiscard]] QPointF devicePoint(double ix, double iy) const;

  // ---- Planes -------------------------------------------------------------------------------------------------------
  [[nodiscard]] int plane_mode() const noexcept { return plane_mode_; }
  void set_plane_mode(int mode);
  [[nodiscard]] int stage() const noexcept { return stage_; }
  void set_stage(int stage);
  [[nodiscard]] bool stage_available() const noexcept { return stage_available_; }
  void set_stage_available(bool available);

  // ---- Probe and selection rectangle (§11.3.2 Pixel probe, Selection rectangle) -------------------------------------
  Q_INVOKABLE void hoverAt(double px, double py);
  Q_INVOKABLE void leave();
  // Ctrl+click: pins the probe at the sample under device (px, py); on the pinned sample it unpins.
  Q_INVOKABLE void toggleProbe(double px, double py);
  // Moves a pinned probe by whole samples (picture.probeLeft/Right/Up/Down), clamped to the picture.
  Q_INVOKABLE void moveProbe(int dx, int dy);
  Q_INVOKABLE void beginRect(double px, double py);
  Q_INVOKABLE void moveRect(double px, double py);
  // Commits the rectangle being drawn to Selection (an empty one clears the selection rectangle).
  Q_INVOKABLE void endRect();
  // Esc: cancels a rectangle being drawn; otherwise unpins the probe and clears the selection rectangle. Returns
  // true when there was something to clear.
  Q_INVOKABLE bool escape();

  // ---- Saved state (§11.5.7, WI-A.455) ----------------------------------------------------------------------------
  // The ViewState Android saves across Activity recreation (app/src/platform/android_state.cpp).
  struct ViewSnapshot {
    bool fit = true;
    double zoom = 1.0;
    double pan_x = 0;
    double pan_y = 0;
    int plane_mode = 0;
    int stage = 0;
    bool show_cropped = true;
  };
  [[nodiscard]] ViewSnapshot viewSnapshot() const;
  // Puts a saved ViewState back. Zoom and pan need the picture (pan is clamped to it): with no picture yet they wait
  // for the next setPicture(), which then applies them instead of fitting.
  void restoreView(const ViewSnapshot& view);

  // ---- Actions ------------------------------------------------------------------------------------------------------
  // The window-scope actions of the Keymap that act on the Picture (view.plane.*, view.zoom.*), given without the
  // "action." prefix. Returns true when the id is one of them.
  Q_INVOKABLE bool handleAction(const QString& id);

  // ---- Properties ---------------------------------------------------------------------------------------------------
  [[nodiscard]] SelectionModel* selection() const { return selection_.data(); }
  void set_selection(SelectionModel* selection);
  [[nodiscard]] QString origin() const { return origin_; }
  void set_origin(const QString& origin);
  [[nodiscard]] double viewport_width() const noexcept { return vw_; }
  void set_viewport_width(double w);
  [[nodiscard]] double viewport_height() const noexcept { return vh_; }
  void set_viewport_height(double h);
  [[nodiscard]] int picture_width() const noexcept { return coded_w_; }
  [[nodiscard]] int picture_height() const noexcept { return coded_h_; }
  [[nodiscard]] QRect crop() const { return crop_; }
  [[nodiscard]] bool has_picture() const noexcept { return coded_w_ > 0 && coded_h_ > 0; }
  [[nodiscard]] bool fit() const noexcept { return fit_; }
  [[nodiscard]] double zoom() const noexcept { return zoom_; }
  [[nodiscard]] double pan_x() const noexcept { return pan_x_; }
  [[nodiscard]] double pan_y() const noexcept { return pan_y_; }
  [[nodiscard]] bool show_cropped() const noexcept { return show_cropped_; }
  void set_show_cropped(bool on);
  [[nodiscard]] QPoint hover() const { return hover_; }
  [[nodiscard]] QPoint probe() const;
  [[nodiscard]] QRect selection_rect() const;
  [[nodiscard]] QRect drag_rect() const { return drag_rect_; }
  [[nodiscard]] bool panning() const noexcept { return panning_; }
  [[nodiscard]] QJSValue sample_reader() const { return sample_reader_; }
  void set_sample_reader(const QJSValue& reader);
  [[nodiscard]] QString readout() const { return readout_; }

 Q_SIGNALS:
  void selectionChanged();
  void viewChanged();
  void pictureChanged();
  void planeChanged();
  void hoverChanged();
  void probeChanged();
  void rectChanged();
  void dragChanged();
  void readoutChanged();
  // The Selection's frame changed: the decoder path (WI-2.171) decodes it and calls setPicture() with the result.
  // SPEC-ASSUMPTION(#650): WI-1.456 step 1 delivers this hook; the decoder job is WI-2.171's.
  void frameWanted(int decode_index, int display_index);

 private:
  struct Visible {
    double x = 0;
    double y = 0;
    double w = 1;
    double h = 1;
  };
  [[nodiscard]] Visible visible() const;
  [[nodiscard]] QRect snapped_rect(QPointF a, QPointF b) const;
  // Applies zoom/pan (zoom clamped, pan clamped so that kMinVisible device pixels stay visible), leaves fit mode and
  // emits viewChanged when something changed.
  void apply(double zoom, double pan_x, double pan_y);
  void refit();
  void clamp_pan();
  void on_selection_changed(int mask);
  void update_readout();

  QPointer<SelectionModel> selection_;
  QString origin_ = QStringLiteral("picture");
  double vw_ = 0;
  double vh_ = 0;
  int coded_w_ = 0;
  int coded_h_ = 0;
  QRect crop_;
  bool fit_ = true;
  double zoom_ = 1.0;
  double pan_x_ = 0;
  double pan_y_ = 0;
  int plane_mode_ = 0;
  bool show_cropped_ = true;
  int stage_ = 0;
  bool stage_available_ = false;
  QPoint hover_{-1, -1};
  QPointF pointer_;  // the last hover position in device pixels
  bool pointer_inside_ = false;
  QRect drag_rect_;
  QPointF rect_anchor_;
  bool drawing_rect_ = false;
  bool panning_ = false;
  QPointF pan_last_;
  QJSValue sample_reader_;
  QString readout_;
  std::optional<ViewSnapshot> pending_view_;  // zoom and pan of restoreView() waiting for a picture
};

}  // namespace ovt::app

#endif  // OVT_APP_PICTURE_CONTROLLER_H
