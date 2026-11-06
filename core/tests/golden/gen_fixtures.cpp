// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// gen_fixtures <out-dir>: writes the WI-0.093 fixture set. The checked-in files in core/tests/golden/ are
// regenerated with it and never edited by hand:
//
//   build/<preset>/core/tests/golden/gen_fixtures core/tests/golden
//
// Output: tiny16_hevc.fa with a `.sha256` next to it (sha256sum format) and tiny16.yuv (its picture).
// WI-0.093b adds tiny32_h264.fa, tiny64_av1.fa and tiny64_av1.inspect.json.
// Exit codes: 0 written, 1 usage or I/O error.

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "fixtures.hpp"
#include "sha256.hpp"

namespace {

bool write_file(const std::filesystem::path& path, std::string_view content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  return out.good();
}

bool write_bytes(const std::filesystem::path& path, const ovt::golden::Bytes& bytes) {
  return write_file(path, {reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}

}  // namespace

int run(int argc, char** argv) {
  if (argc != 2) {
    std::fputs("usage: gen_fixtures <out-dir>\n", stderr);
    return 1;
  }
  const std::filesystem::path dir = argv[1];
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    std::fprintf(stderr, "gen_fixtures: cannot create %s: %s\n", dir.string().c_str(), ec.message().c_str());
    return 1;
  }
  bool ok = true;
  for (const auto& fixture : ovt::golden::kFixtures) {
    const ovt::golden::Bytes bytes = fixture.build();
    const std::string name = std::string(fixture.stem) + ".fa";
    ok = write_bytes(dir / name, bytes) && ok;
    ok = write_file(dir / (name + ".sha256"), ovt::golden::sha256_hex(bytes) + "  " + name + "\n") && ok;
  }
  ok = write_bytes(dir / "tiny16.yuv", ovt::golden::build_tiny16_yuv()) && ok;
  if (!ok) {
    std::fputs("gen_fixtures: write failed\n", stderr);
    return 1;
  }
  return 0;
}

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "gen_fixtures: %s\n", e.what());
    return 1;
  }
}
