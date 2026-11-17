// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// CompareController (chapter 11 §11.3.2 "Compare modes", §11.4.1 SelectionB; WI-2.486): the model behind the Picture
// panel's compare controls. It owns the ovt::render::CompareMode values (chapter 10 §10.2.4) as plain properties,
// cycles the mode (`C` / `Shift+C`), swaps A and B (`X`), steps the Difference gain (`[` `]`), maps divider drags to
// `split_position`, derives SelectionB and runs the first-mismatch finder (FR-PLN-009). It never renders or calls the
// core: PictureHost binds to the properties and draws (FR-GUI-030); an installed JobRunner runs the job and delivers
// its result to completeFirstMismatch().
//
// Plane modes are not part of CompareMode: ViewState.plane_mode applies to both slots (spec rc41, #623).

#ifndef OVT_APP_COMPARE_CONTROLLER_H
#define OVT_APP_COMPARE_CONTROLLER_H

#include <QtQml/qqmlregistration.h>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

#include "models/diagnostics_model.h"
#include "models/selection_model.h"

namespace ovt::app {

class CompareController : public QObject {
  Q_OBJECT
  QML_ELEMENT
  Q_PROPERTY(ovt::app::SelectionModel* selection READ selection WRITE set_selection NOTIFY selectionChanged)
  Q_PROPERTY(ovt::app::DiagnosticsModel* diagnostics READ diagnostics WRITE set_diagnostics NOTIFY selectionChanged)
  Q_PROPERTY(QString origin READ origin WRITE set_origin NOTIFY selectionChanged)

  // The Compare combo (§11.3.2 order): Single, Side by side, Split V, Split H, Overlay, Difference, Flicker.
  Q_PROPERTY(int mode READ mode WRITE set_mode NOTIFY modeChanged)
  Q_PROPERTY(QStringList modeNames READ mode_names CONSTANT)
  // CompareMode.kind / split_axis (indices of the enums of chapter 10 §10.2.4) for PictureHost.
  Q_PROPERTY(int kind READ kind NOTIFY modeChanged)
  Q_PROPERTY(int splitAxis READ split_axis NOTIFY modeChanged)
  Q_PROPERTY(double splitPosition READ split_position WRITE set_split_position NOTIFY paramsChanged)
  Q_PROPERTY(double overlayAlpha READ overlay_alpha WRITE set_overlay_alpha NOTIFY paramsChanged)
  Q_PROPERTY(int diffGain READ diff_gain WRITE set_diff_gain NOTIFY paramsChanged)
  Q_PROPERTY(bool diffFalseColour READ diff_false_colour WRITE set_diff_false_colour NOTIFY paramsChanged)
  Q_PROPERTY(int flickerPeriodMs READ flicker_period_ms WRITE set_flicker_period_ms NOTIFY paramsChanged)
  // Slot B: "none", "file" (a role reference/distorted file) or "memory" (a snapshot, "Set as B from memory").
  Q_PROPERTY(QString bSource READ b_source NOTIFY bChanged)
  Q_PROPERTY(bool hasB READ has_b NOTIFY bChanged)
  Q_PROPERTY(bool swapped READ swapped NOTIFY bChanged)
  // Dual view (WI-2.489) sets it: the mode resets to Single and the combo is disabled (single-surface only).
  Q_PROPERTY(bool dualView READ dual_view WRITE set_dual_view NOTIFY dualViewChanged)
  Q_PROPERTY(bool enabled READ enabled NOTIFY dualViewChanged)

  // SelectionB inputs and result (§11.4.1 "Compare (B side)").
  Q_PROPERTY(int frameOffset READ frame_offset WRITE set_frame_offset NOTIFY selectionBChanged)
  Q_PROPERTY(int aFrameCount READ a_frame_count WRITE set_a_frame_count NOTIFY selectionBChanged)
  Q_PROPERTY(int bFrameCount READ b_frame_count WRITE set_b_frame_count NOTIFY selectionBChanged)
  // The metrics alignment offset (rule 2); `hasAlignment` is false until a metrics run has found one.
  Q_PROPERTY(bool hasAlignment READ has_alignment NOTIFY selectionBChanged)
  Q_PROPERTY(int alignmentOffset READ alignment_offset NOTIFY selectionBChanged)
  // SelectionB.frame.display_index; -1 = SelectionB is empty ("no frame" placeholder, A/B columns show "—").
  Q_PROPERTY(int bFrameIndex READ b_frame_index NOTIFY selectionBChanged)
  Q_PROPERTY(bool bEmpty READ b_empty NOTIFY selectionBChanged)
  Q_PROPERTY(QString bColumnText READ b_column_text NOTIFY selectionBChanged)

  // The first-mismatch finder.
  Q_PROPERTY(bool finding READ finding NOTIFY findingChanged)
  Q_PROPERTY(QString findStatus READ find_status NOTIFY findingChanged)

 public:
  enum Mode : int { Single, SideBySide, SplitVertical, SplitHorizontal, Overlay, Difference, Flicker };
  Q_ENUM(Mode)
  static constexpr int kModeCount = 7;
  static constexpr int kMinGain = 1;
  static constexpr int kMaxGain = 64;  // FR-PLN-007: 1-64x; the combo offers 1..32

  // Runs the core compare job (chapter 9 §9.3.3, `identity`) on A and B. It must return at once and later call
  // completeFirstMismatch() or failFirstMismatch() on the GUI thread. `cancel` is called by cancelFirstMismatch().
  struct JobRunner {
    std::function<void()> start;
    std::function<void()> cancel;
  };

  explicit CompareController(QObject* parent = nullptr);
  ~CompareController() override;
  CompareController(const CompareController&) = delete;
  CompareController& operator=(const CompareController&) = delete;
  CompareController(CompareController&&) = delete;
  CompareController& operator=(CompareController&&) = delete;

  // ---- Mode ---------------------------------------------------------------------------------------------------
  [[nodiscard]] int mode() const noexcept { return mode_; }
  // Ignored while B is missing (a mode needs B) or Dual view is on; other values are clamped away.
  void set_mode(int mode);
  [[nodiscard]] static QStringList mode_names();
  [[nodiscard]] int kind() const noexcept;
  [[nodiscard]] int split_axis() const noexcept { return mode_ == SplitHorizontal ? 1 : 0; }
  Q_INVOKABLE void nextMode();
  Q_INVOKABLE void previousMode();
  Q_INVOKABLE void swapAB();
  Q_INVOKABLE void gainUp();
  Q_INVOKABLE void gainDown();
  Q_INVOKABLE void toggleFalseColour();

  // ---- Parameters ---------------------------------------------------------------------------------------------
  [[nodiscard]] double split_position() const noexcept { return split_position_; }
  void set_split_position(double fraction);  // clamped to 0..1
  // A divider drag: the pointer is at `pos` along an extent of `extent` device pixels (x for Split V, y for Split H).
  Q_INVOKABLE void dragDivider(double pos, double extent);
  [[nodiscard]] double overlay_alpha() const noexcept { return overlay_alpha_; }
  void set_overlay_alpha(double alpha);
  [[nodiscard]] int diff_gain() const noexcept { return diff_gain_; }
  void set_diff_gain(int gain);  // snapped to the powers of two 1..64
  [[nodiscard]] bool diff_false_colour() const noexcept { return diff_false_colour_; }
  void set_diff_false_colour(bool on);
  [[nodiscard]] int flicker_period_ms() const noexcept { return flicker_period_ms_; }
  void set_flicker_period_ms(int ms);

  // ---- Slot B -------------------------------------------------------------------------------------------------
  [[nodiscard]] QString b_source() const;
  [[nodiscard]] bool has_b() const noexcept { return b_source_ != BSource::None; }
  [[nodiscard]] bool swapped() const noexcept { return swapped_; }
  // A file with role reference/distorted is open as B (with its frame count); `frames` <= 0 clears B.
  Q_INVOKABLE void setFileB(int frames);
  // "Picture ▸ Set as B from memory": PictureHost snapshots the frame it shows into slot B (snapshotRequested), then
  // the host calls snapshotTaken().
  Q_INVOKABLE void setAsBFromMemory();
  Q_INVOKABLE void snapshotTaken(bool ok);
  Q_INVOKABLE void clearB();
  [[nodiscard]] bool dual_view() const noexcept { return dual_view_; }
  void set_dual_view(bool on);
  [[nodiscard]] bool enabled() const noexcept { return !dual_view_; }

  // ---- SelectionB ---------------------------------------------------------------------------------------------
  [[nodiscard]] int frame_offset() const noexcept { return frame_offset_; }
  void set_frame_offset(int offset);
  [[nodiscard]] int a_frame_count() const noexcept { return a_frames_; }
  void set_a_frame_count(int n);
  [[nodiscard]] int b_frame_count() const noexcept { return b_frames_; }
  void set_b_frame_count(int n);
  [[nodiscard]] bool has_alignment() const noexcept { return has_alignment_; }
  [[nodiscard]] int alignment_offset() const noexcept { return alignment_; }
  Q_INVOKABLE void setAlignment(bool known, int offset);
  // The rule of §11.4.1, first match wins: (1) a manual offset: a + offset; (2) the aligned index a + alignment;
  // (3) the same display_index. -1 when the result is outside B's frames.
  Q_INVOKABLE [[nodiscard]] int selectionBFrame(int a_display_index) const;
  [[nodiscard]] int b_frame_index() const;
  [[nodiscard]] bool b_empty() const { return has_b() && b_frame_index() < 0; }
  [[nodiscard]] QString b_column_text() const;

  // ---- First mismatch (FR-PLN-009) ----------------------------------------------------------------------------
  void setJobRunner(JobRunner runner);
  [[nodiscard]] bool finding() const noexcept { return finding_; }
  [[nodiscard]] QString find_status() const { return find_status_; }
  // Starts the cancellable job; false (with findStatus set) when B is missing or no runner is installed.
  Q_INVOKABLE bool findFirstMismatch();
  Q_INVOKABLE void cancelFirstMismatch();
  // The job's result (frame is the A display index; plane 0/1/2 with (x, y) in that plane's samples; `sub_x` and
  // `sub_y` are the plane's subsampling so the probe can be pinned in luma samples). Posts VQ-DIFF-0001, selects the
  // frame and pins the probe. `found == false`: the pair is identical.
  Q_INVOKABLE void completeFirstMismatch(bool found, int display_index, int plane, int x, int y, int value_a,
                                         int value_b, int sub_x = 1, int sub_y = 1);
  Q_INVOKABLE void failFirstMismatch(const QString& message);

  // The Keymap / menu actions of the Compare menu (`compare.*`), given without the "action." prefix. Returns true
  // when the id is one of them.
  Q_INVOKABLE bool handleAction(const QString& id);

  // ---- Properties ---------------------------------------------------------------------------------------------
  [[nodiscard]] SelectionModel* selection() const { return selection_.data(); }
  void set_selection(SelectionModel* selection);
  [[nodiscard]] DiagnosticsModel* diagnostics() const { return diagnostics_.data(); }
  void set_diagnostics(DiagnosticsModel* diagnostics);
  [[nodiscard]] QString origin() const { return origin_; }
  void set_origin(const QString& origin);

 Q_SIGNALS:
  void selectionChanged();
  void modeChanged();
  void paramsChanged();
  void bChanged();
  void dualViewChanged();
  void selectionBChanged();
  void findingChanged();
  // PictureHost is to copy the picture it shows into slot B, then call snapshotTaken(ok).
  void snapshotRequested();
  // PictureHost is to exchange its A and B pictures.
  void swapRequested();

 private:
  enum class BSource : int { None, File, Memory };
  void set_b(BSource source);
  void finish_find(const QString& status);

  QPointer<SelectionModel> selection_;
  QPointer<DiagnosticsModel> diagnostics_;
  QString origin_ = QStringLiteral("picture");
  int mode_ = Single;
  double split_position_ = 0.5;
  double overlay_alpha_ = 0.5;
  int diff_gain_ = 1;
  bool diff_false_colour_ = false;
  int flicker_period_ms_ = 500;
  BSource b_source_ = BSource::None;
  bool swapped_ = false;
  bool dual_view_ = false;
  int frame_offset_ = 0;
  int a_frames_ = 0;
  int b_frames_ = 0;
  bool has_alignment_ = false;
  int alignment_ = 0;
  JobRunner runner_;
  bool finding_ = false;
  QString find_status_;
};

}  // namespace ovt::app

#endif  // OVT_APP_COMPARE_CONTROLLER_H
