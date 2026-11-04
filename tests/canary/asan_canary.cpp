// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// test_asan_canary (WI-0.004): a deliberate heap-buffer overflow. Built only for the asan-ubsan
// sanitizer set and labelled `canary`, which every lane preset excludes. Under ASan this program
// aborts with a heap-buffer-overflow report, so the test fails; if it ever passes, ASan is not active.

#include <cstdio>
#include <cstdlib>
#include <memory>

int main(int argc, char** /*argv*/) {
  // The index depends on argc so the compiler cannot prove the overflow at compile time.
  const auto size = static_cast<std::size_t>(8 + argc - 1);
  auto buffer = std::make_unique<unsigned char[]>(size);
  volatile unsigned char* p = buffer.get();
  p[size] = 1;  // one past the end: heap-buffer-overflow
  std::puts("asan canary: overflow was not detected; ASan is not active");
  return EXIT_SUCCESS;
}
