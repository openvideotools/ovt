// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Test-only diagnostics for the render tests: on SIGSEGV, SIGBUS, SIGILL, SIGFPE or SIGABRT, prints the faulting
// thread's backtrace (with each frame's image and offset, for offline symbolication with atos or addr2line) to stderr,
// then hands the signal to the handler that was installed before (Catch2's fatal-condition handler, which reports the
// test and exits). It needs no debugger and no admin rights, so it captures stacks on CI runners where lldb is not
// available. Catch2 installs its handlers when each test case starts, so call install() from inside a test case
// (render tests do it in their surface helper); installing again in the same test case does nothing.

#ifndef OVT_RENDER_TESTS_CRASH_BACKTRACE_H
#define OVT_RENDER_TESTS_CRASH_BACKTRACE_H

#if defined(__APPLE__) || defined(__linux__)
#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#endif

namespace ovt::render::test {

#if defined(__APPLE__) || defined(__linux__)
namespace crash_detail {

constexpr std::array<int, 5> kSignals = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

inline std::array<struct sigaction, kSignals.size()>& previous() {
  static std::array<struct sigaction, kSignals.size()> prev{};
  return prev;
}

// Writes the first n bytes of a snprintf result (truncated to the buffer) to stderr.
inline void emit(const char* text, int n, size_t capacity) {
  const size_t len = static_cast<size_t>(n) < capacity ? static_cast<size_t>(n) : capacity - 1;
  if (write(STDERR_FILENO, text, len) < 0) {
    return;  // nothing more to do on the way to a crash
  }
}

// strsignal() is not thread-safe; the handled signals are few enough to name here.
inline const char* signal_name(int sig) {
  switch (sig) {
    case SIGSEGV:
      return "SIGSEGV";
    case SIGBUS:
      return "SIGBUS";
    case SIGILL:
      return "SIGILL";
    case SIGFPE:
      return "SIGFPE";
    case SIGABRT:
      return "SIGABRT";
    default:
      return "signal";
  }
}

inline void handler(int sig, siginfo_t* info, void* ucontext) {
  // Diagnostics on the way to a crash: snprintf/dladdr are not async-signal-safe, which is acceptable here.
  std::array<char, 256> line{};
  int n = std::snprintf(line.data(), line.size(), "\n==ovt-crash== signal %d (%s) at address %p\n", sig,
                        signal_name(sig), info != nullptr ? info->si_addr : nullptr);
  if (n > 0) {
    emit(line.data(), n, line.size());
  }
  std::array<void*, 64> frames{};
  const int count = backtrace(frames.data(), static_cast<int>(frames.size()));
  for (int i = 0; i < count; ++i) {
    Dl_info dl{};
    void* pc = frames[static_cast<size_t>(i)];
    if (dladdr(pc, &dl) != 0 && dl.dli_fname != nullptr) {
      const auto off = reinterpret_cast<uintptr_t>(pc) - reinterpret_cast<uintptr_t>(dl.dli_fbase);  // NOLINT
      n = std::snprintf(line.data(), line.size(), "==ovt-crash== #%02d %p %s+0x%lx %s\n", i, pc, dl.dli_fname,
                        static_cast<unsigned long>(off),  // NOLINT(google-runtime-int)
                        dl.dli_sname != nullptr ? dl.dli_sname : "?");
    } else {
      n = std::snprintf(line.data(), line.size(), "==ovt-crash== #%02d %p ?\n", i, pc);
    }
    if (n > 0) {
      emit(line.data(), n, line.size());
    }
  }
  // Hand over to the previous handler (normally Catch2's), as if this one had never been installed.
  for (size_t i = 0; i < kSignals.size(); ++i) {
    if (kSignals[i] != sig) {
      continue;
    }
    const struct sigaction& prev = previous()[i];
    if ((prev.sa_flags & SA_SIGINFO) != 0 && prev.sa_sigaction != nullptr) {
      prev.sa_sigaction(sig, info, ucontext);
      return;
    }
    if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN && prev.sa_handler != nullptr) {
      prev.sa_handler(sig);
      return;
    }
    static_cast<void>(sigaction(sig, &prev, nullptr));
    static_cast<void>(raise(sig));
    return;
  }
}

}  // namespace crash_detail

inline void install_crash_backtrace() {
  for (size_t i = 0; i < crash_detail::kSignals.size(); ++i) {
    struct sigaction current{};
    if (sigaction(crash_detail::kSignals[i], nullptr, &current) != 0) {
      continue;
    }
    if ((current.sa_flags & SA_SIGINFO) != 0 && current.sa_sigaction == &crash_detail::handler) {
      continue;  // already installed in this test case
    }
    struct sigaction act{};
    act.sa_sigaction = &crash_detail::handler;
    act.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&act.sa_mask);
    if (sigaction(crash_detail::kSignals[i], &act, &crash_detail::previous()[i]) != 0) {
      crash_detail::previous()[i] = current;
    }
  }
}
#else
inline void install_crash_backtrace() {}
#endif

}  // namespace ovt::render::test

#endif  // OVT_RENDER_TESTS_CRASH_BACKTRACE_H
