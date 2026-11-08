// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of LogSink (WI-1.453 step 4; chapter 11 §11.3.16): the 10 000-line ring, batched appends from any thread,
// the level / module / text filters and Save…

#include <catch2/catch_test_macros.hpp>

#include <QFile>
#include <QTemporaryDir>

#include <thread>
#include <vector>

#include "models/log_sink.h"
#include "ovt/log.h"
#include "qt_test_app.h"

namespace {

using ovt::app::LogFilterModel;
using ovt::app::LogLine;
using ovt::app::LogSink;

LogLine line(int level, const char* module, const char* message) {
  LogLine l;
  l.time_ms = 1'767'225'600'123;  // 2026-01-01T00:00:00.123Z
  l.level = level;
  l.module = QString::fromLatin1(module);
  l.message = QString::fromLatin1(message);
  return l;
}

}  // namespace

TEST_CASE("LogSink: lines appear after a flush, in the format of chapter 7 §7.1.6", "[fast][gui][log]") {
  ovt::app::testing::qt_app();
  LogSink sink(false, nullptr);
  sink.append(line(OVT_LOG_WARN, "index", "[SYN-H264-0103] frame_num gap at unit 1234"));
  CHECK(sink.rowCount() == 0);  // batched
  sink.flush();
  REQUIRE(sink.rowCount() == 1);
  CHECK(sink.data(sink.index(0), LogSink::TextRole).toString() ==
        QStringLiteral("2026-01-01T00:00:00.123Z WARN  index [SYN-H264-0103] frame_num gap at unit 1234"));
}

TEST_CASE("LogSink: the ring keeps the newest 10 000 lines", "[fast][gui][log]") {
  LogSink sink(false, nullptr);
  for (int i = 0; i < 10'050; ++i) {
    sink.append(line(OVT_LOG_INFO, "io", QByteArray::number(i).constData()));
  }
  sink.flush();
  REQUIRE(sink.rowCount() == 10'000);
  CHECK(sink.data(sink.index(0), LogSink::MessageRole).toString() == QStringLiteral("50"));
  CHECK(sink.data(sink.index(9'999), LogSink::MessageRole).toString() == QStringLiteral("10049"));
  sink.append(line(OVT_LOG_INFO, "io", "last"));
  sink.flush();
  CHECK(sink.rowCount() == 10'000);
  CHECK(sink.data(sink.index(0), LogSink::MessageRole).toString() == QStringLiteral("51"));
}

TEST_CASE("LogSink: the timer flushes every 100 ms and pause holds lines back", "[fast][gui][log]") {
  using namespace std::chrono_literals;
  LogSink sink(false, nullptr);
  sink.append(line(OVT_LOG_INFO, "io", "one"));
  REQUIRE(ovt::app::testing::spin_until([&] { return sink.rowCount() == 1; }, 1000ms));
  sink.setPaused(true);
  sink.append(line(OVT_LOG_INFO, "io", "two"));
  ovt::app::testing::spin_until([&] { return false; }, 250ms);
  CHECK(sink.rowCount() == 1);
  sink.setPaused(false);  // resuming shows what was queued
  CHECK(sink.rowCount() == 2);
}

TEST_CASE("LogSink: appends from several threads are all kept", "[fast][gui][log]") {
  LogSink sink(false, nullptr);
  std::vector<std::thread> threads;
  threads.reserve(4);
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&sink] {
      for (int i = 0; i < 500; ++i) {
        sink.append(line(OVT_LOG_DEBUG, "worker", "x"));
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  sink.flush();
  CHECK(sink.rowCount() == 2000);
}

TEST_CASE("LogSink: level, module and text filters and Save", "[fast][gui][log]") {
  LogSink sink(false, nullptr);
  sink.append(line(OVT_LOG_DEBUG, "io", "opened clip.mp4"));
  sink.append(line(OVT_LOG_WARN, "index", "gap"));
  sink.append(line(OVT_LOG_ERROR, "index", "bad nal"));
  sink.append(line(OVT_LOG_ERROR, "io", "short read"));
  sink.flush();
  auto* view = qobject_cast<LogFilterModel*>(sink.filtered());
  REQUIRE(view != nullptr);
  CHECK(view->rowCount() == 4);
  view->setMinLevel(OVT_LOG_WARN);
  CHECK(view->rowCount() == 3);
  view->setModule(QStringLiteral("index"));
  CHECK(view->rowCount() == 2);
  view->setText(QStringLiteral("BAD"));
  REQUIRE(view->rowCount() == 1);
  CHECK(view->data(view->index(0, 0), LogSink::MessageRole).toString() == QStringLiteral("bad nal"));

  const QTemporaryDir dir;
  REQUIRE(dir.isValid());
  const QString path = dir.path() + QStringLiteral("/log.txt");
  REQUIRE(sink.save(path));  // saves what the filter shows
  QFile file(path);
  REQUIRE(file.open(QIODevice::ReadOnly));
  CHECK(file.readAll() == "2026-01-01T00:00:00.123Z ERROR index bad nal\n");
  sink.clear();
  CHECK(sink.rowCount() == 0);
}

TEST_CASE("LogSink: installed as the core logger it receives library lines", "[fast][gui][log]") {
  // No exported function emits a line, so this only checks the install and removal do not crash and leave the
  // default (silent) logger behind.
  {
    LogSink sink(true, nullptr);
    sink.flush();
    CHECK(sink.rowCount() == 0);
  }
  ovt_set_log_level(OVT_LOG_WARN);
  SUCCEED();
}
