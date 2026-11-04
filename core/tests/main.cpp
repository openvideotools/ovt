// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// main() of the golden runner ovt_core_tests (chapter 14 §14.2.1, WI-0.550). It is the only test
// executable with its own main; every other links Catch2::Catch2WithMain (D24). Before the Catch2 session
// starts it reads the test environment (OVT_UPDATE_GOLDEN, OVT_REQUIRE_CORPORA, OVT_CORPORA_DIR) and
// installs the ovt_set_logger hook that turns library log lines into Catch2 INFO context.

#include <catch2/catch_message.hpp>
#include <catch2/catch_session.hpp>

#include <cstdio>
#include <string>
#include <thread>

#include "test_support/corpus.h"
#include "test_support/golden.h"

#if __has_include("ovt/log.h")
#include "ovt/log.h"
#define OVT_TEST_HAVE_LOGGER 1
#else
// ovt/log.h arrives with WI-1.512 (M0.2); until then the hook is not installed.
#define OVT_TEST_HAVE_LOGGER 0
#endif

#if OVT_TEST_HAVE_LOGGER
namespace {
std::thread::id g_test_thread;

// Log lines become part of the INFO context of the running test case, so a failing assertion shows
// what the library logged before it (chapter 12 §12.3.4 signature). The logger may run on any thread;
// Catch2 macros are only safe on the test thread, so lines from other threads go to stderr instead.
void catch2_logger(int level, const char* module, const char* msg, void* /*user*/) {
  const std::string line = "[ovt " + std::to_string(level) + " " + (module != nullptr ? module : "-") + "] " +
                           (msg != nullptr ? msg : "");
  if (std::this_thread::get_id() == g_test_thread) {
    UNSCOPED_INFO(line);
  } else {
    std::fprintf(stderr, "%s\n", line.c_str());
  }
}
}  // namespace
#endif

int main(int argc, char* argv[]) {
  // Read once, before any test runs, so a test cannot change the mode half way.
  ovt::test::set_update_golden(ovt::test::update_golden());

#if OVT_TEST_HAVE_LOGGER
  g_test_thread = std::this_thread::get_id();
  ovt_set_logger(&catch2_logger, nullptr);
#endif

  Catch::Session session;
  const int rc = session.applyCommandLine(argc, argv);
  if (rc != 0) {
    return rc;
  }
  if (ovt::test::update_golden()) {
    std::fprintf(stderr, "ovt_core_tests: OVT_UPDATE_GOLDEN=1, golden files are rewritten\n");
  }
  if (ovt::test::require_corpora()) {
    std::fprintf(stderr, "ovt_core_tests: OVT_REQUIRE_CORPORA=1, missing corpus files fail (corpora: %s)\n",
                 ovt::test::corpora_dir().string().c_str());
  }
  return session.run();
}
