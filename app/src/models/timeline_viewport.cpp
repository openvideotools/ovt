// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "models/timeline_viewport.h"

#include <algorithm>
#include <cmath>

namespace ovt::app {

void TimelineViewport::set_total(std::size_t total) {
  const bool was_fit = is_fit();
  total_ = total;
  if (was_fit || count_ > static_cast<double>(total_)) {
    fit();
  } else {
    clamp();
  }
}

void TimelineViewport::fit() {
  first_ = 0.0;
  count_ = static_cast<double>(total_);
}

void TimelineViewport::clamp() noexcept {
  const auto total = static_cast<double>(total_);
  count_ = std::clamp(count_, std::min(kMinCount, total), total);
  first_ = std::clamp(first_, 0.0, total - count_);
}

std::size_t TimelineViewport::first_frame() const noexcept {
  return static_cast<std::size_t>(std::floor(first_));
}

std::size_t TimelineViewport::end_frame() const noexcept {
  // Tolerance for the rounding of first_ + count_ at the right edge.
  return std::min(total_, static_cast<std::size_t>(std::ceil(first_ + count_ - 1e-9)));
}

void TimelineViewport::zoom_about(double factor, double anchor) {
  if (total_ == 0 || !(factor > 0.0)) {
    return;
  }
  anchor = std::clamp(anchor, 0.0, 1.0);
  const double anchor_position = first_ + anchor * count_;
  const double old_count = count_;
  count_ = old_count / factor;
  clamp();
  first_ = anchor_position - anchor * count_;
  clamp();
}

void TimelineViewport::zoom_notches(double notches, double anchor) {
  zoom_about(std::pow(kZoomPerNotch, notches), anchor);
}

void TimelineViewport::pan_frames(double frames) {
  first_ += frames;
  clamp();
}

double TimelineViewport::position_at(double x, double width) const noexcept {
  return width > 0.0 ? first_ + x / width * count_ : first_;
}

double TimelineViewport::x_of(double position, double width) const noexcept {
  return count_ > 0.0 ? (position - first_) / count_ * width : 0.0;
}

std::size_t TimelineViewport::frame_at(double x, double width) const noexcept {
  if (total_ == 0) {
    return 0;
  }
  const double position = std::floor(position_at(x, width));
  return static_cast<std::size_t>(std::clamp(position, 0.0, static_cast<double>(total_ - 1)));
}

}  // namespace ovt::app
