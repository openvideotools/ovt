// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::log (chapter 7 §7.1.6, WI-1.512): level threshold, the sink slot shared with ovt_set_logger, and the
// default line format. SPEC-ASSUMPTION(#69): the default sink is silent (chapter 04 FR-API-006).

#include "ovt/cxx/log.hpp"

#include <array>
#include <atomic>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace ovt::log {

namespace {

std::atomic<uint8_t> g_level{static_cast<uint8_t>(Level::Warn)};

struct SinkSlot {
  std::mutex m;  // held only to copy or swap the pointer, never while the sink runs
  std::shared_ptr<const Sink> sink;
};

// Never destroyed: Pool::global() and other static destructors may still log at process exit.
SinkSlot& slot() {
  static auto* s = new SinkSlot();  // NOLINT(cppcoreguidelines-owning-memory)
  return *s;
}

void call_sink(const Sink& sink, Level l, std::string_view module, std::string_view code,
               std::string_view message) noexcept {
  sink(l, module, code, message);  // a throwing sink terminates (chapter 12 §12.3.1 rule 8)
}

}  // namespace

void set_level(Level level) noexcept {
  g_level.store(static_cast<uint8_t>(level), std::memory_order_relaxed);
}

Level level() noexcept {
  return static_cast<Level>(g_level.load(std::memory_order_relaxed));
}

void set_sink(Sink sink) {
  std::shared_ptr<const Sink> next;
  if (sink) {
    next = std::make_shared<const Sink>(std::move(sink));
  }
  SinkSlot& s = slot();
  const std::lock_guard<std::mutex> lock(s.m);
  s.sink.swap(next);
  // the previous sink is released after the lock, when `next` goes out of scope
}

void write(Level l, std::string_view module, std::string_view code, std::string_view message) noexcept {
  if (!enabled(l)) {
    return;
  }
  std::shared_ptr<const Sink> sink;
  {
    SinkSlot& s = slot();
    const std::lock_guard<std::mutex> lock(s.m);
    sink = s.sink;
  }
  if (sink) {
    call_sink(*sink, l, module, code, message);
  }
}

std::string_view level_name(Level l) noexcept {
  switch (l) {
    case Level::Trace:
      return "TRACE";
    case Level::Debug:
      return "DEBUG";
    case Level::Info:
      return "INFO";
    case Level::Warn:
      return "WARN";
    case Level::Error:
      return "ERROR";
    case Level::Fatal:
      return "FATAL";
    case Level::Off:
      return "OFF";
  }
  return "?";
}

std::string format_line(Level l, std::string_view module, std::string_view code, std::string_view message,
                        std::chrono::system_clock::time_point when) {
  const auto since_epoch = when.time_since_epoch();
  const auto secs = std::chrono::floor<std::chrono::seconds>(since_epoch);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch - secs).count();
  const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::time_point(secs));
  std::tm utc{};
  gmtime_r(&t, &utc);
  std::array<char, 32> stamp{};
  const size_t n = std::strftime(stamp.data(), stamp.size(), "%Y-%m-%dT%H:%M:%S", &utc);
  if (code.empty()) {
    return fmt::format("{}.{:03}Z {:<5} {} {}", std::string_view(stamp.data(), n), ms, level_name(l), module, message);
  }
  return fmt::format("{}.{:03}Z {:<5} {} [{}] {}", std::string_view(stamp.data(), n), ms, level_name(l), module, code,
                     message);
}

Sink stderr_sink(std::FILE* out) {
  auto lock = std::make_shared<std::mutex>();
  return [out, lock](Level l, std::string_view module, std::string_view code, std::string_view message) {
    std::string line;
    try {
      line = format_line(l, module, code, message, std::chrono::system_clock::now());
      line.push_back('\n');
    } catch (...) {  // NOLINT(bugprone-empty-catch): std::bad_alloc inside a noexcept sink call; drop the line
      return;
    }
    std::FILE* f = out != nullptr ? out : stderr;
    const std::lock_guard<std::mutex> guard(*lock);
    static_cast<void>(std::fwrite(line.data(), 1, line.size(), f));  // nowhere to report a failed log write
  };
}

}  // namespace ovt::log
