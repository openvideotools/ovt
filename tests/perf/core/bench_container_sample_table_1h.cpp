// SPDX-FileCopyrightText: 2026 The Open Video Tools contributors
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// bench_container_sample_table_1h: ovt_container_sample_table of every sample (the `moov` expansion of §8.4.1 and the
// FlatBuffer) of a one-hour, `moov`-first MP4 (chapter 14 §14.9.3, chapter 08 §8.4.1, WI-1.218). The open is not timed.
// The input is the corpus file `syn_h264_1h_1080p.mp4` (recipe 27) when it is in $OVT_CORPORA_DIR; otherwise a sparse
// stand-in of the same shape: `ftyp`, a `moov` whose `stts`, `ctts`, `stsz`, `stsc`, `stco` and `stss` describe the
// recipe's 108 000 samples (30 per chunk, a sync sample every 30), and a 6 GiB `mdat` that is never written.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "ovt/container.h"
#include "ovt/io.h"
#include "ovt_perf.hpp"

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

constexpr uint32_t kFrames = 108000;  // recipe 27: one hour at 30 fps
constexpr uint32_t kPerChunk = 30;
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
// A FullBox (version 0, flags 0) over `words`.
Bytes table(std::string_view type, const std::vector<uint32_t>& words) {
  Bytes p;
  put32(p, 0);
  for (const uint32_t w : words) {
    put32(p, w);
  }
  return box(type, p);
}

Bytes stbl() {
  std::vector<uint32_t> sizes = {0, kFrames};  // sample_size 0, sample_count
  std::vector<uint32_t> chunks = {kFrames / kPerChunk};
  std::vector<uint32_t> sync = {kFrames / kPerChunk};
  for (uint32_t i = 0; i < kFrames; ++i) {
    sizes.push_back(20000 + (i * 7919U) % 30000U);
  }
  for (uint32_t c = 0; c < kFrames / kPerChunk; ++c) {
    chunks.push_back(48 + c * 1000000U);
    sync.push_back(c * kPerChunk + 1);
  }
  const Bytes stsd = table("stsd", {1, 8, 0x61766331, 0});  // one (empty) `avc1` entry
  return box("stbl", cat({stsd, table("stts", {1, kFrames, 1001}),
                          table("ctts", {2, kFrames / 2, 2002, kFrames / 2, 0}), table("stsc", {1, 1, kPerChunk, 1}),
                          table("stsz", sizes), table("stco", chunks), table("stss", sync)}));
}

// A sparse stand-in file; returns its path, or an empty string when it cannot be created.
std::string synthetic_file() {
  const Bytes moov = box("moov", box("trak", box("mdia", box("minf", stbl()))));
  const Bytes ftyp = box("ftyp", Bytes{'i', 's', 'o', 'm', 0, 0, 0, 0});
  const fs::path path = fs::temp_directory_path() / "ovt_bench_container_sample_table_1h.mp4";
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

void bench_container_sample_table_1h(benchmark::State& state) {
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
    state.PauseTiming();  // a fresh handle each time: the reader keeps the expanded table of a track
    ovt_Container* c = nullptr;
    if (ovt_container_open(file, &c) != OVT_OK) {
      state.SkipWithError("ovt_container_open failed");
      break;
    }
    state.ResumeTiming();
    ovt_Buffer table{};
    if (ovt_container_sample_table(c, 0, 0, 0, &table) != OVT_OK) {
      state.SkipWithError("ovt_container_sample_table failed");
      ovt_container_close(c);
      break;
    }
    benchmark::DoNotOptimize(table.size);
    state.PauseTiming();
    delete[] table.data;  // §12.3.1 rule 3a: caller-owned buffers are new[]; status.cpp is not linked here
    ovt_container_close(c);
    state.ResumeTiming();
  }
  ovt_file_close(file);
}

}  // namespace

OVT_BENCHMARK(bench_container_sample_table_1h);

BENCHMARK_MAIN();
