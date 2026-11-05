// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// ovt/log.h entry points (chapter 12 §12.3.4, WI-1.512): the C logger is an ovt::log sink.
// SPEC-ASSUMPTION(#69): NULL restores the silent default; a diagnostic code starts msg as "[CODE] ".

#include <string>
#include <string_view>

#include "ovt/cxx/log.hpp"
#include "ovt/log.h"

namespace {

void invoke(ovt_log_fn fn, int level, const char* module, const char* msg, void* user) noexcept {
  fn(level, module, msg, user);  // MUST NOT throw (§12.3.1 rule 8); a throw terminates here
}

// Adapts one ovt::log line to the C callback. It runs inside the noexcept sink call, so running out of memory
// while building the strings drops the line instead of terminating (§12.3.1 rule 8).
void deliver(ovt_log_fn fn, void* user, ovt::log::Level l, std::string_view module, std::string_view code,
             std::string_view message) noexcept {
  std::string text;
  std::string mod;
  try {
    if (!code.empty()) {
      text.reserve(code.size() + 3 + message.size());
      text.append("[").append(code).append("] ");
    }
    text.append(message);
    mod.assign(module);
  } catch (...) {  // NOLINT(bugprone-empty-catch): std::bad_alloc, the line is dropped
    return;
  }
  invoke(fn, static_cast<int>(l), mod.c_str(), text.c_str(), user);
}

}  // namespace

extern "C" {

OVT_API void ovt_set_logger(ovt_log_fn fn, void* user) {
  try {
    if (fn == nullptr) {
      ovt::log::set_sink({});
      return;
    }
    ovt::log::set_sink([fn, user](ovt::log::Level l, std::string_view module, std::string_view code,
                                  std::string_view message) { deliver(fn, user, l, module, code, message); });
  } catch (...) {  // NOLINT(bugprone-empty-catch): out of memory; the previous logger stays installed
  }
}

OVT_API void ovt_set_log_level(int level) {
  ovt::log::set_level(ovt::log::detail::to_level(level));
}

}  // extern "C"
