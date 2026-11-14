// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// TimelineViewport (chapter 11 §11.3.1 "Interactions"; WI-1.455b): which stretch of the frame positions the Timeline
// shows, and the zoom / pan / fit arithmetic. Wheel zooms about the cursor (1.25x per notch), Shift+wheel pans,
// double-click fits. No Qt types.

#ifndef OVT_APP_MODELS_TIMELINE_VIEWPORT_H
#define OVT_APP_MODELS_TIMELINE_VIEWPORT_H

#include <cstddef>

namespace ovt::app {

class TimelineViewport {
 public:
  // The zoom step of one wheel notch (angleDelta 120).
  static constexpr double kZoomPerNotch = 1.25;
  // The smallest visible stretch, in frames (or the whole series when shorter).
  static constexpr double kMinCount = 4.0;

  TimelineViewport() = default;
  explicit TimelineViewport(std::size_t total) { set_total(total); }

  // A new series length. The view is kept when it still fits, otherwise fitted.
  void set_total(std::size_t total);
  [[nodiscard]] std::size_t total() const noexcept { return total_; }

  void fit();
  // The visible stretch is [first(), first() + count()) in frame positions (fractional at the edges).
  [[nodiscard]] double first() const noexcept { return first_; }
  [[nodiscard]] double count() const noexcept { return count_; }
  [[nodiscard]] bool is_fit() const noexcept { return count_ >= static_cast<double>(total_); }

  // The whole frames overlapping the view: [first_frame(), end_frame()).
  [[nodiscard]] std::size_t first_frame() const noexcept;
  [[nodiscard]] std::size_t end_frame() const noexcept;

  // Zooms in by `factor` (> 1 in, < 1 out) keeping the frame at `anchor` (0..1 across the width) where it is.
  void zoom_about(double factor, double anchor);
  // Zooms by `notches` wheel notches (angleDelta / 120; positive zooms in).
  void zoom_notches(double notches, double anchor);
  // Moves the view by `frames` (positive: later), clamped.
  void pan_frames(double frames);

  // Frame position under device x of a plot `width` wide, and back. position_at may be fractional or outside the
  // series; frame_at clamps to a valid frame.
  [[nodiscard]] double position_at(double x, double width) const noexcept;
  [[nodiscard]] double x_of(double position, double width) const noexcept;
  [[nodiscard]] std::size_t frame_at(double x, double width) const noexcept;

 private:
  void clamp() noexcept;
  std::size_t total_ = 0;
  double first_ = 0.0;
  double count_ = 0.0;
};

}  // namespace ovt::app

#endif  // OVT_APP_MODELS_TIMELINE_VIEWPORT_H
