// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A QCoreApplication for the model tests (timers and queued signals need an event loop), created once.

#ifndef OVT_APP_TESTS_QT_TEST_APP_H
#define OVT_APP_TESTS_QT_TEST_APP_H

#include <QCoreApplication>
#include <QThread>

#include <array>
#include <chrono>
#include <functional>

namespace ovt::app::testing {

inline QCoreApplication& qt_app() {
  static int argc = 1;
  static std::array<char, 21> name{"ovt_tests_app_models"};
  static std::array<char*, 2> argv{name.data(), nullptr};
  static QCoreApplication app(argc, argv.data());
  return app;
}

// Created before main(), so that every test (also one run alone) has an application when it builds a model.
inline QCoreApplication& const_app = qt_app();

// Runs the event loop until `done()` or `limit` passes; true when `done()` became true.
inline bool spin_until(const std::function<bool()>& done, std::chrono::milliseconds limit) {
  qt_app();
  const auto end = std::chrono::steady_clock::now() + limit;
  while (!done()) {
    if (std::chrono::steady_clock::now() >= end) {
      return false;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(1);
  }
  return true;
}

}  // namespace ovt::app::testing

#endif  // OVT_APP_TESTS_QT_TEST_APP_H
