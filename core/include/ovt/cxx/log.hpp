// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt::log, the C++ logging front-end of libovt-core (chapter 7 §7.1.6, chapter 12 §12.3.4; WI-1.512).
// C++ helper header (D22): no ABI promise, never included from a C header, not installed.
//
// The library is silent until a sink is installed (chapter 04 FR-API-006): the default sink drops every
// line. ovt_set_logger() and set_sink() fill the same slot. stderr_sink() is the formatted sink of
// §7.1.6 that the CLI installs (it writes to stderr, or to --log-file).
// SPEC-ASSUMPTION(#69): silent default; Level gains Fatal and Off; OVT_LOG_CODE carries the code.
//
// Log lines are not deterministic output and never appear on stdout (§7.1.6 rule 4).

#ifndef OVT_CXX_LOG_HPP
#define OVT_CXX_LOG_HPP

#include <fmt/format.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ovt::log {

// Values equal the C levels of ovt_log_fn. Chapter 7 texts "level 1/3" refer to Debug/Warn. Fatal and Off
// mirror the C API and the CLI's --log-level (chapter 12 §12.1.2); Off is a threshold only.
enum class Level : uint8_t { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4, Fatal = 5, Off = 6 };

using Sink = std::function<void(Level, std::string_view module, std::string_view code /* may be empty */,
                                std::string_view message)>;

// Threshold: lines below it are not formatted at all. Default Warn. Thread-safe.
void set_level(Level level) noexcept;
[[nodiscard]] Level level() noexcept;
[[nodiscard]] inline bool enabled(Level l) noexcept {
  return l != Level::Off && static_cast<uint8_t>(l) >= static_cast<uint8_t>(level());
}

// Installs the sink; an empty function restores the silent default. Call before any other API use
// (chapter 12 §12.3.1 rule 4); a concurrent call is still safe, a line in flight goes to the old sink.
// The sink MAY be invoked from any thread, concurrently, and MUST NOT throw or log.
void set_sink(Sink sink);

// Sends one line to the sink if `l` is enabled. Never throws: a sink that throws terminates (rule 8).
void write(Level l, std::string_view module, std::string_view code, std::string_view message) noexcept;

// The default line format of §7.1.6, without a trailing newline:
//   <ISO-8601 UTC time with ms> <LEVEL 5-wide> <module> [<diagnostic code>] <message>
//   2026-11-03T10:15:42.117Z WARN  index [SYN-H264-0103] frame_num gap at unit 1234
// The " [<code>]" part is omitted when the code is empty.
[[nodiscard]] std::string format_line(Level l, std::string_view module, std::string_view code, std::string_view message,
                                      std::chrono::system_clock::time_point when);
// A sink that writes format_line() plus '\n' to `out` (stderr when null), one line per fwrite under a lock.
// `out` is not owned and must outlive the sink.
[[nodiscard]] Sink stderr_sink(std::FILE* out = nullptr);

// "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF".
[[nodiscard]] std::string_view level_name(Level l) noexcept;

namespace detail {

constexpr Level to_level(Level l) noexcept {
  return l;
}
// OVT_LOG(2, ...) is level 2 (Info), as the chapter 7 texts write it. Out-of-range values clamp.
template <class I, std::enable_if_t<std::is_integral_v<I>, int> = 0>
constexpr Level to_level(I i) noexcept {
  return i <= 0 ? Level::Trace : (i >= 6 ? Level::Off : static_cast<Level>(i));
}

// Whether a level survives the build's OVT_LOG_MIN_LEVEL (a function, so the comparison never trips
// -Wtype-limits when the minimum is 0).
constexpr bool compiled_in(Level l, int min_level) noexcept {
  return static_cast<int>(l) >= min_level;
}

template <class... Args>
void write_fmt(Level l, std::string_view module, std::string_view code, fmt::format_string<Args...> f,
               Args&&... args) noexcept {
  try {
    write(l, module, code, fmt::format(f, std::forward<Args>(args)...));
  } catch (...) {  // NOLINT(bugprone-empty-catch): formatting failed (bad_alloc); a log line is not worth more
  }
}

}  // namespace detail

}  // namespace ovt::log

// Build-wide minimum level; lines below it are compiled out. 0 (Trace) keeps everything.
#ifndef OVT_LOG_MIN_LEVEL
#define OVT_LOG_MIN_LEVEL 0
#endif

// OVT_LOG(level, module, fmt, args...): fmt-style, checked at compile time; `level` is an ovt::log::Level or
// an integer 0..5. The arguments are not evaluated when the level is disabled.
#define OVT_LOG(level, module, ...) OVT_LOG_CODE(level, module, std::string_view{}, __VA_ARGS__)

// Same, with a diagnostic code (`SYN-H264-0103`) shown in brackets by the default format.
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define OVT_LOG_CODE(level, module, code, ...)                                          \
  do {                                                                                  \
    constexpr ::ovt::log::Level ovt_log_level_ = ::ovt::log::detail::to_level(level);   \
    if constexpr (::ovt::log::detail::compiled_in(ovt_log_level_, OVT_LOG_MIN_LEVEL)) { \
      if (::ovt::log::enabled(ovt_log_level_)) {                                        \
        ::ovt::log::detail::write_fmt(ovt_log_level_, (module), (code), __VA_ARGS__);   \
      }                                                                                 \
    }                                                                                   \
  } while (false)
// NOLINTEND(cppcoreguidelines-macro-usage)

#endif  // OVT_CXX_LOG_HPP
