// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// CompareController (chapter 11 §11.3.2, §11.4.1; WI-2.486). See compare_controller.h.

#include "compare_controller.h"

#include <QPoint>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace ovt::app {

namespace {

constexpr const char* kFindOrigin = "compare";

// CompareMode::Kind indices of chapter 10 §10.2.4: Single, SideBySide, Split, Overlay, Difference, Flicker.
constexpr int kKindOfMode[CompareController::kModeCount] = {0, 1, 2, 2, 3, 4, 5};

}  // namespace

CompareController::CompareController(QObject* parent) : QObject(parent) {}

CompareController::~CompareController() = default;

// ---- Mode ----------------------------------------------------------------------------------------------------------

QStringList CompareController::mode_names() {
  return {tr("Single"),  tr("Side by side"), tr("Split vertical"), tr("Split horizontal"),
          tr("Overlay"), tr("Difference"),   tr("Flicker")};
}

int CompareController::kind() const noexcept {
  return kKindOfMode[std::clamp(mode_, 0, kModeCount - 1)];
}

void CompareController::set_mode(int mode) {
  if (mode < 0 || mode >= kModeCount || mode == mode_) {
    return;
  }
  // A mode other than Single needs B, and compare is single-surface only (§11.3.2).
  if (mode != Single && (!has_b() || dual_view_)) {
    return;
  }
  mode_ = mode;
  Q_EMIT modeChanged();
}

void CompareController::nextMode() {
  if (!has_b() || dual_view_) {
    return;
  }
  set_mode((mode_ + 1) % kModeCount);
}

void CompareController::previousMode() {
  if (!has_b() || dual_view_) {
    return;
  }
  set_mode((mode_ + kModeCount - 1) % kModeCount);
}

void CompareController::swapAB() {
  if (!has_b() || dual_view_) {
    return;
  }
  swapped_ = !swapped_;
  Q_EMIT swapRequested();
  Q_EMIT bChanged();
}

void CompareController::gainUp() {
  set_diff_gain(diff_gain_ * 2);
}

void CompareController::gainDown() {
  set_diff_gain(diff_gain_ / 2);
}

void CompareController::toggleFalseColour() {
  set_diff_false_colour(!diff_false_colour_);
}

// ---- Parameters ----------------------------------------------------------------------------------------------------

void CompareController::set_split_position(double fraction) {
  const double v = std::clamp(std::isfinite(fraction) ? fraction : 0.5, 0.0, 1.0);
  if (v == split_position_) {
    return;
  }
  split_position_ = v;
  Q_EMIT paramsChanged();
}

void CompareController::dragDivider(double pos, double extent) {
  if (extent > 0) {
    set_split_position(pos / extent);
  }
}

void CompareController::set_overlay_alpha(double alpha) {
  const double v = std::clamp(std::isfinite(alpha) ? alpha : 0.5, 0.0, 1.0);
  if (v == overlay_alpha_) {
    return;
  }
  overlay_alpha_ = v;
  Q_EMIT paramsChanged();
}

void CompareController::set_diff_gain(int gain) {
  // The gain steps through 1, 2, 4, ... 64; any other value snaps down to the power of two at or below it.
  int g = kMinGain;
  while (g * 2 <= std::clamp(gain, kMinGain, kMaxGain)) {
    g *= 2;
  }
  if (g == diff_gain_) {
    return;
  }
  diff_gain_ = g;
  Q_EMIT paramsChanged();
}

void CompareController::set_diff_false_colour(bool on) {
  if (on == diff_false_colour_) {
    return;
  }
  diff_false_colour_ = on;
  Q_EMIT paramsChanged();
}

void CompareController::set_flicker_period_ms(int ms) {
  const int v = std::clamp(ms, 50, 5000);
  if (v == flicker_period_ms_) {
    return;
  }
  flicker_period_ms_ = v;
  Q_EMIT paramsChanged();
}

// ---- Slot B --------------------------------------------------------------------------------------------------------

QString CompareController::b_source() const {
  switch (b_source_) {
    case BSource::File:
      return QStringLiteral("file");
    case BSource::Memory:
      return QStringLiteral("memory");
    case BSource::None:
      break;
  }
  return QStringLiteral("none");
}

void CompareController::set_b(BSource source) {
  if (source == b_source_) {
    return;
  }
  b_source_ = source;
  if (source == BSource::None) {
    swapped_ = false;
    if (mode_ != Single) {
      mode_ = Single;
      Q_EMIT modeChanged();
    }
  }
  Q_EMIT bChanged();
  Q_EMIT selectionBChanged();
}

void CompareController::setFileB(int frames) {
  b_frames_ = std::max(frames, 0);
  set_b(frames > 0 ? BSource::File : BSource::None);
  Q_EMIT selectionBChanged();
}

void CompareController::setAsBFromMemory() {
  if (!dual_view_) {
    Q_EMIT snapshotRequested();
  }
}

void CompareController::snapshotTaken(bool ok) {
  if (!ok) {
    return;
  }
  // A snapshot is a single picture: B has one frame, which SelectionB shows for every A frame (index 0).
  b_frames_ = 1;
  frame_offset_ = 0;
  has_alignment_ = false;
  set_b(BSource::Memory);
  Q_EMIT selectionBChanged();
}

void CompareController::clearB() {
  set_b(BSource::None);
}

void CompareController::set_dual_view(bool on) {
  if (on == dual_view_) {
    return;
  }
  dual_view_ = on;
  if (on && mode_ != Single) {
    mode_ = Single;
    Q_EMIT modeChanged();
  }
  Q_EMIT dualViewChanged();
}

// ---- SelectionB ----------------------------------------------------------------------------------------------------

void CompareController::set_frame_offset(int offset) {
  if (offset == frame_offset_) {
    return;
  }
  frame_offset_ = offset;
  Q_EMIT selectionBChanged();
}

void CompareController::set_a_frame_count(int n) {
  if (n == a_frames_) {
    return;
  }
  a_frames_ = n;
  Q_EMIT selectionBChanged();
}

void CompareController::set_b_frame_count(int n) {
  if (n == b_frames_) {
    return;
  }
  b_frames_ = n;
  Q_EMIT selectionBChanged();
}

void CompareController::setAlignment(bool known, int offset) {
  if (known == has_alignment_ && (!known || offset == alignment_)) {
    return;
  }
  has_alignment_ = known;
  alignment_ = known ? offset : 0;
  Q_EMIT selectionBChanged();
}

int CompareController::selectionBFrame(int a_display_index) const {
  if (!has_b() || a_display_index < 0) {
    return -1;
  }
  int b = a_display_index;
  if (b_source_ == BSource::Memory) {
    b = 0;  // a snapshot has one picture
  } else if (frame_offset_ != 0) {
    b = a_display_index + frame_offset_;  // rule 1: the manual offset
  } else if (has_alignment_) {
    b = a_display_index + alignment_;  // rule 2: the metrics alignment
  }
  // Rules 1 to 3: outside B's frames, SelectionB is empty.
  return (b >= 0 && b < b_frames_) ? b : -1;
}

int CompareController::b_frame_index() const {
  return selectionBFrame(selection_ != nullptr ? selection_->frame_display_index() : -1);
}

QString CompareController::b_column_text() const {
  const int b = b_frame_index();
  return b < 0 ? QStringLiteral("—") : QString::number(b);
}

// ---- First mismatch ------------------------------------------------------------------------------------------------

void CompareController::setJobRunner(JobRunner runner) {
  runner_ = std::move(runner);
}

void CompareController::finish_find(const QString& status) {
  finding_ = false;
  find_status_ = status;
  Q_EMIT findingChanged();
}

bool CompareController::findFirstMismatch() {
  if (finding_) {
    return false;
  }
  if (!has_b()) {
    find_status_ = tr("Open a file as B (or Set as B from memory) first.");
    Q_EMIT findingChanged();
    return false;
  }
  if (!runner_.start) {
    find_status_ = tr("The compare job is not available in this build.");
    Q_EMIT findingChanged();
    return false;
  }
  finding_ = true;
  find_status_ = tr("Comparing A and B…");
  Q_EMIT findingChanged();
  runner_.start();
  return true;
}

void CompareController::cancelFirstMismatch() {
  if (!finding_) {
    return;
  }
  if (runner_.cancel) {
    runner_.cancel();
  }
  finish_find(tr("Cancelled."));
}

void CompareController::completeFirstMismatch(bool found, int display_index, int plane, int x, int y, int value_a,
                                              int value_b, int sub_x, int sub_y) {
  if (!finding_) {
    return;  // cancelled meanwhile
  }
  if (!found) {
    if (diagnostics_ != nullptr) {
      diagnostics_->removeSource(QString::fromLatin1(kFindOrigin));
    }
    finish_find(tr("A and B are identical."));
    return;
  }
  // The probe is in luma samples (§11.4.1); (x, y) is in the plane's own samples.
  const QPoint probe(x * std::max(sub_x, 1), y * std::max(sub_y, 1));
  static const char* const kPlane[] = {"Y", "U", "V"};
  const QString plane_name = QString::fromLatin1(kPlane[std::clamp(plane, 0, 2)]);
  if (diagnostics_ != nullptr) {
    DiagnosticEntry entry;
    entry.severity = DiagSeverity::Info;
    entry.code = QStringLiteral("VQ-DIFF-0001");
    entry.message = QStringLiteral("first differing sample: frame %1, plane %2, (%3,%4): ref %5 dist %6")
                        .arg(display_index)
                        .arg(plane)
                        .arg(x)
                        .arg(y)
                        .arg(value_a)
                        .arg(value_b);
    entry.spec_ref = QStringLiteral("9.4.6");
    entry.location.kind = DiagnosticLocation::Kind::FrameProbe;
    entry.location.display_index = display_index;
    entry.location.probe = probe;
    std::vector<DiagnosticEntry> entries;
    entries.push_back(std::move(entry));
    diagnostics_->setSource(QString::fromLatin1(kFindOrigin), std::move(entries));
  }
  if (selection_ != nullptr) {
    // Jump to the frame (its decode index is filled by the SelectionModel's index) and pin the probe.
    selection_->selectFrame(-1, display_index, origin_);
    selection_->selectProbe(probe.x(), probe.y(), origin_);
  }
  finish_find(tr("First difference: frame %1, plane %2, (%3, %4): %5 vs %6")
                  .arg(display_index)
                  .arg(plane_name)
                  .arg(x)
                  .arg(y)
                  .arg(value_a)
                  .arg(value_b));
}

void CompareController::failFirstMismatch(const QString& message) {
  if (finding_) {
    finish_find(message);
  }
}

// ---- Actions -------------------------------------------------------------------------------------------------------

bool CompareController::handleAction(const QString& id) {
  static const struct {
    const char* id;
    int mode;
  } kModes[] = {{"compare.single", Single},
                {"compare.sideBySide", SideBySide},
                {"compare.splitVertical", SplitVertical},
                {"compare.splitHorizontal", SplitHorizontal},
                {"compare.overlay", Overlay},
                {"compare.difference", Difference},
                {"compare.flicker", Flicker}};
  for (const auto& m : kModes) {
    if (id == QLatin1String(m.id)) {
      set_mode(m.mode);
      return true;
    }
  }
  if (id == QLatin1String("compare.nextMode")) {
    nextMode();
  } else if (id == QLatin1String("compare.previousMode")) {
    previousMode();
  } else if (id == QLatin1String("compare.swap")) {
    swapAB();
  } else if (id == QLatin1String("compare.gainUp")) {
    gainUp();
  } else if (id == QLatin1String("compare.gainDown")) {
    gainDown();
  } else if (id == QLatin1String("compare.falseColour")) {
    toggleFalseColour();
  } else if (id.startsWith(QLatin1String("compare.gain."))) {
    bool ok = false;
    const int gain = id.mid(13).toInt(&ok);
    if (!ok) {
      return false;
    }
    set_diff_gain(gain);
  } else if (id == QLatin1String("compare.findFirstMismatch")) {
    findFirstMismatch();
  } else if (id == QLatin1String("compare.setAsB")) {
    setAsBFromMemory();
  } else {
    return false;
  }
  return true;
}

// ---- Properties ----------------------------------------------------------------------------------------------------

void CompareController::set_selection(SelectionModel* selection) {
  if (selection == selection_.data()) {
    return;
  }
  if (selection_ != nullptr) {
    disconnect(selection_, nullptr, this, nullptr);
  }
  selection_ = selection;
  if (selection_ != nullptr) {
    // SelectionB follows SelectionA's frame (§11.4.1).
    connect(selection_, &SelectionModel::changed, this, [this](int mask) {
      if ((static_cast<unsigned>(mask) & SelectionModel::ChangeFrame) != 0U) {
        Q_EMIT selectionBChanged();
      }
    });
  }
  Q_EMIT selectionChanged();
  Q_EMIT selectionBChanged();
}

void CompareController::set_diagnostics(DiagnosticsModel* diagnostics) {
  if (diagnostics == diagnostics_.data()) {
    return;
  }
  diagnostics_ = diagnostics;
  Q_EMIT selectionChanged();
}

void CompareController::set_origin(const QString& origin) {
  if (origin == origin_) {
    return;
  }
  origin_ = origin;
  Q_EMIT selectionChanged();
}

}  // namespace ovt::app
