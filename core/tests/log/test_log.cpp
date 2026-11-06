// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::log, OVT_LOG and ovt/log.h (chapter 7 §7.1.6, chapter 12 §12.3.4, FR-API-006; WI-1.512), and the
// ovt/status.h strings.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "ovt/cxx/log.hpp"
#include "ovt/log.h"
#include "ovt/status.h"

namespace {

using ovt::log::Level;

struct Line {
  Level level;
  std::string module, code, message;
  bool operator==(const Line&) const = default;
};

// Installs a capturing sink for the scope of a test and restores the silent default and level afterwards.
struct Capture {
  std::vector<Line> lines;
  Capture() {
    ovt::log::set_sink([this](Level l, std::string_view m, std::string_view c, std::string_view msg) {
      lines.push_back({l, std::string(m), std::string(c), std::string(msg)});
    });
  }
  Capture(const Capture&) = delete;
  Capture& operator=(const Capture&) = delete;
  Capture(Capture&&) = delete;
  Capture& operator=(Capture&&) = delete;
  ~Capture() {
    ovt::log::set_sink({});
    ovt::log::set_level(Level::Warn);
  }
};

struct CLine {
  int level;
  std::string module, msg;
  void* user;
  bool operator==(const CLine&) const = default;
};
std::vector<CLine> g_c_lines;

void c_logger(int level, const char* module, const char* msg, void* user) {
  g_c_lines.push_back({level, module, msg, user});
}

// Logs two lines through c_logger, then one more after restoring the silent default.
void log_through_c_logger(void* user) {
  g_c_lines.clear();
  ovt_set_logger(&c_logger, user);
  OVT_LOG(Level::Warn, "io", "a {}", 1);
  OVT_LOG_CODE(Level::Error, "index", "SYN-H264-0103", "gap");
  ovt_set_logger(nullptr, nullptr);  // back to silent
  OVT_LOG(Level::Error, "io", "dropped");
}

}  // namespace

TEST_CASE("default level is warn", "[log][fast]") {
  CHECK(ovt::log::level() == Level::Warn);
  CHECK(ovt::log::enabled(Level::Warn));
  CHECK_FALSE(ovt::log::enabled(Level::Info));
}

TEST_CASE("lines below the level are not formatted", "[log][fast]") {
  const Capture cap;
  int evaluated = 0;
  OVT_LOG(Level::Info, "test", "not shown {}", ++evaluated);
  OVT_LOG(2, "test", "not shown either {}", ++evaluated);
  CHECK(evaluated == 0);
  CHECK(cap.lines.empty());
}

TEST_CASE("OVT_LOG formats with fmt and OVT_LOG_CODE carries the code", "[log][fast]") {
  const Capture cap;
  OVT_LOG(Level::Warn, "test", "shown {} {}", 1, "two");
  OVT_LOG_CODE(4, "index", "SYN-H264-0103", "frame_num gap at unit {}", 1234);
  REQUIRE(cap.lines.size() == 2);
  CHECK(cap.lines[0] == Line{Level::Warn, "test", "", "shown 1 two"});
  CHECK(cap.lines[1] == Line{Level::Error, "index", "SYN-H264-0103", "frame_num gap at unit 1234"});
}

TEST_CASE("Trace shows everything and Off drops even fatal", "[log][fast]") {
  const Capture cap;
  ovt::log::set_level(Level::Trace);
  OVT_LOG(0, "test", "trace");
  ovt::log::set_level(Level::Off);
  OVT_LOG(Level::Fatal, "test", "dropped");
  CHECK(cap.lines.size() == 1);
}

TEST_CASE("no sink: the library is silent", "[log][fast]") {
  ovt::log::set_sink({});
  ovt::log::set_level(Level::Trace);
  // Nothing to observe but the absence of a crash: the default sink drops the line (FR-API-006).
  OVT_LOG(Level::Error, "test", "dropped");
  ovt::log::write(Level::Error, "test", "", "dropped");
  ovt::log::set_level(Level::Warn);
  SUCCEED();
}

TEST_CASE("default line format", "[log][fast]") {
  // 2026-11-03T10:15:42.117Z
  const auto when = std::chrono::system_clock::time_point(std::chrono::milliseconds(1793700942117LL));
  CHECK(ovt::log::format_line(Level::Warn, "index", "SYN-H264-0103", "frame_num gap at unit 1234", when) ==
        "2026-11-03T10:15:42.117Z WARN  index [SYN-H264-0103] frame_num gap at unit 1234");
  CHECK(ovt::log::format_line(Level::Info, "io", "", "mmap failed (x), using pread", when) ==
        "2026-11-03T10:15:42.117Z INFO  io mmap failed (x), using pread");
  CHECK(ovt::log::format_line(Level::Error, "jobs", "", "m", when).substr(25, 6) == "ERROR ");
}

TEST_CASE("stderr_sink writes one formatted line", "[log][fast]") {
  std::FILE* f = std::tmpfile();
  REQUIRE(f != nullptr);
  const ovt::log::Sink sink = ovt::log::stderr_sink(f);
  sink(Level::Warn, "io", "IO-0001", "hello");
  std::rewind(f);
  std::array<char, 256> buf{};
  REQUIRE(std::fgets(buf.data(), static_cast<int>(buf.size()), f) != nullptr);
  std::fclose(f);
  const std::string line(buf.data());
  CHECK(line.size() > 25);
  CHECK(line.substr(23) == "Z WARN  io [IO-0001] hello\n");
}

TEST_CASE("ovt_set_logger receives C levels and the code in the message", "[log][fast]") {
  int user = 0;
  log_through_c_logger(&user);
  REQUIRE(g_c_lines.size() == 2);
  CHECK(g_c_lines[0] == CLine{3, "io", "a 1", &user});
  CHECK(g_c_lines[1] == CLine{4, "index", "[SYN-H264-0103] gap", &user});
}

TEST_CASE("ovt_set_log_level sets and clamps the threshold", "[log][fast]") {
  ovt_set_log_level(OVT_LOG_DEBUG);
  CHECK(ovt::log::level() == Level::Debug);
  ovt_set_log_level(-3);
  CHECK(ovt::log::level() == Level::Trace);
  ovt_set_log_level(99);
  CHECK(ovt::log::level() == Level::Off);
  ovt_set_log_level(OVT_LOG_WARN);
  CHECK(ovt::log::level() == Level::Warn);
}

TEST_CASE("ovt_status_string", "[log][fast]") {
  CHECK(std::string(ovt_status_string(OVT_OK)) == "OVT_OK");
  CHECK(std::string(ovt_status_string(OVT_E_CANCELLED)) == "OVT_E_CANCELLED");
  CHECK(std::string(ovt_status_string(OVT_E_INTERNAL)) == "OVT_E_INTERNAL");
}
