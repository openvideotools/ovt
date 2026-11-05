// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Replay driver of the fuzz targets (chapter 14 §14.7.3, WI-0.557): main() of ovt_fuzz_<name>_regress. Each
// argument is a file or a directory (its regular files, not recursive, dot files skipped, sorted); every file
// is passed once to LLVMFuzzerTestOneInput. No libFuzzer needed, so it runs on every compiler. A crash is a
// sanitizer report or abort; a missing path is an error (exit 2); a run with no inputs at all is fine.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

namespace {

namespace fs = std::filesystem;

bool collect(const fs::path& arg, std::vector<fs::path>& out) {
  std::error_code ec;
  if (fs::is_regular_file(arg, ec)) {
    out.push_back(arg);
    return true;
  }
  if (!fs::is_directory(arg, ec)) {
    return false;
  }
  std::vector<fs::path> found;
  for (const auto& entry : fs::directory_iterator(arg, ec)) {
    if (entry.is_regular_file() && !entry.path().filename().string().starts_with('.')) {
      found.push_back(entry.path());
    }
  }
  std::sort(found.begin(), found.end());
  out.insert(out.end(), found.begin(), found.end());
  return !ec;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<fs::path> inputs;
  for (int i = 1; i < argc; ++i) {
    if (!collect(argv[i], inputs)) {
      std::fprintf(stderr, "fuzz regress: cannot read '%s'\n", argv[i]);
      return 2;
    }
  }
  for (const auto& path : inputs) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
      std::fprintf(stderr, "fuzz regress: cannot read '%s'\n", path.string().c_str());
      return 2;
    }
    (void)LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  }
  std::printf("fuzz regress: %zu inputs replayed\n", inputs.size());
  return 0;
}
