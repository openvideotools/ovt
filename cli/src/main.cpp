// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ovt entry point (chapter 12 §12.1, WI-1.500). Everything except the process environment lives in
// ovt_cli_lib (app.cpp), so tests run the same code in-process.

#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <vector>

#include "app.h"

namespace {

int run_main(int argc, char** argv) {
  const std::span<char*> argv_span(argv, static_cast<std::size_t>(argc));
  const std::vector<std::string> args(argv_span.begin() + (argc > 0 ? 1 : 0), argv_span.end());
  ovt::cli::install_signal_handlers();
  ovt::cli::Context ctx;
  ctx.stdout_tty = isatty(STDOUT_FILENO) == 1;
  ctx.stderr_tty = isatty(STDERR_FILENO) == 1;
  return ovt::cli::run(args, ctx);
}

}  // namespace

// Nothing may escape main (bugprone-exception-escape): allocation failures while building the arguments, or
// anything thrown by the CLI library, end the process with a diagnostic and exit status 1 instead of
// std::terminate. 1 is the generic-failure code of chapter 12.
int main(int argc, char** argv) {
  try {
    return run_main(argc, argv);
  } catch (const std::exception& e) {
    (void)std::fprintf(stderr, "ovt: internal error: %s\n", e.what());
  } catch (...) {
    (void)std::fputs("ovt: internal error: unknown exception\n", stderr);
  }
  return 1;
}
