// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of JobsModel (WI-1.453; chapter 11 §11.3.18, §11.7). Acceptance test 1: a running job that reports progress
// shows its percentage in the status-bar properties within 100 ms.

#include <catch2/catch_test_macros.hpp>

#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "models/jobs_model.h"
#include "qt_test_app.h"

namespace {

using namespace std::chrono_literals;
using ovt::app::JobContext;
using ovt::app::JobsModel;
using ovt::app::testing::spin_until;

// Blocks the job body until released or cancelled.
ovt_status wait_for(JobContext& ctx, const std::atomic<bool>& release) {
  while (!release.load()) {
    if (ctx.cancelled()) {
      return OVT_E_CANCELLED;
    }
    std::this_thread::sleep_for(1ms);
  }
  return OVT_OK;
}

QString state_of(const JobsModel& model, int row) {
  return model.data(model.index(row), JobsModel::StateRole).toString();
}

}  // namespace

TEST_CASE("JobsModel: progress reaches the status bar within 100 ms", "[fast][gui][jobs]") {
  ovt::app::testing::qt_app();
  JobsModel model(2, nullptr);
  std::atomic<bool> release{false};
  std::atomic<std::int64_t> reported_ns{0};
  model.enqueue(QStringLiteral("Indexing"), [&](JobContext& ctx) {
    ctx.report(1203, 2870, QStringLiteral("frames"));
    reported_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    return wait_for(ctx, release);
  });
  CHECK(model.activeCount() == 1);
  REQUIRE(spin_until([&] { return model.foregroundProgress() > 0.0; }, 2000ms));
  const auto seen_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  CHECK((seen_ns - reported_ns.load()) < 100'000'000);
  CHECK(model.foregroundProgress() == 1203.0 / 2870.0);
  // The wording of §11.7: name, percent, done / total with the stage as unit; no ETA before 1 % of the work.
  CHECK(model.foregroundText().startsWith(QStringLiteral("Indexing… 41 % · 1203 / 2870 frames")));
  CHECK(model.foregroundActive());
  release = true;
  REQUIRE(spin_until([&] { return model.activeCount() == 0; }, 2000ms));
  CHECK(state_of(model, 0) == QStringLiteral("done"));
  CHECK_FALSE(model.foregroundActive());
}

TEST_CASE("JobsModel: an unknown total is indeterminate", "[fast][gui][jobs]") {
  JobsModel model(1, nullptr);
  std::atomic<bool> release{false};
  model.enqueue(QStringLiteral("Scanning"), [&](JobContext& ctx) {
    ctx.report(10, 0, QStringLiteral("scanning units"));
    return wait_for(ctx, release);
  });
  REQUIRE(spin_until([&] { return model.foregroundText().contains(QStringLiteral("scanning units")); }, 2000ms));
  CHECK(model.foregroundProgress() == -1.0);
  release = true;
  REQUIRE(spin_until([&] { return model.activeCount() == 0; }, 2000ms));
}

TEST_CASE("JobsModel: cancel and cancel all stop the jobs", "[fast][gui][jobs]") {
  JobsModel model(2, nullptr);
  std::atomic<bool> never{false};
  const quint64 first = model.enqueue(QStringLiteral("A"), [&](JobContext& ctx) { return wait_for(ctx, never); });
  model.enqueue(QStringLiteral("B"), [&](JobContext& ctx) { return wait_for(ctx, never); });
  model.cancel(first);
  REQUIRE(spin_until([&] { return model.activeCount() == 1; }, 2000ms));
  CHECK(state_of(model, 0) == QStringLiteral("cancelled"));
  model.cancelAll();
  REQUIRE(spin_until([&] { return model.activeCount() == 0; }, 2000ms));
  CHECK(state_of(model, 1) == QStringLiteral("cancelled"));
}

TEST_CASE("JobsModel: a failed job keeps its error for five minutes", "[fast][gui][jobs]") {
  JobsModel model(1, nullptr);
  int finished = 0;
  QObject::connect(&model, &JobsModel::jobFinished, &model, [&](quint64, int) { ++finished; });
  model.enqueue(QStringLiteral("Failing"), [](JobContext&) { return OVT_E_IO; });
  model.enqueue(QStringLiteral("Throwing"), [](JobContext&) -> ovt_status { throw 7; });
  model.enqueue(QStringLiteral("Fine"), [](JobContext&) { return OVT_OK; });
  REQUIRE(spin_until([&] { return model.activeCount() == 0; }, 2000ms));
  REQUIRE(model.rowCount() == 3);
  CHECK(finished == 3);
  CHECK(state_of(model, 0) == QStringLiteral("failed"));
  CHECK(model.data(model.index(0), JobsModel::ErrorRole).toString() == QStringLiteral("OVT_E_IO"));
  CHECK(model.data(model.index(1), JobsModel::ErrorRole).toString() == QStringLiteral("OVT_E_INTERNAL"));
  CHECK(model.data(model.index(2), JobsModel::TextRole).toString() == QStringLiteral("Fine · done"));
  const auto now = std::chrono::steady_clock::now();
  model.prune(now + 4min);
  CHECK(model.rowCount() == 2);  // the successful job is gone after kKeepDone, the failures stay
  model.prune(now + 6min);
  CHECK(model.rowCount() == 0);
}

TEST_CASE("JobsModel: closing a file waits, then detaches what did not stop", "[fast][gui][jobs]") {
  JobsModel model(2, nullptr);
  std::vector<QString> detached;
  QObject::connect(&model, &JobsModel::jobDetached, &model, [&](const QString& name) { detached.push_back(name); });
  std::atomic<bool> stop{false};
  std::atomic<bool> started{false};
  // The first job honours the cancellation; the second ignores it until `stop`.
  std::atomic<bool> never{false};
  model.enqueue(QStringLiteral("Polite"), [&](JobContext& ctx) { return wait_for(ctx, never); }, QStringLiteral("f0"));
  model.enqueue(
      QStringLiteral("Stubborn"),
      [&](JobContext&) {
        started = true;
        while (!stop.load()) {
          std::this_thread::sleep_for(1ms);
        }
        return OVT_OK;
      },
      QStringLiteral("f0"));
  model.enqueue(
      QStringLiteral("Other file"), [&](JobContext& ctx) { return wait_for(ctx, never); }, QStringLiteral("f1"));
  REQUIRE(spin_until([&] { return started.load(); }, 2000ms));
  model.cancelOwner(QStringLiteral("f0"), 100ms);
  REQUIRE(detached.size() == 1);
  CHECK(detached.front() == QStringLiteral("Stubborn"));
  // "Polite" ended cancelled and stays listed; "Stubborn" left the list; the other file's job still runs.
  CHECK(model.rowCount() == 2);
  CHECK(model.activeCount() == 1);
  stop = true;  // the detached worker may now finish; its result is dropped
  model.cancelAll();
  REQUIRE(spin_until([&] { return model.activeCount() == 0; }, 2000ms));
}

TEST_CASE("JobsModel: a jobFinished slot may erase entries", "[fast][gui][jobs]") {
  ovt::app::testing::qt_app();
  JobsModel model(2, nullptr);
  int finished = 0;
  QObject::connect(&model, &JobsModel::jobFinished, &model, [&](quint64 /*id*/, int /*status*/) {
    ++finished;
    model.clearFinished();  // erases entries while tick() is still iterating in the old design
  });
  for (int i = 0; i < 3; ++i) {
    model.enqueue(QStringLiteral("Job"), [](JobContext& /*ctx*/) { return OVT_OK; });
  }
  REQUIRE(spin_until([&] { return finished == 3; }, 2000ms));
  CHECK(model.rowCount() == 0);
}
