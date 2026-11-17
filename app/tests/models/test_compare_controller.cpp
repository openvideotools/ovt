// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of CompareController (WI-2.486; chapter 11 §11.3.2, §11.4.1): the mode cycle (AT1, model half), the divider
// (AT2, model half), SelectionB with the manual-offset rule (AT4), the memory snapshot, Dual view exclusion and the
// first-mismatch finder (AT3, model half: the job itself is the application's). The QML half and the end-to-end
// scripts are app/tests/scripts/test_compare_modes.py.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <QPoint>
#include <QSignalSpy>
#include <QString>

#include <vector>

#include "compare_controller.h"
#include "models/diagnostics_model.h"
#include "models/qt_test_app.h"
#include "models/selection_model.h"

using Catch::Matchers::WithinAbs;
using ovt::app::CompareController;
using ovt::app::DiagnosticsModel;
using ovt::app::SelectionModel;

TEST_CASE("AT1: C cycles Single, Side by side, Split V, Split H, Overlay, Difference, Flicker and wraps",
          "[fast][gui][compare]") {
  ovt::app::testing::qt_app();
  CompareController c;
  c.setFileB(10);
  REQUIRE(c.mode() == CompareController::Single);
  const std::vector<int> expected = {CompareController::SideBySide,      CompareController::SplitVertical,
                                     CompareController::SplitHorizontal, CompareController::Overlay,
                                     CompareController::Difference,      CompareController::Flicker};
  for (const int mode : expected) {
    CHECK(c.handleAction(QStringLiteral("compare.nextMode")));
    CHECK(c.mode() == mode);
  }
  c.nextMode();  // a 7th press wraps to Single
  CHECK(c.mode() == CompareController::Single);
  c.previousMode();  // Shift+C goes back
  CHECK(c.mode() == CompareController::Flicker);
}

TEST_CASE("a mode other than Single needs B; clearing B returns to Single", "[fast][gui][compare]") {
  ovt::app::testing::qt_app();
  CompareController c;
  c.nextMode();
  c.set_mode(CompareController::Difference);
  CHECK(c.mode() == CompareController::Single);
  c.setFileB(5);
  c.set_mode(CompareController::Difference);
  CHECK(c.mode() == CompareController::Difference);
  c.clearB();
  CHECK(c.mode() == CompareController::Single);
  CHECK_FALSE(c.has_b());
}

TEST_CASE("AT2: dragging the divider to 25 % sets split_position to 0.25", "[fast][gui][compare]") {
  ovt::app::testing::qt_app();
  CompareController c;
  c.setFileB(2);
  c.set_mode(CompareController::SplitVertical);
  c.dragDivider(480.0, 1920.0);
  CHECK_THAT(c.split_position(), WithinAbs(0.25, 0.01));
  c.dragDivider(-50.0, 1920.0);  // clamps to the viewport
  CHECK(c.split_position() == 0.0);
  c.dragDivider(5000.0, 1920.0);
  CHECK(c.split_position() == 1.0);
}

TEST_CASE("Difference gain steps through the powers of two 1 to 64 and the keys [ ] move it", "[fast][gui][compare]") {
  ovt::app::testing::qt_app();
  CompareController c;
  CHECK(c.diff_gain() == 1);
  for (const int expected : {2, 4, 8, 16, 32, 64, 64}) {
    c.gainUp();
    CHECK(c.diff_gain() == expected);
  }
  c.gainDown();
  CHECK(c.diff_gain() == 32);
  CHECK(c.handleAction(QStringLiteral("compare.gain.8")));
  CHECK(c.diff_gain() == 8);
  c.toggleFalseColour();
  CHECK(c.diff_false_colour());
}

TEST_CASE("Swap A/B asks the host to exchange its pictures and flips the flag", "[fast][gui][compare]") {
  ovt::app::testing::qt_app();
  CompareController c;
  QSignalSpy swaps(&c, &CompareController::swapRequested);
  c.swapAB();  // no B: nothing to swap
  CHECK(swaps.count() == 0);
  c.setFileB(4);
  c.swapAB();
  CHECK(swaps.count() == 1);
  CHECK(c.swapped());
  c.handleAction(QStringLiteral("compare.swap"));
  CHECK_FALSE(c.swapped());
}

TEST_CASE("Set as B from memory: the host snapshots the picture and slot B appears", "[fast][gui][compare]") {
  ovt::app::testing::qt_app();
  CompareController c;
  QSignalSpy asked(&c, &CompareController::snapshotRequested);
  c.setAsBFromMemory();
  CHECK(asked.count() == 1);
  c.snapshotTaken(false);  // the host had no picture
  CHECK_FALSE(c.has_b());
  c.snapshotTaken(true);
  CHECK(c.has_b());
  CHECK(c.b_source() == QStringLiteral("memory"));
  c.nextMode();
  CHECK(c.mode() == CompareController::SideBySide);
  CHECK(c.selectionBFrame(41) == 0);  // a snapshot is one picture, shown against every A frame
}

TEST_CASE("Dual view resets the mode to Single and disables compare", "[fast][gui][compare]") {
  ovt::app::testing::qt_app();
  CompareController c;
  c.setFileB(4);
  c.set_mode(CompareController::Overlay);
  c.set_dual_view(true);
  CHECK(c.mode() == CompareController::Single);
  CHECK_FALSE(c.enabled());
  c.nextMode();
  c.set_mode(CompareController::SplitVertical);
  CHECK(c.mode() == CompareController::Single);
  QSignalSpy asked(&c, &CompareController::snapshotRequested);
  c.setAsBFromMemory();
  CHECK(asked.count() == 0);
  c.set_dual_view(false);
  c.nextMode();
  CHECK(c.mode() == CompareController::SideBySide);
}

TEST_CASE("AT4: offset +3 with B two frames shorter than A leaves SelectionB empty at A's last frame",
          "[fast][gui][compare]") {
  ovt::app::testing::qt_app();
  SelectionModel selection;
  CompareController c;
  c.set_selection(&selection);
  c.set_a_frame_count(100);
  c.setFileB(98);
  c.set_frame_offset(3);
  selection.setFrame({99, 99}, QStringLiteral("test"));
  CHECK(c.b_frame_index() == -1);
  CHECK(c.b_empty());
  CHECK(c.b_column_text() == QString::fromUtf8("—"));
  // Inside B's range the offset applies: 94 + 3 = 97 is B's last frame, 95 + 3 is past it.
  selection.setFrame({94, 94}, QStringLiteral("test"));
  CHECK(c.b_frame_index() == 97);
  CHECK(c.b_column_text() == QStringLiteral("97"));
  selection.setFrame({95, 95}, QStringLiteral("test"));
  CHECK(c.b_frame_index() == -1);
  // A negative result is empty too.
  c.set_frame_offset(-3);
  selection.setFrame({1, 1}, QStringLiteral("test"));
  CHECK(c.b_frame_index() == -1);
}

TEST_CASE("SelectionB rules: the manual offset beats the alignment, which beats the same display index",
          "[fast][gui][compare]") {
  ovt::app::testing::qt_app();
  CompareController c;
  c.setFileB(50);
  CHECK(c.selectionBFrame(10) == 10);  // rule 3
  CHECK(c.selectionBFrame(50) == -1);  // B is shorter
  c.setAlignment(true, 7);
  CHECK(c.selectionBFrame(10) == 17);  // rule 2
  c.set_frame_offset(-2);
  CHECK(c.selectionBFrame(10) == 8);  // rule 1
  c.set_frame_offset(0);
  CHECK(c.selectionBFrame(10) == 17);  // offset 0 is "not set"
  c.clearB();
  CHECK(c.selectionBFrame(0) == -1);
}

TEST_CASE("AT3 (model half): first mismatch selects the frame, pins the probe and posts VQ-DIFF-0001",
          "[fast][gui][compare]") {
  ovt::app::testing::qt_app();
  SelectionModel selection;
  DiagnosticsModel diagnostics;
  CompareController c;
  c.set_selection(&selection);
  c.set_diagnostics(&diagnostics);

  int started = 0;
  int cancelled = 0;
  c.setJobRunner({[&] { ++started; }, [&] { ++cancelled; }});
  CHECK_FALSE(c.findFirstMismatch());  // no B yet
  CHECK(started == 0);
  c.setFileB(60);
  REQUIRE(c.findFirstMismatch());
  CHECK(started == 1);
  CHECK(c.finding());
  CHECK_FALSE(c.findFirstMismatch());  // one job at a time

  c.completeFirstMismatch(true, 37, 0, 611, 203, 96, 97);
  CHECK_FALSE(c.finding());
  CHECK(selection.frame_display_index() == 37);
  CHECK(selection.probe() == QPoint(611, 203));
  REQUIRE(diagnostics.rowCount() == 1);
  CHECK(c.find_status().contains(QStringLiteral("frame 37")));
}

TEST_CASE("first mismatch in a chroma plane pins the probe at the luma position", "[fast][gui][compare]") {
  ovt::app::testing::qt_app();
  SelectionModel selection;
  CompareController c;
  c.set_selection(&selection);
  c.setJobRunner({[] {}, [] {}});
  c.setFileB(60);
  REQUIRE(c.findFirstMismatch());
  c.completeFirstMismatch(true, 5, 1, 100, 50, 128, 129, 2, 2);
  CHECK(selection.probe() == QPoint(200, 100));
}

TEST_CASE("first mismatch: identical pair, cancel and failure", "[fast][gui][compare]") {
  ovt::app::testing::qt_app();
  SelectionModel selection;
  DiagnosticsModel diagnostics;
  CompareController c;
  c.set_selection(&selection);
  c.set_diagnostics(&diagnostics);
  int cancelled = 0;
  c.setJobRunner({[] {}, [&] { ++cancelled; }});
  c.setFileB(10);

  REQUIRE(c.findFirstMismatch());
  c.completeFirstMismatch(false, -1, 0, 0, 0, 0, 0);
  CHECK(diagnostics.rowCount() == 0);
  CHECK(selection.frame_display_index() == -1);
  CHECK(c.find_status().contains(QStringLiteral("identical")));

  REQUIRE(c.findFirstMismatch());
  c.cancelFirstMismatch();
  CHECK(cancelled == 1);
  CHECK_FALSE(c.finding());
  c.completeFirstMismatch(true, 3, 0, 1, 1, 0, 1);  // a late result of the cancelled job is dropped
  CHECK(selection.frame_display_index() == -1);

  REQUIRE(c.findFirstMismatch());
  c.failFirstMismatch(QStringLiteral("boom"));
  CHECK(c.find_status() == QStringLiteral("boom"));
}
