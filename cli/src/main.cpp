// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal ovt entry point of WI-0.000: prints the version (FR-GEN-010 format) and nothing else.
// Exit codes follow chapter 12 §12.1.3 (0 OK, 2 USAGE).

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>

#include "ovt/ovt.h"
#include "ovt_version_full.h"  // generated: OVT_VERSION_FULL = <VERSION>+<git sha>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;

void print_usage(std::FILE* out) {
  static_cast<void>(std::fprintf(out,
                                 "Usage: %s [--version | -V] [--help | -h]\n"
                                 "\n"
                                 "  -V, --version  print the version and exit\n"
                                 "  -h, --help     print this help and exit\n",
                                 OVT_CLI_NAME_VALUE));  // best effort: nothing to do if stdout is closed
}

}  // namespace

int main(int argc, char** argv) {
  // argv as a span: no raw pointer arithmetic (cppcoreguidelines-pro-bounds-pointer-arithmetic).
  const std::span<char*> args(argv, static_cast<std::size_t>(argc));
  const auto argc_n = args.size();
  if (argc_n == 2 && (std::strcmp(args[1], "--version") == 0 || std::strcmp(args[1], "-V") == 0)) {
    std::printf("%s %s (core %s, api %d.%d.%d)\n", OVT_CLI_NAME_VALUE, OVT_VERSION_FULL, ovt_version_string(),
                OVT_API_VERSION_MAJOR, OVT_API_VERSION_MINOR, OVT_API_VERSION_PATCH);
    return kExitOk;
  }
  if (argc_n == 2 && (std::strcmp(args[1], "--help") == 0 || std::strcmp(args[1], "-h") == 0)) {
    print_usage(stdout);
    return kExitOk;
  }
  if (argc_n >= 2) {
    static_cast<void>(std::fprintf(stderr, "error: CLI-0001 unknown option '%s' for '%s'. See '%s --help'.\n", args[1],
                                   OVT_CLI_NAME_VALUE, OVT_CLI_NAME_VALUE));
  } else {
    print_usage(stderr);
  }
  return kExitUsage;
}
