// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// bench_container_open_moov_1h: ovt_container_open + ovt_container_tree of a one-hour, `moov`-first MP4 with a
// multi-GiB `mdat` (chapter 14 §14.9.3, chapter 08 §8.2.4, WI-1.204). The input is the corpus file
// `syn_h264_1h_1080p.mp4` (recipe 27) when it is in $OVT_CORPORA_DIR; otherwise a sparse stand-in of the same shape:
// `ftyp`, a `moov` whose sample tables have the recipe's 108 000 entries, and a 6 GiB `mdat` that is never written.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "ovt/container.h"
#include "ovt/io.h"
#include "ovt_perf.hpp"

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

constexpr uint32_t kFrames = 108000;  // recipe 27: one hour at 30 fps
constexpr uint64_t kMdatBytes = 6ULL << 30U;

void put32(Bytes& b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    b.push_back(static_cast<uint8_t>(v >> static_cast<unsigned>(s)));
  }
}
Bytes box(std::string_view type, const Bytes& payload) {
  Bytes b;
  put32(b, static_cast<uint32_t>(8 + payload.size()));
  b.insert(b.end(), type.begin(), type.end());
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}
Bytes cat(const std::vector<Bytes>& parts) {
  Bytes b;
  for (const Bytes& p : parts) {
    b.insert(b.end(), p.begin(), p.end());
  }
  return b;
}

// A sparse stand-in file; returns its path, or an empty string when it cannot be created.
std::string synthetic_file() {
  const Bytes table(static_cast<size_t>(kFrames) * 4U, 0);
  const Bytes stbl = box("stbl", cat({box("stsz", table), box("stco", table), box("stts", table), box("ctts", table)}));
  const Bytes moov = box("moov", cat({box("mvhd", Bytes(100, 0)), box("trak", box("mdia", box("minf", stbl)))}));
  const Bytes ftyp = box("ftyp", Bytes{'i', 's', 'o', 'm', 0, 0, 0, 0});
  const fs::path path = fs::temp_directory_path() / "ovt_bench_container_open_moov_1h.mp4";
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(ftyp.data()), static_cast<std::streamsize>(ftyp.size()));
    out.write(reinterpret_cast<const char*>(moov.data()), static_cast<std::streamsize>(moov.size()));
    Bytes mdat_header;  // largesize form: size 1, 'mdat', u64 total
    put32(mdat_header, 1);
    mdat_header.insert(mdat_header.end(), {'m', 'd', 'a', 't'});
    put32(mdat_header, static_cast<uint32_t>(kMdatBytes >> 32U));
    put32(mdat_header, static_cast<uint32_t>(kMdatBytes));
    out.write(reinterpret_cast<const char*>(mdat_header.data()), static_cast<std::streamsize>(mdat_header.size()));
    if (!out) {
      return {};
    }
  }
  std::error_code ec;
  fs::resize_file(path, ftyp.size() + moov.size() + kMdatBytes, ec);
  return ec ? std::string{} : path.string();
}

std::string input_path() {
  const char* dir = std::getenv("OVT_CORPORA_DIR");  // NOLINT(concurrency-mt-unsafe): read once, before any thread
  if (dir != nullptr) {
    const fs::path p = fs::path(dir) / "syn_h264_1h_1080p.mp4";
    if (fs::exists(p)) {
      return p.string();
    }
  }
  return synthetic_file();
}

void bench_container_open_moov_1h(benchmark::State& state) {
  static const std::string path = input_path();
  if (path.empty()) {
    state.SkipWithError("cannot create the sparse stand-in file");
    return;
  }
  ovt_File* file = nullptr;
  if (ovt_file_open(path.c_str(), &file) != OVT_OK) {
    state.SkipWithError("cannot open the input file");
    return;
  }
  for (auto _ : state) {  // NOLINT(clang-analyzer-deadcode.DeadStores): the idiom of Google Benchmark
    ovt_Container* c = nullptr;
    if (ovt_container_open(file, &c) != OVT_OK) {
      state.SkipWithError("ovt_container_open failed");
      break;
    }
    benchmark::DoNotOptimize(ovt_container_tree(c).size);
    ovt_container_close(c);
  }
  ovt_file_close(file);
}

}  // namespace

OVT_BENCHMARK(bench_container_open_moov_1h);

BENCHMARK_MAIN();
